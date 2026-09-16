/*
 * include/vmm_vgic.h — 虚拟 GICv2 中断核心
 *
 * 移植自 x-kernel: virt/kvmm/src/vdev/aarch64/vgic.rs
 *
 * 职责：维护 GICD/GICC 共享的「挂起 / 使能 / 活动」状态，并把可投递
 * 的虚拟中断排入 GICH list registers。guest 的 GICC 访问仍由 VMM MMIO
 * 模拟，所以软件 ack/EOI 必须同步维护 LR 状态。
 */
#ifndef VMM_VGIC_H
#define VMM_VGIC_H

#include "types.h"

#define VGIC_MAX_IRQS   1024
#define VGIC_MAX_WORDS  (VGIC_MAX_IRQS / 32)
#define VGIC_MAX_VCPUS  8
#define VGIC_MAX_LRS    4

#define LR_HW              (1u << 31)
#define LR_GROUP1          (1u << 30)
#define LR_STATE_PENDING   (1u << 28)
#define LR_PRIORITY        (0x14u << 23)
#define LR_STATE_MASK      (0x3u << 28)
#define LR_PHYSID_SHIFT    10
#define LR_SGI_SRC_SHIFT   10
#define LR_VINTID_MASK     0x3ffu

/*
 * vmm_vgic_init — 初始化 vGIC（清空位图与 LR）
 * 返回 0。
 */
int vmm_vgic_init(uint32_t nr_vcpus);

/* 置某中断为挂起（设备/定时器注入入口）*/
void vmm_vgic_set_pending(uint32_t vcpu_id, uint32_t irq);

/* 置 SGI 挂起，同时记录 source vCPU。*/
void vmm_vgic_set_sgi_pending(uint32_t vcpu_id, uint32_t source_vcpu,
                              uint32_t irq);

/* 按分发器状态使能/禁用某中断 */
void vmm_vgic_set_enabled(uint32_t vcpu_id, uint32_t irq, int enabled);

/* 分发器全局开关（GICD_CTLR bit0）。*/
void vmm_vgic_set_dist_enabled(int enabled);
int  vmm_vgic_dist_enabled(void);

/* GICD 寄存器读回辅助。*/
uint32_t vmm_vgic_enabled_word(uint32_t vcpu_id, uint32_t word);
uint32_t vmm_vgic_pending_word(uint32_t vcpu_id, uint32_t word);
uint32_t vmm_vgic_active_word(uint32_t vcpu_id, uint32_t word);
void vmm_vgic_clear_pending_word(uint32_t vcpu_id, uint32_t word,
                                 uint32_t bits);
void vmm_vgic_clear_active_word(uint32_t vcpu_id, uint32_t word,
                                uint32_t bits);

/*
 * vmm_vgic_inject_timer — 宿主 PPI 27 ISR 专用：把虚拟定时器中断挂起。
 *
 * 供宿主 ISR 在 **guest 运行中** 直接调用；该函数会同步 GICH LR，guest
 * 随后通过虚拟 IRQ 信号进入自己的 GIC handler。
 */
void vmm_vgic_inject_timer(uint32_t vcpu_id);

/* 进入/退出 guest 或 EL2 IRQ 注入时，同步软件 pending 到 GICH LR。*/
void vmm_vgic_sync_entry(uint32_t vcpu_id);
void vmm_vgic_sync_exit(uint32_t vcpu_id);

/* 取下一个可投递（挂起且已使能）的中断号；无则返回 -1。不改状态。*/
int vmm_vgic_next_pending(uint32_t vcpu_id);

/* guest 读 GICC_IAR：取中断号并清挂起、置 active；无则可投递中断返回 -1 */
int vmm_vgic_ack(uint32_t vcpu_id);

/* guest 写 GICC_EOIR / GICC_DIR：清 active */
void vmm_vgic_eoi(uint32_t vcpu_id, uint32_t irq);

#endif /* VMM_VGIC_H */
