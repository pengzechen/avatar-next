/*
 * include/vmm_vgicc.h — 虚拟 GICv2 CPU 接口（GICC）软件模拟
 *
 * 为什么不像 kvmm 那样把 guest 的 GICC 直通到硬件 GICV：
 *   直通依赖 GICv2 虚拟化扩展由**硬件**完成虚拟中断投递（GICH LR →
 *   GICV → nIRQ）。但这套机制需要物理中断与虚拟中断严格配对（HW=1 LR），
 *   而宿主的中断入口是「ack + EOI + DIR」的通用流程、且 PPI 27 被标记为
 *   guest-owned 不能 DIR，物理中断会长期停在 active，硬件就不肯再投虚拟
 *   中断（实测 GICV_IAR 恒返回 1022 = "有挂起中断但不可投递"）。
 *
 * 所以这里把 GICC 完整收进 VMM：
 *   - guest 对 0x08010000 的访问全部陷入 MMIO 总线（stage-2 不再直通）
 *   - GICC_IAR/EOIR/DIR 驱动 vgic 的挂起/active 位图
 *   - 中断通过 HCR_EL2.VI 注入（vIRQ），guest 在 EL1 直接收到
 */
#ifndef VMM_VGICC_H
#define VMM_VGICC_H

#include "types.h"
#include "vmm_mmio.h"

/* guest 看到的 GICC 基址（DTB 里的 GIC 第二段 reg）*/
#define VGICC_BASE  0x08010000ULL

/* GICC 寄存器偏移（GICv2）*/
#define GICC_CTLR   0x0000
#define GICC_PMR    0x0004
#define GICC_IAR    0x000c
#define GICC_EOIR   0x0010
#define GICC_IIDR   0x00fc
#define GICC_DIR    0x1000

/*
 * vgicc_init — 初始化软件 GICC 并注册到 MMIO 总线
 * 返回 0 成功。
 */
int vgicc_init(mmio_device_t *dev, mmio_bus_t *bus);

#endif /* VMM_VGICC_H */
