/*
 * kernel/vmm/vdev/vgic/vgicc.c — virtual GICv2 CPU interface / GICH state
 *
 * GICC registers are MMIO-emulated for the guest. The cached GICH list
 * registers live here as per-vCPU CPU-interface state; vgic.c only decides
 * which virtual IRQ is pending and asks this layer to program an LR.
 *
 * Linux 的 gic_cpu_init()/gic_handle_irq() 通过这些寄存器工作：
 *   GICC_CTLR  — 使能 CPU 接口（本模拟恒为"已使能"，只记录值）
 *   GICC_PMR   — 优先级掩码（记录；投递选择不做优先级过滤）
 *   GICC_IAR   — ack：返回中断号并从挂起位图取走
 *   GICC_EOIR  — EOI：清 active
 *   GICC_DIR   — deactivate（EOImode=1 时 EOI 之后写）
 *   GICC_IIDR  — 实现版本（Linux 只打印）
 */

#include "vmm_vgicc.h"
#include "vmm_vgic.h"
#include "klog.h"
#include "string.h"

uint32_t gic_read_lr(int32_t n);
void gic_write_lr(int32_t n, uint32_t mask);

/* GICv2 的 GICC_IIDR：Architecture=2, Revision=2, Implementer=ARM(0x43B) */
#define GICC_IIDR_VALUE  0x0202043B

/* spurious / "有挂起但不可投递" —— 这里用 1023（无挂起）*/
#define GICC_IAR_SPURIOUS  1023

typedef struct {
    uint32_t ctlr;
    uint32_t pmr;
    uint32_t iidr;
    uint32_t eoi_mode;   /* GICC_CTLR.EOImodeNS */
    uint32_t lr[VGIC_MAX_LRS];
} vgicc_state_t;

static vgicc_state_t g_vgicc[VGIC_MAX_VCPUS];
static uint32_t g_nr_vcpus;

static int valid_vcpu(uint32_t vcpu_id)
{
    return vcpu_id < g_nr_vcpus;
}

/* ── guest 读 ─────────────────────────────────────────────── */
static uint64_t vgicc_read_for_vcpu(mmio_device_t *dev, uint64_t off,
                                    uint8_t size, uint32_t vcpu_id)
{
    (void)dev;
    (void)size;

    if (!valid_vcpu(vcpu_id))
        vcpu_id = 0;

    vgicc_state_t *s = &g_vgicc[vcpu_id];

    switch (off) {
    case GICC_CTLR:
        return s->ctlr;
    case GICC_PMR:
        return s->pmr;
    case GICC_IIDR:
        return s->iidr;
    case GICC_IAR: {
        int irq = vmm_vgic_ack(vcpu_id);
        if (irq < 0)
            return GICC_IAR_SPURIOUS;
        return (uint64_t)irq;
    }
    default:
        return 0;
    }
}

/* ── guest 写 ─────────────────────────────────────────────── */
static void vgicc_write_for_vcpu(mmio_device_t *dev, uint64_t off,
                                 uint8_t size, uint64_t value,
                                 uint32_t vcpu_id)
{
    (void)dev;
    (void)size;

    if (!valid_vcpu(vcpu_id))
        vcpu_id = 0;

    vgicc_state_t *s = &g_vgicc[vcpu_id];

    switch (off) {
    case GICC_CTLR:
        s->ctlr = (uint32_t)value;
        s->eoi_mode = (uint32_t)(value >> 9) & 1;   /* EOImodeNS */
        break;
    case GICC_PMR:
        s->pmr = (uint32_t)value;
        break;
    case GICC_EOIR:
        /* EOImode=0：EOI 同时 deactivate；EOImode=1：仅优先级下降 */
        if (!s->eoi_mode)
            vmm_vgic_eoi(vcpu_id, (uint32_t)value & 0x3ffu);
        break;
    case GICC_DIR:
        vmm_vgic_eoi(vcpu_id, (uint32_t)value & 0x3ffu);
        break;
    default:
        break;
    }
}

/* ── 初始化 ───────────────────────────────────────────────── */
static mmio_dev_ops_t g_vgicc_ops = {
    .name           = "vgicc",
    .base           = VGICC_BASE,
    .size           = 0x10000,
    .read           = NULL,
    .write          = NULL,
    .read_for_vcpu  = vgicc_read_for_vcpu,
    .write_for_vcpu = vgicc_write_for_vcpu,
};

int vgicc_init(mmio_device_t *dev, mmio_bus_t *bus, uint32_t nr_vcpus)
{
    if (!dev || !bus)
        return -1;
    if (nr_vcpus < 1)
        nr_vcpus = 1;
    if (nr_vcpus > VGIC_MAX_VCPUS)
        nr_vcpus = VGIC_MAX_VCPUS;

    memset(g_vgicc, 0, sizeof(g_vgicc));
    g_nr_vcpus = nr_vcpus;

    for (uint32_t i = 0; i < VGIC_MAX_VCPUS; i++) {
        g_vgicc[i].iidr = GICC_IIDR_VALUE;
        /* 上电默认：PMR 全放行（guest 随后会自己写）*/
        g_vgicc[i].pmr = 0xf0;
    }

    dev->ops  = &g_vgicc_ops;
    dev->priv = g_vgicc;

    if (mmio_bus_register(bus, dev) != 0) {
        KLOG_WARN("[vgicc] MMIO registration failed\n");
        return -1;
    }

    KLOG_INFO("[vgicc] CPU interface ready (%u vCPU, IPA 0x%llx, %u LR)\n",
              nr_vcpus, (unsigned long long)VGICC_BASE,
              (unsigned)VGIC_MAX_LRS);
    return 0;
}

int vgicc_lr_has_irq(uint32_t vcpu_id, uint32_t irq)
{
    if (!valid_vcpu(vcpu_id))
        return 0;

    vgicc_state_t *s = &g_vgicc[vcpu_id];
    for (uint32_t i = 0; i < VGIC_MAX_LRS; i++) {
        if ((s->lr[i] & LR_STATE_MASK) &&
            ((s->lr[i] & LR_VINTID_MASK) == (irq & LR_VINTID_MASK)))
            return 1;
    }
    return 0;
}

int vgicc_lr_empty_slot(uint32_t vcpu_id)
{
    if (!valid_vcpu(vcpu_id))
        return -1;

    vgicc_state_t *s = &g_vgicc[vcpu_id];
    for (uint32_t i = 0; i < VGIC_MAX_LRS; i++) {
        if ((s->lr[i] & LR_STATE_MASK) == 0)
            return (int)i;
    }
    return -1;
}

void vgicc_write_lr(uint32_t vcpu_id, uint32_t slot, uint32_t value)
{
    if (!valid_vcpu(vcpu_id) || slot >= VGIC_MAX_LRS)
        return;

    g_vgicc[vcpu_id].lr[slot] = value;
    gic_write_lr((int32_t)slot, value);
}

void vgicc_clear_lr_irq(uint32_t vcpu_id, uint32_t irq)
{
    if (!valid_vcpu(vcpu_id))
        return;

    vgicc_state_t *s = &g_vgicc[vcpu_id];
    for (uint32_t i = 0; i < VGIC_MAX_LRS; i++) {
        if ((s->lr[i] & LR_STATE_MASK) &&
            ((s->lr[i] & LR_VINTID_MASK) == (irq & LR_VINTID_MASK))) {
            s->lr[i] = 0;
            gic_write_lr((int32_t)i, 0);
        }
    }
}

void vgicc_save_state_from_hw(uint32_t vcpu_id)
{
    if (!valid_vcpu(vcpu_id))
        return;

    vgicc_state_t *s = &g_vgicc[vcpu_id];
    for (uint32_t i = 0; i < VGIC_MAX_LRS; i++) {
        s->lr[i] = gic_read_lr((int32_t)i);
        if ((s->lr[i] & LR_STATE_MASK) == 0)
            s->lr[i] = 0;
        gic_write_lr((int32_t)i, 0);
    }
}
