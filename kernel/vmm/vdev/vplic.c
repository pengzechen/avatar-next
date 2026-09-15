/*
 * kernel/vmm/vdev/vplic.c — 虚拟 RISC-V PLIC 实现
 *
 * 移植自 x-kernel: virt/kvmm/src/vdev/riscv64/irq.rs，适配 Avatar OS：
 *   - Rust Atomic 数组 → 普通静态数组（vCPU 世界切换窗口内串行访问）
 *   - 仅支持前 64 源，context N == vCPU N
 */

#include "vmm_vplic.h"
#include "klog.h"
#include "string.h"

/* ── PLIC 寄存器偏移 ──────────────────────────────────────── */
#define PRIORITY_OFFSET        0x000000
#define PENDING_OFFSET         0x001000
#define ENABLE_OFFSET          0x002000
#define ENABLE_STRIDE          0x80
#define CONTEXT_OFFSET         0x200000
#define CONTEXT_STRIDE         0x1000
#define CONTEXT_THRESHOLD_OFF    0x00
#define CONTEXT_CLAIM_COMPLETE   0x04

/* 源 0 无效（不支持 IRQ 0）*/
#define VALID_IRQ_MASK   (~1ULL)

/* ── 设备私有状态 ─────────────────────────────────────────── */
typedef struct {
    uint32_t priority[VPLIC_MAX_IRQS];
    uint64_t pending[VPLIC_MAX_VCPUS];
    uint64_t active[VPLIC_MAX_VCPUS];
    uint64_t enable[VPLIC_MAX_VCPUS];
    uint32_t threshold[VPLIC_MAX_VCPUS];
    uint32_t nr_vcpus;
} vplic_state_t;

static vplic_state_t g_vplic;

/* ── 公开注入接口 ─────────────────────────────────────────── */
void vplic_set_pending(uint32_t irq)
{
    if (irq == 0 || irq >= VPLIC_MAX_IRQS)
        return;
    /* 挂起是全局的（源级），每个 context 各自判断是否可见 */
    for (uint32_t i = 0; i < g_vplic.nr_vcpus && i < VPLIC_MAX_VCPUS; i++)
        g_vplic.pending[i] |= (1ULL << irq);
}

uint32_t vplic_next_deliverable(uint32_t vcpu_id)
{
    if (vcpu_id >= g_vplic.nr_vcpus || vcpu_id >= VPLIC_MAX_VCPUS)
        return 0;

    uint64_t candidates = g_vplic.pending[vcpu_id]
                        & g_vplic.enable[vcpu_id]
                        & ~g_vplic.active[vcpu_id]
                        & VALID_IRQ_MASK;

    uint32_t threshold = g_vplic.threshold[vcpu_id];
    uint32_t best_irq = 0;
    uint32_t best_prio = threshold;

    /*
     * 注意：freestanding 构建下 __builtin_ctzll 会生成对 __ctzdi2 的调用
     * （libgcc 未链接），故用「低位清零」循环代替（与 hext_run.c 同风格）。
     */
    while (candidates != 0) {
        uint32_t irq = 0;
        uint64_t t = candidates & (~candidates + 1);   /* 取最低置位 */
        while (t > 1) { t >>= 1; irq++; }              /* 位序号 */

        candidates &= candidates - 1;

        uint32_t prio = g_vplic.priority[irq];
        if (prio > best_prio) {
            best_irq = irq;
            best_prio = prio;
        }
    }
    return best_irq;
}

uint32_t vplic_claim(uint32_t vcpu_id)
{
    if (vcpu_id >= VPLIC_MAX_VCPUS)
        return 0;

    uint32_t irq = vplic_next_deliverable(vcpu_id);
    if (irq == 0)
        return 0;

    g_vplic.pending[vcpu_id] &= ~(1ULL << irq);
    g_vplic.active[vcpu_id]  |=  (1ULL << irq);
    return irq;
}

void vplic_complete(uint32_t vcpu_id, uint32_t irq)
{
    if (vcpu_id >= VPLIC_MAX_VCPUS || irq == 0 || irq >= VPLIC_MAX_IRQS)
        return;
    g_vplic.active[vcpu_id] &= ~(1ULL << irq);
}

/* ── MMIO 读写回调 ────────────────────────────────────────── */
static uint64_t vplic_read(mmio_device_t *dev, uint64_t off, uint8_t size)
{
    (void)dev;
    (void)size;

    /* priority：每源 4 字节 */
    if (off < PRIORITY_OFFSET + VPLIC_MAX_IRQS * 4) {
        uint32_t irq = (uint32_t)(off / 4);
        if (irq < VPLIC_MAX_IRQS)
            return g_vplic.priority[irq];
        return 0;
    }

    /* pending：每 32 源 1 字 */
    if (off >= PENDING_OFFSET && off < PENDING_OFFSET + 8) {
        uint32_t word = (uint32_t)((off - PENDING_OFFSET) / 4);
        if (word >= 2)
            return 0;
        uint64_t all = 0;
        for (uint32_t i = 0; i < g_vplic.nr_vcpus && i < VPLIC_MAX_VCPUS; i++)
            all |= g_vplic.pending[i];
        return (all >> (word * 32)) & 0xFFFFFFFFu;
    }

    /* enable：按 context 分块 */
    if (off >= ENABLE_OFFSET &&
        off < ENABLE_OFFSET + (uint64_t)ENABLE_STRIDE * VPLIC_MAX_VCPUS) {
        uint32_t ctx  = (uint32_t)((off - ENABLE_OFFSET) / ENABLE_STRIDE);
        uint32_t word = (uint32_t)(((off - ENABLE_OFFSET) % ENABLE_STRIDE) / 4);
        if (ctx < g_vplic.nr_vcpus && word < 2)
            return (g_vplic.enable[ctx] >> (word * 32)) & 0xFFFFFFFFu;
        return 0;
    }

    /* context 区：threshold / claim-complete */
    if (off >= CONTEXT_OFFSET) {
        uint32_t ctx  = (uint32_t)((off - CONTEXT_OFFSET) / CONTEXT_STRIDE);
        uint32_t reg  = (uint32_t)((off - CONTEXT_OFFSET) % CONTEXT_STRIDE);
        if (ctx < g_vplic.nr_vcpus) {
            if (reg == CONTEXT_THRESHOLD_OFF)
                return g_vplic.threshold[ctx];
            if (reg == CONTEXT_CLAIM_COMPLETE)
                return vplic_claim(ctx);    /* 读 claim = 认领 */
        }
        return 0;
    }

    return 0;
}

static void vplic_write(mmio_device_t *dev, uint64_t off, uint8_t size,
                        uint64_t value)
{
    (void)dev;
    (void)size;
    uint32_t v = (uint32_t)value;

    /* priority */
    if (off < PRIORITY_OFFSET + VPLIC_MAX_IRQS * 4) {
        uint32_t irq = (uint32_t)(off / 4);
        if (irq > 0 && irq < VPLIC_MAX_IRQS)
            g_vplic.priority[irq] = v;
        return;
    }

    /* enable */
    if (off >= ENABLE_OFFSET &&
        off < ENABLE_OFFSET + (uint64_t)ENABLE_STRIDE * VPLIC_MAX_VCPUS) {
        uint32_t ctx  = (uint32_t)((off - ENABLE_OFFSET) / ENABLE_STRIDE);
        uint32_t word = (uint32_t)(((off - ENABLE_OFFSET) % ENABLE_STRIDE) / 4);
        if (ctx < g_vplic.nr_vcpus && word < 2) {
            uint32_t shift = word * 32;
            g_vplic.enable[ctx] &= ~((uint64_t)0xFFFFFFFFu << shift);
            g_vplic.enable[ctx] |= ((uint64_t)v << shift) & VALID_IRQ_MASK;
        }
        return;
    }

    /* context：threshold / complete */
    if (off >= CONTEXT_OFFSET) {
        uint32_t ctx = (uint32_t)((off - CONTEXT_OFFSET) / CONTEXT_STRIDE);
        uint32_t reg = (uint32_t)((off - CONTEXT_OFFSET) % CONTEXT_STRIDE);
        if (ctx < g_vplic.nr_vcpus) {
            if (reg == CONTEXT_THRESHOLD_OFF) {
                g_vplic.threshold[ctx] = v;
            } else if (reg == CONTEXT_CLAIM_COMPLETE) {
                vplic_complete(ctx, v);     /* 写 complete = 完成 */
            }
        }
        return;
    }

    /* pending 区只读（guest 不能直接写挂起）*/
}

static const mmio_dev_ops_t g_vplic_ops = {
    .name  = "vplic",
    .base  = VPLIC_BASE,
    .size  = VPLIC_SIZE,
    .read  = vplic_read,
    .write = vplic_write,
};

int vplic_init(mmio_device_t *dev, mmio_bus_t *bus, uint32_t nr_vcpus)
{
    if (!dev || !bus)
        return -1;
    if (nr_vcpus < 1)
        nr_vcpus = 1;
    if (nr_vcpus > VPLIC_MAX_VCPUS)
        nr_vcpus = VPLIC_MAX_VCPUS;

    memset(&g_vplic, 0, sizeof(g_vplic));
    g_vplic.nr_vcpus = nr_vcpus;

    dev->ops  = &g_vplic_ops;
    dev->priv = &g_vplic;

    KLOG_INFO("[vplic] virtual PLIC ready (%u vCPU, %u sources)\n",
              nr_vcpus, (unsigned)VPLIC_MAX_IRQS);
    return mmio_bus_register(bus, dev);
}
