/*
 * kernel/vmm/vdev/vgic/vgic.c - GICv2 virtual interrupt core.
 *
 * This is the VM-level vGIC device: it owns virtual interrupt lifecycle state
 * and coordinates the distributor and per-vCPU CPU-interface/GICH layers.
 */

#include "vmm_vgic.h"
#include "vmm_vgicc.h"
#include "vmm_irq_route.h"
#include "klog.h"
#include "string.h"

#define SGI_MASK 0xffffu

typedef struct {
    uint32_t enabled0;
    uint32_t pending0;
    uint32_t active0;
    uint16_t sgi_sources[16];
} vgic_vcpu_t;

static vgic_vcpu_t g_vcpus[VGIC_MAX_VCPUS];
static uint32_t g_enabled[VGIC_MAX_WORDS];
static uint32_t g_pending[VGIC_MAX_WORDS];
static uint32_t g_active[VGIC_MAX_WORDS];
static uint32_t g_nr_vcpus;
static int g_dist_enabled;

static int valid_vcpu(uint32_t vcpu_id)
{
    return vcpu_id < g_nr_vcpus;
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
    uint32_t lr = LR_GROUP1 | LR_STATE_PENDING | LR_PRIORITY | (irq & LR_VINTID_MASK);

    if (irq < 16)
        lr |= (source_vcpu & 0x7u) << LR_SGI_SRC_SHIFT;

    return lr;
}

int vmm_vgic_init(uint32_t nr_vcpus)
{
    if (nr_vcpus < 1)
        nr_vcpus = 1;
    if (nr_vcpus > VGIC_MAX_VCPUS)
        nr_vcpus = VGIC_MAX_VCPUS;

    memset(g_vcpus, 0, sizeof(g_vcpus));
    memset(g_enabled, 0, sizeof(g_enabled));
    memset(g_pending, 0, sizeof(g_pending));
    memset(g_active, 0, sizeof(g_active));

    g_nr_vcpus = nr_vcpus;
    g_dist_enabled = 0;

    for (uint32_t i = 0; i < VGIC_MAX_VCPUS; i++)
        g_vcpus[i].enabled0 = SGI_MASK;

    KLOG_INFO("[vgic] GICv2 core ready (%u vCPU, %u IRQs, %u LR)\n",
              nr_vcpus, (unsigned)VGIC_MAX_IRQS, (unsigned)VGIC_MAX_LRS);
    return 0;
}

void vmm_vgic_set_dist_enabled(int enabled)
{
    g_dist_enabled = enabled ? 1 : 0;
}

int vmm_vgic_dist_enabled(void)
{
    return g_dist_enabled;
}

void vmm_vgic_set_sgi_pending(uint32_t vcpu_id, uint32_t source_vcpu,
                              uint32_t irq)
{
    if (!valid_vcpu(vcpu_id) || irq >= 16)
        return;
    if (source_vcpu >= VGIC_MAX_VCPUS)
        source_vcpu = 0;

    g_vcpus[vcpu_id].sgi_sources[irq] |= (uint16_t)(1u << source_vcpu);
    g_vcpus[vcpu_id].pending0 |= irq_bit(irq);
}

void vmm_vgic_set_pending(uint32_t vcpu_id, uint32_t irq)
{
    if (!valid_irq(irq))
        return;

    if (irq < 16) {
        vmm_vgic_set_sgi_pending(vcpu_id, 0, irq);
    } else if (irq < 32) {
        if (valid_vcpu(vcpu_id))
            g_vcpus[vcpu_id].pending0 |= irq_bit(irq);
    } else {
        g_pending[irq / 32] |= irq_bit(irq);
    }
}

void vmm_vgic_set_enabled(uint32_t vcpu_id, uint32_t irq, int enabled)
{
    if (!valid_irq(irq) || irq < 16)
        return;

    if (irq < 32) {
        if (!valid_vcpu(vcpu_id))
            return;
        if (enabled)
            g_vcpus[vcpu_id].enabled0 |= irq_bit(irq);
        else
            g_vcpus[vcpu_id].enabled0 &= ~irq_bit(irq);
    } else if (enabled) {
        g_enabled[irq / 32] |= irq_bit(irq);
    } else {
        g_enabled[irq / 32] &= ~irq_bit(irq);
    }
}

uint32_t vmm_vgic_enabled_word(uint32_t vcpu_id, uint32_t word)
{
    if (word >= VGIC_MAX_WORDS)
        return 0;
    if (word == 0)
        return valid_vcpu(vcpu_id) ? (g_vcpus[vcpu_id].enabled0 | SGI_MASK) : SGI_MASK;
    return g_enabled[word];
}

uint32_t vmm_vgic_pending_word(uint32_t vcpu_id, uint32_t word)
{
    if (word >= VGIC_MAX_WORDS)
        return 0;
    if (word == 0)
        return valid_vcpu(vcpu_id) ? g_vcpus[vcpu_id].pending0 : 0;
    return g_pending[word];
}

uint32_t vmm_vgic_active_word(uint32_t vcpu_id, uint32_t word)
{
    if (word >= VGIC_MAX_WORDS)
        return 0;
    if (word == 0)
        return valid_vcpu(vcpu_id) ? g_vcpus[vcpu_id].active0 : 0;
    return g_active[word];
}

void vmm_vgic_clear_pending_word(uint32_t vcpu_id, uint32_t word,
                                 uint32_t bits)
{
    if (word >= VGIC_MAX_WORDS)
        return;
    if (word == 0) {
        if (valid_vcpu(vcpu_id))
            g_vcpus[vcpu_id].pending0 &= ~bits;
    } else {
        g_pending[word] &= ~bits;
    }
}

void vmm_vgic_clear_active_word(uint32_t vcpu_id, uint32_t word,
                                uint32_t bits)
{
    if (word >= VGIC_MAX_WORDS)
        return;
    if (word == 0) {
        if (valid_vcpu(vcpu_id))
            g_vcpus[vcpu_id].active0 &= ~bits;
    } else {
        g_active[word] &= ~bits;
    }
}

int vmm_vgic_next_pending(uint32_t vcpu_id)
{
    if (!valid_vcpu(vcpu_id))
        return -1;

    vgic_vcpu_t *vcpu = &g_vcpus[vcpu_id];
    uint32_t ready0 = vcpu->pending0 & (vcpu->enabled0 | SGI_MASK) & ~vcpu->active0;
    if (ready0)
        return (int)lowest_irq(ready0, 0);

    for (uint32_t word = 1; word < VGIC_MAX_WORDS; word++) {
        uint32_t ready = g_pending[word] & g_enabled[word] & ~g_active[word];
        if (ready)
            return (int)lowest_irq(ready, word * 32);
    }
    return -1;
}

int vmm_vgic_ack(uint32_t vcpu_id)
{
    int irq = vmm_vgic_next_pending(vcpu_id);
    if (irq < 0)
        return -1;

    if ((uint32_t)irq < 32) {
        vgic_vcpu_t *vcpu = &g_vcpus[vcpu_id];
        vcpu->pending0 &= ~irq_bit((uint32_t)irq);
        if ((uint32_t)irq < 16)
            vcpu->sgi_sources[irq] = 0;
        vcpu->active0 |= irq_bit((uint32_t)irq);
        vgicc_clear_lr_irq(vcpu_id, (uint32_t)irq);
    } else {
        g_pending[(uint32_t)irq / 32] &= ~irq_bit((uint32_t)irq);
        g_active[(uint32_t)irq / 32] |= irq_bit((uint32_t)irq);
        vgicc_clear_lr_irq(vcpu_id, (uint32_t)irq);
    }
    return irq;
}

void vmm_vgic_eoi(uint32_t vcpu_id, uint32_t irq)
{
    if (!valid_irq(irq))
        return;

    if (irq < 32) {
        if (valid_vcpu(vcpu_id))
            g_vcpus[vcpu_id].active0 &= ~irq_bit(irq);
    } else {
        g_active[irq / 32] &= ~irq_bit(irq);
    }
}

void vmm_vgic_inject_timer(uint32_t vcpu_id)
{
    vmm_vgic_set_pending(vcpu_id, HOST_VTIMER_IRQ);
    vmm_vgic_sync_entry(vcpu_id);
}

void vmm_vgic_sync_entry(uint32_t vcpu_id)
{
    if (!valid_vcpu(vcpu_id))
        return;

    vgic_vcpu_t *vcpu = &g_vcpus[vcpu_id];

    while (1) {
        int irq = vmm_vgic_next_pending(vcpu_id);
        if (irq < 0)
            break;
        if (vgicc_lr_has_irq(vcpu_id, (uint32_t)irq))
            break;

        int slot = vgicc_lr_empty_slot(vcpu_id);
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

        vgicc_write_lr(vcpu_id, (uint32_t)slot, lr_value((uint32_t)irq, src));
        break;
    }
}

void vmm_vgic_sync_exit(uint32_t vcpu_id)
{
    if (!valid_vcpu(vcpu_id))
        return;

    vgicc_save_state_from_hw(vcpu_id);
}
