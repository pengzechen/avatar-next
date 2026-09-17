/*
 * kernel/vmm/vdev/vgic/vgicd.c - VM-owned virtual GICv2 distributor.
 */

#include "vmm/vmm_vgicd.h"
#include "vmm/vmm_irq_route.h"
#include "klog.h"
#include "string.h"

#define GICD_CTLR        0x000
#define GICD_TYPER       0x004
#define GICD_IIDR        0x008
#define GICD_ISENABLER   0x100
#define GICD_ICENABLER   0x180
#define GICD_ISPENDR     0x200
#define GICD_ICPENDR     0x280
#define GICD_ISACTIVER   0x300
#define GICD_ICACTIVER   0x380
#define GICD_IPRIORITYR  0x400
#define GICD_ITARGETSR   0x800
#define GICD_ICFGR       0xc00
#define GICD_SGIR        0xf00
#define ENABLE_WORDS     32

static uint64_t reg_read(const vgic_t *vgic, uint32_t off, uint8_t size)
{
    uint64_t value = 0;
    for (uint32_t i = 0; i < size; i++)
        if (off + i < VGICD_REG_SIZE)
            value |= (uint64_t)vgic->dist_regs[off + i] << (8 * i);
    return value;
}

static void reg_write(vgic_t *vgic, uint32_t off, uint8_t size,
                      uint64_t value)
{
    for (uint32_t i = 0; i < size; i++)
        if (off + i < VGICD_REG_SIZE)
            vgic->dist_regs[off + i] = (uint8_t)(value >> (8 * i));
}

static int word_index(uint64_t off, uint32_t base, uint32_t *index)
{
    if (off < base || off >= base + ENABLE_WORDS * 4u)
        return 0;
    *index = (uint32_t)((off - base) / 4);
    return 1;
}

void vgicd_set_pending(vgic_t *vgic, uint32_t vcpu_id, uint32_t irq)
{
    vmm_vgic_set_pending(vgic, vcpu_id, irq);
}

int vgicd_is_enabled(const vgic_t *vgic, uint32_t vcpu_id, uint32_t irq)
{
    if (!vgic || irq >= VGICD_MAX_IRQS)
        return 0;
    return (vmm_vgic_enabled_word(vgic, vcpu_id, irq / 32) &
            (1u << (irq & 31))) != 0;
}

static void handle_sgir(vgic_t *vgic, uint32_t value, uint32_t source_vcpu)
{
    uint32_t irq = value & 0xf;
    uint32_t filter = (value >> 24) & 0x3;
    uint32_t mask;

    switch (filter) {
    case 0:
        mask = (value >> 16) & 0xff;
        break;
    case 1:
        mask = ((1u << vgic->nr_vcpus) - 1) & ~(1u << source_vcpu);
        break;
    case 2:
        mask = 1u << source_vcpu;
        break;
    default:
        mask = 0;
        break;
    }

    for (uint32_t cpu = 0; cpu < vgic->nr_vcpus && cpu < 32; cpu++)
        if (mask & (1u << cpu))
            vmm_vgic_set_sgi_pending(vgic, cpu, source_vcpu, irq);
}

static uint64_t vgicd_read_for_vcpu(mmio_device_t *dev, uint64_t off,
                                    uint8_t size, uint32_t vcpu_id)
{
    vgic_t *vgic = (vgic_t *)dev->priv;
    uint32_t word;
    if (!vgic)
        return 0;

    if (off == GICD_CTLR)
        return vmm_vgic_dist_enabled(vgic) ? 1 : 0;
    if (off == GICD_TYPER)
        return (((uint64_t)(vgic->nr_vcpus - 1) & 0x7) << 5) | 0x3;
    if (off == GICD_IIDR)
        return 0;
    if (word_index(off, GICD_ISENABLER, &word) ||
        word_index(off, GICD_ICENABLER, &word))
        return vmm_vgic_enabled_word(vgic, vcpu_id, word);
    if (word_index(off, GICD_ISPENDR, &word) ||
        word_index(off, GICD_ICPENDR, &word))
        return vmm_vgic_pending_word(vgic, vcpu_id, word);
    if (word_index(off, GICD_ISACTIVER, &word) ||
        word_index(off, GICD_ICACTIVER, &word))
        return vmm_vgic_active_word(vgic, vcpu_id, word);

    if (off >= GICD_ITARGETSR && off < GICD_ITARGETSR + 0x20) {
        uint32_t mask = 1u << (vcpu_id < 8 ? vcpu_id : 0);
        return mask | (mask << 8) | (mask << 16) | (mask << 24);
    }
    return reg_read(vgic, (uint32_t)off, size);
}

static void vgicd_write_for_vcpu(mmio_device_t *dev, uint64_t off,
                                 uint8_t size, uint64_t value,
                                 uint32_t vcpu_id)
{
    vgic_t *vgic = (vgic_t *)dev->priv;
    uint32_t word;
    uint32_t value32 = (uint32_t)value;
    if (!vgic)
        return;

    if (off == GICD_CTLR) {
        vmm_vgic_set_dist_enabled(vgic, (value32 & 1u) != 0);
        reg_write(vgic, (uint32_t)off, size, value);
        return;
    }
    if (word_index(off, GICD_ISENABLER, &word)) {
        for (uint32_t bit = 0; bit < 32; bit++)
            if (value32 & (1u << bit))
                vmm_vgic_set_enabled(vgic, vcpu_id, word * 32 + bit, 1);
        if (word == 0 && (value32 & (1u << VGICD_VTIMER_IRQ)))
            vmm_irq_route_set_vtimer_enabled(1);
        return;
    }
    if (word_index(off, GICD_ICENABLER, &word)) {
        for (uint32_t bit = 0; bit < 32; bit++)
            if (value32 & (1u << bit))
                vmm_vgic_set_enabled(vgic, vcpu_id, word * 32 + bit, 0);
        if (word == 0 && (value32 & (1u << VGICD_VTIMER_IRQ)))
            vmm_irq_route_set_vtimer_enabled(0);
        return;
    }
    if (word_index(off, GICD_ISPENDR, &word)) {
        for (uint32_t bit = 0; bit < 32; bit++) {
            if (!(value32 & (1u << bit)))
                continue;
            uint32_t irq = word * 32 + bit;
            uint32_t mask = irq < 32 ? (1u << vcpu_id) : vgic->targets[irq];
            if (!mask)
                mask = 1;
            for (uint32_t cpu = 0; cpu < vgic->nr_vcpus && cpu < 32; cpu++)
                if (mask & (1u << cpu))
                    vgicd_set_pending(vgic, cpu, irq);
        }
        return;
    }
    if (word_index(off, GICD_ICPENDR, &word)) {
        vmm_vgic_clear_pending_word(vgic, vcpu_id, word, value32);
        return;
    }
    if (word_index(off, GICD_ICACTIVER, &word)) {
        vmm_vgic_clear_active_word(vgic, vcpu_id, word, value32);
        return;
    }
    if (off == GICD_SGIR) {
        handle_sgir(vgic, value32, vcpu_id);
        return;
    }
    if (off >= GICD_ITARGETSR && off < GICD_ITARGETSR + 0x20)
        return;
    if (off >= GICD_ITARGETSR && off < GICD_ITARGETSR + VGICD_MAX_IRQS) {
        uint32_t index = (uint32_t)(off - GICD_ITARGETSR);
        for (uint32_t i = 0; i < size && index + i < VGICD_MAX_IRQS; i++)
            vgic->targets[index + i] = (uint8_t)(value >> (8 * i));
    }
    reg_write(vgic, (uint32_t)off, size, value);
}

static const mmio_dev_ops_t g_vgicd_ops = {
    .name = "vgicd",
    .base = VGICD_BASE,
    .size = VGICD_SIZE,
    .read = NULL,
    .write = NULL,
    .read_for_vcpu = vgicd_read_for_vcpu,
    .write_for_vcpu = vgicd_write_for_vcpu,
};

int vgicd_init(mmio_device_t *dev, mmio_bus_t *bus, vgic_t *vgic)
{
    if (!dev || !bus || !vgic)
        return -1;
    dev->ops = &g_vgicd_ops;
    dev->priv = vgic;
    if (mmio_bus_register(bus, dev) != 0)
        return -1;
    KLOG_INFO("[vgicd] VM distributor ready (%u vCPU)\n", vgic->nr_vcpus);
    return 0;
}
