/*
 * kernel/vmm/vdev/vgic/vgic.c - VM-level virtual GICv2 core.
 */

#include "vmm_vgic.h"
#include "vmm_vgicc.h"
#include "vmm_irq_route.h"
#include "klog.h"
#include "string.h"

#define SGI_MASK 0xffffu

static int valid_vcpu(const vgic_t *vgic, uint32_t vcpu_id)
{
    return vgic && vcpu_id < vgic->nr_vcpus;
}

static int valid_irq(uint32_t irq)
{
    return irq < 1020;
}

static uint32_t irq_bit(uint32_t irq)
{
    return 1u << (irq & 31u);
}

static uint32_t lowest_irq(uint32_t bits, uint32_t base)
{
    uint32_t bit = bits & (~bits + 1u);
    uint32_t off = 0;

    while (bit > 1u) {
        bit >>= 1;
        off++;
    }
    return base + off;
}

static uint32_t lr_value(uint32_t irq, uint32_t source_vcpu)
{
    uint32_t lr = LR_GROUP1 | LR_STATE_PENDING | LR_PRIORITY |
                  (irq & LR_VINTID_MASK);

    if (irq < 16)
        lr |= (source_vcpu & 0x7u) << LR_SGI_SRC_SHIFT;
    return lr;
}

int vmm_vgic_init(vgic_t *vgic, uint32_t nr_vcpus)
{
    if (!vgic)
        return -1;
    if (nr_vcpus < 1)
        nr_vcpus = 1;
    if (nr_vcpus > VGIC_MAX_VCPUS)
        nr_vcpus = VGIC_MAX_VCPUS;

    memset(vgic, 0, sizeof(*vgic));
    vgic->nr_vcpus = nr_vcpus;
    for (uint32_t i = 0; i < VGIC_MAX_VCPUS; i++)
        vgic->vcpu[i].enabled0 = SGI_MASK;

    KLOG_INFO("[vgic] VM vGIC ready (%u vCPU, %u IRQs, %u LR)\n",
              nr_vcpus, (unsigned)VGIC_MAX_IRQS, (unsigned)VGIC_MAX_LRS);
    return 0;
}

void vmm_vgic_set_dist_enabled(vgic_t *vgic, int enabled)
{
    if (vgic)
        vgic->dist_enabled = enabled ? 1 : 0;
}

int vmm_vgic_dist_enabled(const vgic_t *vgic)
{
    return vgic ? vgic->dist_enabled : 0;
}

void vmm_vgic_set_sgi_pending(vgic_t *vgic, uint32_t vcpu_id,
                              uint32_t source_vcpu, uint32_t irq)
{
    if (!valid_vcpu(vgic, vcpu_id) || irq >= 16)
        return;
    if (source_vcpu >= VGIC_MAX_VCPUS)
        source_vcpu = 0;

    vgic->vcpu[vcpu_id].sgi_sources[irq] |= (uint16_t)(1u << source_vcpu);
    vgic->vcpu[vcpu_id].pending0 |= irq_bit(irq);
}

void vmm_vgic_set_pending(vgic_t *vgic, uint32_t vcpu_id, uint32_t irq)
{
    if (!vgic || !valid_irq(irq))
        return;
    if (irq < 16) {
        vmm_vgic_set_sgi_pending(vgic, vcpu_id, 0, irq);
    } else if (irq < 32) {
        if (valid_vcpu(vgic, vcpu_id))
            vgic->vcpu[vcpu_id].pending0 |= irq_bit(irq);
    } else {
        if (valid_vcpu(vgic, vcpu_id))
            vgic->spi_pending[vcpu_id][irq / 32] |= irq_bit(irq);
    }
}

void vmm_vgic_set_enabled(vgic_t *vgic, uint32_t vcpu_id, uint32_t irq,
                          int enabled)
{
    if (!vgic || !valid_irq(irq) || irq < 16)
        return;
    if (irq < 32) {
        if (!valid_vcpu(vgic, vcpu_id))
            return;
        if (enabled)
            vgic->vcpu[vcpu_id].enabled0 |= irq_bit(irq);
        else
            vgic->vcpu[vcpu_id].enabled0 &= ~irq_bit(irq);
    } else if (enabled) {
        vgic->enabled[irq / 32] |= irq_bit(irq);
    } else {
        vgic->enabled[irq / 32] &= ~irq_bit(irq);
    }
}

uint32_t vmm_vgic_enabled_word(const vgic_t *vgic, uint32_t vcpu_id,
                               uint32_t word)
{
    if (!vgic || word >= VGIC_MAX_WORDS)
        return 0;
    if (word == 0)
        return valid_vcpu(vgic, vcpu_id) ?
               (vgic->vcpu[vcpu_id].enabled0 | SGI_MASK) : SGI_MASK;
    return vgic->enabled[word];
}

uint32_t vmm_vgic_pending_word(const vgic_t *vgic, uint32_t vcpu_id,
                               uint32_t word)
{
    if (!vgic || word >= VGIC_MAX_WORDS)
        return 0;
    if (word == 0)
        return valid_vcpu(vgic, vcpu_id) ? vgic->vcpu[vcpu_id].pending0 : 0;
    if (valid_vcpu(vgic, vcpu_id))
        return vgic->spi_pending[vcpu_id][word];
    return 0;
}

uint32_t vmm_vgic_active_word(const vgic_t *vgic, uint32_t vcpu_id,
                              uint32_t word)
{
    if (!vgic || word >= VGIC_MAX_WORDS)
        return 0;
    if (word == 0)
        return valid_vcpu(vgic, vcpu_id) ? vgic->vcpu[vcpu_id].active0 : 0;
    if (valid_vcpu(vgic, vcpu_id))
        return vgic->spi_active[vcpu_id][word];
    return 0;
}

void vmm_vgic_clear_pending_word(vgic_t *vgic, uint32_t vcpu_id,
                                 uint32_t word, uint32_t bits)
{
    if (!vgic || word >= VGIC_MAX_WORDS)
        return;
    if (word == 0) {
        if (valid_vcpu(vgic, vcpu_id))
            vgic->vcpu[vcpu_id].pending0 &= ~bits;
    } else {
        if (valid_vcpu(vgic, vcpu_id))
            vgic->spi_pending[vcpu_id][word] &= ~bits;
    }
}

void vmm_vgic_clear_active_word(vgic_t *vgic, uint32_t vcpu_id,
                                uint32_t word, uint32_t bits)
{
    if (!vgic || word >= VGIC_MAX_WORDS)
        return;
    if (word == 0) {
        if (valid_vcpu(vgic, vcpu_id))
            vgic->vcpu[vcpu_id].active0 &= ~bits;
    } else {
        if (valid_vcpu(vgic, vcpu_id))
            vgic->spi_active[vcpu_id][word] &= ~bits;
    }
}

int vmm_vgic_next_pending(const vgic_t *vgic, uint32_t vcpu_id)
{
    if (!valid_vcpu(vgic, vcpu_id))
        return -1;

    const vgic_vcpu_state_t *vcpu = &vgic->vcpu[vcpu_id];
    uint32_t ready0 = vcpu->pending0 & (vcpu->enabled0 | SGI_MASK) &
                      ~vcpu->active0;
    if (ready0)
        return (int)lowest_irq(ready0, 0);

    for (uint32_t word = 1; word < VGIC_MAX_WORDS; word++) {
        uint32_t ready = vgic->spi_pending[vcpu_id][word] &
                          vgic->enabled[word] &
                          ~vgic->spi_active[vcpu_id][word];
        if (ready)
            return (int)lowest_irq(ready, word * 32);
    }
    return -1;
}

int vmm_vgic_ack(vgic_t *vgic, uint32_t vcpu_id)
{
    int irq = vmm_vgic_next_pending(vgic, vcpu_id);
    if (irq < 0)
        return -1;

    if ((uint32_t)irq < 32) {
        vgic_vcpu_state_t *vcpu = &vgic->vcpu[vcpu_id];
        vcpu->pending0 &= ~irq_bit((uint32_t)irq);
        if ((uint32_t)irq < 16)
            vcpu->sgi_sources[irq] = 0;
        vcpu->active0 |= irq_bit((uint32_t)irq);
    } else {
        vgic->spi_pending[vcpu_id][(uint32_t)irq / 32] &=
            ~irq_bit((uint32_t)irq);
        vgic->spi_active[vcpu_id][(uint32_t)irq / 32] |=
            irq_bit((uint32_t)irq);
    }
    vgicc_clear_lr_irq(vgic, vcpu_id, (uint32_t)irq);
    return irq;
}

void vmm_vgic_eoi(vgic_t *vgic, uint32_t vcpu_id, uint32_t irq)
{
    if (!vgic || !valid_vcpu(vgic, vcpu_id) || !valid_irq(irq))
        return;
    if (irq < 32)
        vgic->vcpu[vcpu_id].active0 &= ~irq_bit(irq);
    else
        vgic->spi_active[vcpu_id][irq / 32] &= ~irq_bit(irq);
}

void vmm_vgic_inject_timer(vgic_t *vgic, uint32_t vcpu_id)
{
    vmm_vgic_set_pending(vgic, vcpu_id, HOST_VTIMER_IRQ);
    vmm_vgic_sync_entry(vgic, vcpu_id);
}

void vmm_vgic_sync_entry(vgic_t *vgic, uint32_t vcpu_id)
{
    if (!valid_vcpu(vgic, vcpu_id))
        return;

    vgic_vcpu_state_t *vcpu = &vgic->vcpu[vcpu_id];
    while (1) {
        int irq = vmm_vgic_next_pending(vgic, vcpu_id);
        if (irq < 0 || vgicc_lr_has_irq(vgic, vcpu_id, (uint32_t)irq))
            break;

        int slot = vgicc_lr_empty_slot(vgic, vcpu_id);
        if (slot < 0)
            break;

        uint32_t src = 0;
        if ((uint32_t)irq < 16) {
            uint16_t sources = vcpu->sgi_sources[irq];
            if (sources) {
                uint16_t bit = (uint16_t)(sources & (uint16_t)(~sources + 1u));
                while (bit > 1u) {
                    bit >>= 1;
                    src++;
                }
            }
        }
        vgicc_write_lr(vgic, vcpu_id, (uint32_t)slot,
                       lr_value((uint32_t)irq, src));
        break;
    }
}

void vmm_vgic_sync_exit(vgic_t *vgic, uint32_t vcpu_id)
{
    if (valid_vcpu(vgic, vcpu_id))
        vgicc_save_state_from_hw(vgic, vcpu_id);
}
