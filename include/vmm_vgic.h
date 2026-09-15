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

#endif /* VMM_VGIC_H */
