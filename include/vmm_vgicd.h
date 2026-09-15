/*
 * include/vmm_vgicd.h — 虚拟 GICv2 分发器（GICD）模拟
 *
 * 移植自 x-kernel: virt/vkvmm/src/vdev/aarch64/vgicd.rs
 *
 * guest 通过 MMIO 配置 GICD（使能/优先级/目标 CPU/SGI）。本模块为这些
 * 访问提供一个可读回的模型：
 *   - 字节可寻址后备存储：保证 guest 探测 IPRIORITYR/ITARGETSR/ICFGR/CTLR
 *     时读回正确值（否则 guest 的「优先级位数」探测会读到 0 并卡死）
 *   - ISENABLER/ICENABLER → 使能位图
 *   - ISPENDR / SGIR → 置挂起位
 *   - ITARGETSR(0-31) banked 只读：guest 据此发现每 CPU 的 SGI 目标掩码
 *
 * 真实的宿主 GIC 分发器永不触碰。
 *
 * 注意：本模型只维护「挂起/使能」状态，尚未把待处理中断投递到 guest
 * （那需要 GICH 列表寄存器 / GICV 硬件辅助，见 vmm_vgic.h 的说明）。
 */
#ifndef VMM_VGICD_H
#define VMM_VGICD_H

#include "vmm_mmio.h"

/* QEMU virt GICv2 分发器基址/大小 */
#define VGICD_BASE   0x08000000ULL
#define VGICD_SIZE   0x10000ULL

/* 最大跟踪中断数 */
#define VGICD_MAX_IRQS   1024
#define VGICD_MAX_VCPUS  8

/* guest 虚拟定时器中断（PPI 27），对标 vgicd.rs 的 GUEST_VTIMER_IRQ */
#define VGICD_VTIMER_IRQ   27

/* vCPU 世界切换时处理挂起中断的回调（由 vgic 提供；可为 NULL）*/
typedef void (*vgicd_sync_fn)(uint32_t vcpu_id);

/*
 * vgicd_init — 初始化 GICD 模拟并注册到 MMIO 总线
 * @nr_vcpus: VM 的 vCPU 数量（影响 GICD_TYPER 与 ITARGETSR 读数）
 * 返回 0 成功。
 */
int vgicd_init(mmio_device_t *dev, mmio_bus_t *bus, uint32_t nr_vcpus);

/* 使某个中断挂起（设备/定时器注入入口）*/
void vgicd_set_pending(uint32_t vcpu_id, uint32_t irq);

/* 查询某中断是否使能（供投递路径使用）*/
int  vgicd_is_enabled(uint32_t vcpu_id, uint32_t irq);

#endif /* VMM_VGICD_H */
