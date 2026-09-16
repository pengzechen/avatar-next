/*
 * include/vmm_vgicc.h — per-vCPU virtual CPU interface / GICH state.
 *
 * guest visible GICC registers are MMIO-emulated here. The same per-vCPU
 * state also owns the cached GICH list registers used to signal virtual IRQs.
 */
#ifndef VMM_VGICC_H
#define VMM_VGICC_H

#include "types.h"
#include "vmm_mmio.h"
#include "vmm_vgic.h"

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
 * vgicc_init — 初始化软件 GICC/GICH state 并注册 GICC MMIO 设备
 * 返回 0 成功。
 */
int vgicc_init(mmio_device_t *dev, mmio_bus_t *bus, vgic_t *vgic);

/* GICH LR state helpers. vgic core decides what to inject; vgicc owns where. */
int  vgicc_lr_has_irq(vgic_t *vgic, uint32_t vcpu_id, uint32_t irq);
int  vgicc_lr_empty_slot(vgic_t *vgic, uint32_t vcpu_id);
void vgicc_write_lr(vgic_t *vgic, uint32_t vcpu_id, uint32_t slot,
                    uint32_t value);
void vgicc_clear_lr_irq(vgic_t *vgic, uint32_t vcpu_id, uint32_t irq);
void vgicc_save_state_from_hw(vgic_t *vgic, uint32_t vcpu_id);

#endif /* VMM_VGICC_H */
