/*
 * include/vmm_vgicv3.h — 虚拟 GICv3（GICv3 vGIC）
 *
 * 与 GICv2 vGIC（vmm_vgic.h / vmm_vgicd.h / vmm_vgicc.h）的关系：
 *   中断生命周期状态机（pending / enabled / active 位图）沿用同一套模型，
 *   差异在「投递后端」和「guest 可见的寄存器」：
 *
 *     GICv2                                  GICv3
 *     ────────────────────────────────       ────────────────────────────────
 *     GICC MMIO（0x08010000）由 VMM 陷入    CPU interface 是 ICV_* 系统寄存器，
 *     软件模拟 IAR/EOIR                      由 GIC 硬件直接服务（VMM 无法陷入）
 *     GICH_LR<n>（32 位，MMIO）              ICH_LR<n>_EL2（64 位，系统寄存器）
 *     SGI/PPI 在 GICD（banked）              SGI/PPI 在 GICR（0x080A0000/核）
 *
 * 因此 GICv3 下 VMM 不再「替 guest 应答中断」，而是：
 *   进入 guest 前  sync_entry：把 pending&enabled&!active 的中断写进空 LR
 *   guest 运行中   硬件负责 ack/EOI，并自动改 LR 的 State 字段
 *   退出 guest 后  sync_exit ：读 ICH_ELRSR_EL2 / LR.State 回推软件状态
 *
 * 注意：HCR_EL2.IMO=1 是前提 —— guest 在 EL1 访问 ICC_*_EL1 才会被重定向到
 * ICV_*（虚拟 CPU interface），否则 guest 会直接操作物理 GIC。
 */
#ifndef VMM_VGICV3_H
#define VMM_VGICV3_H

#include "types.h"
#include "vmm_mmio.h"
#include "vmm_virq.h"   /* VIRQ_VTIMER */

/* ── 容量 ────────────────────────────────────────────────────── */
#define VGIC3_MAX_IRQS   1024
#define VGIC3_MAX_WORDS  (VGIC3_MAX_IRQS / 32)
#define VGIC3_MAX_VCPUS  8
#define VGIC3_MAX_LRS    16
#define VGIC3D_REG_SIZE  0x10000   /* GICD 窗口 64KB */
#define VGIC3R_REG_SIZE  0x20000   /* 每核 Redistributor：RD 64KB + SGI 64KB */

/* ── guest 看到的地址（必须与 imgs/guests/aarch64/linux-gicv3.dts 一致）── */
#define VGIC3D_BASE      0x08000000ULL
#define VGIC3D_SIZE      0x10000ULL
#define VGIC3R_BASE      0x080A0000ULL
#define VGIC3R_STRIDE    0x20000ULL
#define VGIC3R_SGI_OFF   0x10000ULL

/* guest 虚拟定时器 PPI 27 —— 统一用 VIRQ_VTIMER（见 include/vmm/vmm_virq.h）*/

/* 默认优先级：与 Linux 给 PPI/SGI 设的 0xA0 一致 */
#define VGIC3_DEFAULT_PRIO 0xA0

/* ── 每 vCPU 状态 ───────────────────────────────────────────── */
typedef struct vgic3_vcpu {
    /* INTID 0..31：banked 的 SGI/PPI 状态 */
    uint32_t enabled0;
    uint32_t pending0;
    uint32_t active0;
    uint16_t sgi_sources[16];      /* 每 SGI 的源核位图 */
    uint8_t  prio0[32];            /* GICR_IPRIORITYR0..7 */

    /* Redistributor RD_base 帧里需要读回的字 */
    uint32_t rd_ctlr;
    uint32_t rd_waker;             /* 只跟踪 ProcessorSleep */
    uint64_t rd_propbaser;
    uint64_t rd_pendbaser;

    /* GICR SGI 帧里需要读回的字 */
    uint32_t sgi_igroupr0;
    uint32_t sgi_igrpmodr0;
    uint32_t sgi_icfgr[2];

    /* 硬件 LR 槽位的镜像：0 = 空，否则为我们写进去的 LR 值 */
    uint64_t lr[VGIC3_MAX_LRS];
} vgic3_vcpu_t;

typedef struct vgic3 {
    uint32_t nr_vcpus;
    int      dist_enabled;

    /*
     * ── per-CPU 硬件寄存器 ICH_*_EL2 的软件镜像 ──────────────────
     *
     * ICH_VMCR_EL2 / ICH_AP1R*_EL2 是**每 CPU 一份**的硬件。VHE 下 guest
     * 读写的 ICC_PMR_EL1 / ICC_CTLR_EL1 / ICC_BPR1_EL1 / ICC_IGRPEN1_EL1 /
     * ICC_AP1R*_EL1 全部落在它们上面 —— 而多个 VM 的 vCPU 可以在同一颗核上
     * 分时跑，不保存/恢复就会互相覆盖。
     *
     * 症状（实测）：第二个 VM 的 guest 内核完整启动到
     * "Freeing unused kernel memory"，之后 init 再也推进不了；宿主侧看到
     * vtimer 注入计数一路涨、而 guest 的 CNTV_CVAL 恒定不变 —— 也就是
     * 「我们在注入，guest 收不到」。原因是它的 vGIC 配置被前一个 VM 的覆盖，
     * 中断被 PMR/IGRPEN1 挡住了。
     *
     * 切换 VMC R/AP1R 的动作在 vmm_vgic3_lr_switch_in() 里（和 LR 同一处）。
     */
    uint64_t ich_vmcr;
    uint64_t ich_ap1r[4];

    /* Distributor 侧（SPI，INTID >= 32）*/
    uint32_t enabled[VGIC3_MAX_WORDS];
    uint32_t spi_pending[VGIC3_MAX_VCPUS][VGIC3_MAX_WORDS];
    uint32_t spi_active[VGIC3_MAX_VCPUS][VGIC3_MAX_WORDS];

    /* GICD 字节可寻址后备存储：保证 guest 探测 IGROUPR/IPRIORITYR/
     * ICFGR/PIDR… 时读回合理值 */
    uint8_t  dist_regs[VGIC3D_REG_SIZE];

    vgic3_vcpu_t vcpu[VGIC3_MAX_VCPUS];
} vgic3_t;

/* ── 核心状态机 ─────────────────────────────────────────────── */
int  vmm_vgic3_init(vgic3_t *vgic, uint32_t nr_vcpus);

void vmm_vgic3_set_pending(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq);
void vmm_vgic3_set_sgi_pending(vgic3_t *vgic, uint32_t vcpu_id,
                               uint32_t source_vcpu, uint32_t irq);
void vmm_vgic3_set_enabled(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq,
                           int enabled);

void vmm_vgic3_set_dist_enabled(vgic3_t *vgic, int enabled);
int  vmm_vgic3_dist_enabled(const vgic3_t *vgic);

uint32_t vmm_vgic3_enabled_word(const vgic3_t *vgic, uint32_t vcpu_id,
                                uint32_t word);
uint32_t vmm_vgic3_pending_word(const vgic3_t *vgic, uint32_t vcpu_id,
                                uint32_t word);
uint32_t vmm_vgic3_active_word(const vgic3_t *vgic, uint32_t vcpu_id,
                               uint32_t word);
void vmm_vgic3_clear_pending_word(vgic3_t *vgic, uint32_t vcpu_id,
                                  uint32_t word, uint32_t bits);
void vmm_vgic3_clear_active_word(vgic3_t *vgic, uint32_t vcpu_id,
                                 uint32_t word, uint32_t bits);

/* ── 硬件（ICH_*_EL2）侧 ────────────────────────────────────── */
void vmm_vgic3_hw_init(void);
/*
 * vmm_vgic3_lr_switch_in — 进入 guest 前调用：确保**本 pCPU** 的
 * ICH_LR<n>_EL2 属于给定的 vCPU，再把可投递中断排进去。
 *
 * LR 是 per-pCPU 硬件，多个 VM 的 vCPU 在同一核上分时跑时会互相看到对方
 * 的残留中断。本函数处理归属切换（先回收上一个 VM 的状态、再清空硬件、
 * 最后排入当前 VM 的），替代直接调用 vmm_vgic3_sync_entry()。
 */
void vmm_vgic3_lr_switch_in(vgic3_t *vgic, uint32_t vcpu_id);

void vmm_vgic3_sync_entry(vgic3_t *vgic, uint32_t vcpu_id);
void vmm_vgic3_sync_exit(vgic3_t *vgic, uint32_t vcpu_id);
void vmm_vgic3_inject_timer(vgic3_t *vgic, uint32_t vcpu_id);

/* ── MMIO 虚拟设备 ──────────────────────────────────────────── */
int vgic3d_init(mmio_device_t *dev, mmio_bus_t *bus, vgic3_t *vgic);
int vgic3r_init(mmio_device_t *dev, mmio_bus_t *bus, vgic3_t *vgic);

#endif /* VMM_VGICV3_H */
