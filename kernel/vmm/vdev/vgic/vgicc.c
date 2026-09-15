/*
 * kernel/vmm/vdev/vgic/vgicc.c — 虚拟 GICv2 CPU 接口（GICC）软件模拟
 *
 * 见 include/vmm_vgicc.h 的说明：直通硬件 GICV 依赖 HW=1 LR 的硬件投递，
 * 在当前宿主中断模型下不可用，改为 VMM 全模拟 + HCR_EL2.VI 注入。
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

/* GICv2 的 GICC_IIDR：Architecture=2, Revision=2, Implementer=ARM(0x43B) */
#define GICC_IIDR_VALUE  0x0202043B

/* spurious / "有挂起但不可投递" —— 这里用 1023（无挂起）*/
#define GICC_IAR_SPURIOUS  1023

typedef struct {
    uint32_t ctlr;
    uint32_t pmr;
    uint32_t iidr;
    uint32_t eoi_mode;   /* GICC_CTLR.EOImodeNS */
} vgicc_state_t;

static vgicc_state_t g_vgicc;

/* ── guest 读 ─────────────────────────────────────────────── */
static uint64_t vgicc_read(mmio_device_t *dev, uint64_t off, uint8_t size)
{
    (void)dev;
    (void)size;

    switch (off) {
    case GICC_CTLR:
        return g_vgicc.ctlr;
    case GICC_PMR:
        return g_vgicc.pmr;
    case GICC_IIDR:
        return g_vgicc.iidr;
    case GICC_IAR: {
        /* hmm—只有 vCPU0（单 vCPU 模型）*/
        int irq = vmm_vgic_ack(0);
        if (irq < 0)
            return GICC_IAR_SPURIOUS;
        return (uint64_t)irq;
    }
    default:
        return 0;
    }
}

/* ── guest 写 ─────────────────────────────────────────────── */
static void vgicc_write(mmio_device_t *dev, uint64_t off, uint8_t size,
                        uint64_t value)
{
    (void)dev;
    (void)size;

    switch (off) {
    case GICC_CTLR:
        g_vgicc.ctlr = (uint32_t)value;
        g_vgicc.eoi_mode = (uint32_t)(value >> 9) & 1;   /* EOImodeNS */
        break;
    case GICC_PMR:
        g_vgicc.pmr = (uint32_t)value;
        break;
    case GICC_EOIR:
        /* EOImode=0：EOI 同时 deactivate；EOImode=1：仅优先级下降 */
        if (!g_vgicc.eoi_mode)
            vmm_vgic_eoi(0, (uint32_t)value & 0x3ffu);
        break;
    case GICC_DIR:
        vmm_vgic_eoi(0, (uint32_t)value & 0x3ffu);
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
    .read           = vgicc_read,
    .write          = vgicc_write,
    .read_for_vcpu  = NULL,
    .write_for_vcpu = NULL,
};

int vgicc_init(mmio_device_t *dev, mmio_bus_t *bus)
{
    memset(&g_vgicc, 0, sizeof(g_vgicc));
    g_vgicc.iidr = GICC_IIDR_VALUE;
    /* 上电默认：PMR 全放行（guest 随后会自己写）*/
    g_vgicc.pmr = 0xf0;

    dev->ops  = &g_vgicc_ops;
    dev->priv = &g_vgicc;

    if (mmio_bus_register(bus, dev) != 0) {
        KLOG_WARN("[vgicc] MMIO registration failed\n");
        return -1;
    }

    KLOG_INFO("[vgicc] software GICC ready (IPA 0x%llx, irq via HCR_EL2.VI)\n",
              (unsigned long long)VGICC_BASE);
    return 0;
}
