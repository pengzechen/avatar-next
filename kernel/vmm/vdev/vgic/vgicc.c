/*
 * kernel/vmm/vdev/vgic/vgicc.c - per-vCPU GICC and GICH state.
 */

#include "vmm_vgicc.h"
#include "klog.h"
#include "string.h"

uint32_t gic_read_lr(int32_t n);
void gic_write_lr(int32_t n, uint32_t mask);

#define GICC_IIDR_VALUE  0x0202043B
#define GICC_IAR_SPURIOUS 1023

static int valid_vcpu(const vgic_t *vgic, uint32_t vcpu_id)
{
    return vgic && vcpu_id < vgic->nr_vcpus;
}

static uint64_t vgicc_read_for_vcpu(mmio_device_t *dev, uint64_t off,
                                    uint8_t size, uint32_t vcpu_id)
{
    (void)size;
    vgic_t *vgic = (vgic_t *)dev->priv;
    if (!valid_vcpu(vgic, vcpu_id))
        return 0;

    vgic_vcpu_state_t *vcpu = &vgic->vcpu[vcpu_id];
    switch (off) {
    case GICC_CTLR:
        return vcpu->gicc_ctlr;
    case GICC_PMR:
        return vcpu->gicc_pmr;
    case GICC_IIDR:
        return vcpu->gicc_iidr;
    case GICC_IAR: {
        int irq = vmm_vgic_ack(vgic, vcpu_id);
        return irq < 0 ? GICC_IAR_SPURIOUS : (uint64_t)irq;
    }
    default:
        return 0;
    }
}

static void vgicc_write_for_vcpu(mmio_device_t *dev, uint64_t off,
                                 uint8_t size, uint64_t value,
                                 uint32_t vcpu_id)
{
    (void)size;
    vgic_t *vgic = (vgic_t *)dev->priv;
    if (!valid_vcpu(vgic, vcpu_id))
        return;

    vgic_vcpu_state_t *vcpu = &vgic->vcpu[vcpu_id];
    switch (off) {
    case GICC_CTLR:
        vcpu->gicc_ctlr = (uint32_t)value;
        vcpu->gicc_eoi_mode = (uint32_t)(value >> 9) & 1u;
        break;
    case GICC_PMR:
        vcpu->gicc_pmr = (uint32_t)value;
        break;
    case GICC_EOIR:
        if (!vcpu->gicc_eoi_mode)
            vmm_vgic_eoi(vgic, vcpu_id, (uint32_t)value & 0x3ffu);
        break;
    case GICC_DIR:
        vmm_vgic_eoi(vgic, vcpu_id, (uint32_t)value & 0x3ffu);
        break;
    default:
        break;
    }
}

static const mmio_dev_ops_t g_vgicc_ops = {
    .name           = "vgicc",
    .base           = VGICC_BASE,
    .size           = 0x10000,
    .read           = NULL,
    .write          = NULL,
    .read_for_vcpu  = vgicc_read_for_vcpu,
    .write_for_vcpu = vgicc_write_for_vcpu,
};

int vgicc_init(mmio_device_t *dev, mmio_bus_t *bus, vgic_t *vgic)
{
    if (!dev || !bus || !vgic)
        return -1;

    for (uint32_t i = 0; i < vgic->nr_vcpus; i++) {
        vgic->vcpu[i].gicc_iidr = GICC_IIDR_VALUE;
        vgic->vcpu[i].gicc_pmr = 0xf0;
    }

    dev->ops = &g_vgicc_ops;
    dev->priv = vgic;
    if (mmio_bus_register(bus, dev) != 0)
        return -1;

    KLOG_INFO("[vgicc] VM CPU interface ready (%u vCPU, IPA 0x%llx, %u LR)\n",
              vgic->nr_vcpus, (unsigned long long)VGICC_BASE,
              (unsigned)VGIC_MAX_LRS);
    return 0;
}

int vgicc_lr_has_irq(vgic_t *vgic, uint32_t vcpu_id, uint32_t irq)
{
    if (!valid_vcpu(vgic, vcpu_id))
        return 0;
    vgic_vcpu_state_t *vcpu = &vgic->vcpu[vcpu_id];
    for (uint32_t i = 0; i < VGIC_MAX_LRS; i++)
        if ((vcpu->lr[i] & LR_STATE_MASK) &&
            ((vcpu->lr[i] & LR_VINTID_MASK) == (irq & LR_VINTID_MASK)))
            return 1;
    return 0;
}

int vgicc_lr_empty_slot(vgic_t *vgic, uint32_t vcpu_id)
{
    if (!valid_vcpu(vgic, vcpu_id))
        return -1;
    for (uint32_t i = 0; i < VGIC_MAX_LRS; i++)
        if ((vgic->vcpu[vcpu_id].lr[i] & LR_STATE_MASK) == 0)
            return (int)i;
    return -1;
}

void vgicc_write_lr(vgic_t *vgic, uint32_t vcpu_id, uint32_t slot,
                    uint32_t value)
{
    if (!valid_vcpu(vgic, vcpu_id) || slot >= VGIC_MAX_LRS)
        return;
    vgic->vcpu[vcpu_id].lr[slot] = value;
    gic_write_lr((int32_t)slot, value);
}

void vgicc_clear_lr_irq(vgic_t *vgic, uint32_t vcpu_id, uint32_t irq)
{
    if (!valid_vcpu(vgic, vcpu_id))
        return;
    for (uint32_t i = 0; i < VGIC_MAX_LRS; i++) {
        uint32_t *lr = &vgic->vcpu[vcpu_id].lr[i];
        if ((*lr & LR_STATE_MASK) &&
            ((*lr & LR_VINTID_MASK) == (irq & LR_VINTID_MASK))) {
            *lr = 0;
            gic_write_lr((int32_t)i, 0);
        }
    }
}

void vgicc_save_state_from_hw(vgic_t *vgic, uint32_t vcpu_id)
{
    if (!valid_vcpu(vgic, vcpu_id))
        return;
    for (uint32_t i = 0; i < VGIC_MAX_LRS; i++) {
        uint32_t value = gic_read_lr((int32_t)i);
        vgic->vcpu[vcpu_id].lr[i] =
            (value & LR_STATE_MASK) ? value : 0;
        gic_write_lr((int32_t)i, 0);
    }
}
