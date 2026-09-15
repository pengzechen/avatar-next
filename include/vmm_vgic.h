/*
 * include/vmm_vgic.h — 虚拟 GICv2 中断注入核心（GICH 列表寄存器模型）
 *
 * 移植自 x-kernel: virt/kvmm/src/vdev/aarch64/vgic.rs
 *
 * 职责：维护「挂起 / 使能 / 活动」位图与每 SGI 源状态，并把可投递的
 * 挂起中断整理进 GICH 列表寄存器（LR），在 guest 退出时回读 LR 状态。
 * 整个同步过程发生在 IRQ 关闭的世界切换窗口内。
 *
 * guest 的 GIC **CPU 接口**（GICC）在 kvmm 中是映射到 Stage-2 的硬件 GICV，
 * 因此 ack/EOI 由硬件辅助；本模块只负责分发器侧（见 vmm_vgicd.h）与
 * 列表寄存器注入。
 *
 * ⚠️ 当前实现状态：位图与 LR 整理逻辑已完整移植；但**写入真实 GICH 硬件
 *    寄存器需要一个映射到内核地址空间的 GICH MMIO 窗口**（kvmm 用
 *    `gich_va` 传入）。avatar 目前未映射 GICH，故 `vmm_vgic_sync_entry()`
 *    只做软件侧整理并记录日志，实际的 GICH 写由后续补上映射后启用
 *    （见 vmm_vgic_set_gich_base）。这样在没有运行时验证的环境里不会
 *    误写非法地址。
 */
#ifndef VMM_VGIC_H
#define VMM_VGIC_H

#include "types.h"

/* GICH（hypervisor control）寄存器偏移 —— 对标 vgic.rs */
#define GICH_HCR     0x000
#define GICH_VMCR    0x008
#define GICH_ELSR0   0x030
#define GICH_APR     0x0f0
#define GICH_LR0     0x100

#define VGIC_MAX_LRS   4
#define VGIC_MAX_IRQS  64

/* GICH_HCR.EN */
#define GICH_HCR_EN    1u

/* GICH_LR 位域（GICv2，32 位）*/
#define LR_HW              (1u << 31)
/*
 * LR.Group（bit30）：0 = Group 0，1 = Group 1。
 *
 * QEMU virt 的 GICv2 没有安全扩展（GICD_TYPER.SecurityExtn=0），这种 GIC 上
 * 只有 Group 1：LR 的 Group 位不置 1，硬件不认为该虚拟中断需要投递，guest
 * 永远收不到（现象是 GICV_IAR 一直读回 1023，guest 卡在 calibrate_delay()
 * 等 jiffy 推进）。
 */
#define LR_GROUP1          (1u << 30)
#define LR_STATE_PENDING   (1u << 28)
#define LR_PRIORITY        (0x14u << 23)
#define LR_STATE_MASK      (0x3u << 28)
#define LR_PHYSID_SHIFT    10
#define LR_SGI_SRC_SHIFT   10
#define LR_VINTID_MASK     0x3ffu

#define VGIC_MAX_VCPUS  8

/*
 * vmm_vgic_init — 初始化 vGIC（清空位图与 LR）
 * 返回 0。
 */
int  vmm_vgic_init(uint32_t nr_vcpus);

/*
 * vmm_vgic_set_gich_base — 设置 GICH 寄存器窗口的内核虚拟地址
 *
 * 传入 0 表示「未映射」：同步函数只做软件侧整理，不写硬件。
 */
void vmm_vgic_set_gich_base(uintptr_t gich_va);

/* 置某中断为挂起（设备/定时器注入入口）*/
void vmm_vgic_set_pending(uint32_t vcpu_id, uint32_t irq);

/* 按分发器状态使能/禁用某中断 */
void vmm_vgic_set_enabled(uint32_t vcpu_id, uint32_t irq, int enabled);

/* 进入 guest 前：把可投递挂起中断整理进 LR 并（若已映射）写入 GICH */
void vmm_vgic_sync_entry(uint32_t vcpu_id);

/* 退出 guest 后：回读 LR 状态并（若已映射）清空 GICH */
void vmm_vgic_sync_exit(uint32_t vcpu_id);

/*
 * vmm_vgic_inject_timer — 宿主 PPI 27 ISR 专用：把虚拟定时器中断挂起。
 *
 * 供宿主 ISR 在 **guest 运行中** 直接调用；调用方随后置 HCR_EL2.VI，
 * guest 就会在 EL1 上收到 vIRQ（见 vmm_arch_update_vi）。
 */
void vmm_vgic_inject_timer(uint32_t vcpu_id);

/* ── 软件 GICC（GICV 由 VMM 模拟，见 vmm_vgicc.c）───────────────── */

/* 取下一个可投递（挂起且已使能）的中断号；无则返回 -1。不改状态。*/
int vmm_vgic_next_pending(uint32_t vcpu_id);

/* guest 读 GICC_IAR：取中断号并清挂起、置 active；无则可投递中断返回 -1 */
int vmm_vgic_ack(uint32_t vcpu_id);

/* guest 写 GICC_EOIR / GICC_DIR：清 active */
void vmm_vgic_eoi(uint32_t vcpu_id, uint32_t irq);

#endif /* VMM_VGIC_H */
