/*
 * kernel/vmm/vdev/vplic.c — 虚拟 RISC-V PLIC 实现
 *
 * 移植自 x-kernel: virt/kvmm/src/vdev/riscv64/irq.rs，适配 Avatar OS：
 *   - Rust Atomic 数组 → 普通静态数组
 *   - 仅支持前 64 源，context N == vCPU N
 *
 * ── 多 VM：状态与锁都搬进了 vm_t ─────────────────────────────
 *
 * 从前状态是个文件级 static（g_vplic），整机只有一份 —— 第二个 VM 的
 * vplic_init() 一句 memset 就把第一个 VM 的 priority/enable/threshold 全部
 * 清掉，两个 VM 从此共用一个中断控制器且毫无报错。现在每 VM 一份。
 *
 * 锁是新加的（老代码一把都没有）：单 VM 时所有调用点确实都在同一个 vCPU
 * 任务里，但状态既然按 VM 分开了，就该按"可能跨上下文"来写。加锁的形状与
 * vpl011/uart16550 一致，且把 `_locked` 内部版和公开版分开，避免 MMIO 回调
 * 里出现"自己锁自己"。
 *
 * ⚠️ 约定：所有 `_locked` 后缀的函数要求调用方已持 vplic_lock。
 */

#include "vmm/vmm_vplic.h"
#include "vmm/vmm.h"        /* vm_t：vplic_state_t 的宿主 */
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

/* MMIO 回调只有 dev->priv（→ state），state->owner 是回指的 VM（拿锁用）*/
#define VPLIC_LOCK(s)   (&(s)->owner->vplic_lock)

/* ── 内部实现（调用方已持锁）──────────────────────────────── */

static uint32_t vplic_next_deliverable_locked(const vplic_state_t *s,
                                              uint32_t vcpu_id)
{
    if (vcpu_id >= s->nr_vcpus || vcpu_id >= VPLIC_MAX_VCPUS)
        return 0;

    uint64_t candidates = s->pending[vcpu_id]
                        & s->enable[vcpu_id]
                        & ~s->active[vcpu_id]
                        & VALID_IRQ_MASK;

    uint32_t threshold = s->threshold[vcpu_id];
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

        uint32_t prio = s->priority[irq];
        if (prio > best_prio) {
            best_irq = irq;
            best_prio = prio;
        }
    }
    return best_irq;
}

static uint32_t vplic_claim_locked(vplic_state_t *s, uint32_t vcpu_id)
{
    uint32_t irq;

    if (vcpu_id >= VPLIC_MAX_VCPUS)
        return 0;

    irq = vplic_next_deliverable_locked(s, vcpu_id);
    if (irq == 0)
        return 0;

    s->pending[vcpu_id] &= ~(1ULL << irq);
    s->active[vcpu_id]  |=  (1ULL << irq);
    return irq;
}

static void vplic_complete_locked(vplic_state_t *s, uint32_t vcpu_id,
                                  uint32_t irq)
{
    if (vcpu_id >= VPLIC_MAX_VCPUS || irq == 0 || irq >= VPLIC_MAX_IRQS)
        return;
    s->active[vcpu_id] &= ~(1ULL << irq);
}

/* ── 公开注入接口 ─────────────────────────────────────────── */
void vplic_set_pending(vm_t *vm, uint32_t irq)
{
    vplic_state_t *s = &vm->vplic;
    uint64_t flags;

    if (irq == 0 || irq >= VPLIC_MAX_IRQS)
        return;

    spin_lock_irqsave(&vm->vplic_lock, &flags);
    /* 挂起是全局的（源级），每个 context 各自判断是否可见 */
    for (uint32_t i = 0; i < s->nr_vcpus && i < VPLIC_MAX_VCPUS; i++)
        s->pending[i] |= (1ULL << irq);
    spin_unlock_irqrestore(&vm->vplic_lock, flags);
}

uint32_t vplic_next_deliverable(vm_t *vm, uint32_t vcpu_id)
{
    uint64_t flags;
    uint32_t irq;

    spin_lock_irqsave(&vm->vplic_lock, &flags);
    irq = vplic_next_deliverable_locked(&vm->vplic, vcpu_id);
    spin_unlock_irqrestore(&vm->vplic_lock, flags);
    return irq;
}

uint32_t vplic_claim(vm_t *vm, uint32_t vcpu_id)
{
    uint64_t flags;
    uint32_t irq;

    spin_lock_irqsave(&vm->vplic_lock, &flags);
    irq = vplic_claim_locked(&vm->vplic, vcpu_id);
    spin_unlock_irqrestore(&vm->vplic_lock, flags);
    return irq;
}

void vplic_complete(vm_t *vm, uint32_t vcpu_id, uint32_t irq)
{
    uint64_t flags;

    spin_lock_irqsave(&vm->vplic_lock, &flags);
    vplic_complete_locked(&vm->vplic, vcpu_id, irq);
    spin_unlock_irqrestore(&vm->vplic_lock, flags);
}

/* ── MMIO 读写回调 ────────────────────────────────────────── */
static uint64_t vplic_read(mmio_device_t *dev, uint64_t off, uint8_t size)
{
    vplic_state_t *s = (vplic_state_t *)dev->priv;
    uint64_t flags;
    uint64_t ret = 0;

    (void)size;

    spin_lock_irqsave(VPLIC_LOCK(s), &flags);

    /* priority：每源 4 字节 */
    if (off < PRIORITY_OFFSET + VPLIC_MAX_IRQS * 4) {
        uint32_t irq = (uint32_t)(off / 4);
        ret = (irq < VPLIC_MAX_IRQS) ? s->priority[irq] : 0;
        goto out;
    }

    /* pending：每 32 源 1 字 */
    if (off >= PENDING_OFFSET && off < PENDING_OFFSET + 8) {
        uint32_t word = (uint32_t)((off - PENDING_OFFSET) / 4);
        uint64_t all = 0;
        if (word >= 2) {
            ret = 0;
            goto out;
        }
        for (uint32_t i = 0; i < s->nr_vcpus && i < VPLIC_MAX_VCPUS; i++)
            all |= s->pending[i];
        ret = (all >> (word * 32)) & 0xFFFFFFFFu;
        goto out;
    }

    /* enable：按 context 分块 */
    if (off >= ENABLE_OFFSET &&
        off < ENABLE_OFFSET + (uint64_t)ENABLE_STRIDE * VPLIC_MAX_VCPUS) {
        uint32_t ctx  = (uint32_t)((off - ENABLE_OFFSET) / ENABLE_STRIDE);
        uint32_t word = (uint32_t)(((off - ENABLE_OFFSET) % ENABLE_STRIDE) / 4);
        ret = (ctx < s->nr_vcpus && word < 2)
            ? (uint32_t)((s->enable[ctx] >> (word * 32)) & 0xFFFFFFFFu) : 0;
        goto out;
    }

    /* context 区：threshold / claim-complete */
    if (off >= CONTEXT_OFFSET) {
        uint32_t ctx  = (uint32_t)((off - CONTEXT_OFFSET) / CONTEXT_STRIDE);
        uint32_t reg  = (uint32_t)((off - CONTEXT_OFFSET) % CONTEXT_STRIDE);
        if (ctx < s->nr_vcpus) {
            if (reg == CONTEXT_THRESHOLD_OFF)
                ret = s->threshold[ctx];
            else if (reg == CONTEXT_CLAIM_COMPLETE)
                ret = vplic_claim_locked(s, ctx);   /* 读 claim = 认领 */
        }
        goto out;
    }

out:
    spin_unlock_irqrestore(VPLIC_LOCK(s), flags);
    return ret;
}

static void vplic_write(mmio_device_t *dev, uint64_t off, uint8_t size,
                        uint64_t value)
{
    vplic_state_t *s = (vplic_state_t *)dev->priv;
    uint64_t flags;
    uint32_t v = (uint32_t)value;

    (void)size;

    spin_lock_irqsave(VPLIC_LOCK(s), &flags);

    /* priority */
    if (off < PRIORITY_OFFSET + VPLIC_MAX_IRQS * 4) {
        uint32_t irq = (uint32_t)(off / 4);
        if (irq > 0 && irq < VPLIC_MAX_IRQS)
            s->priority[irq] = v;
        goto out;
    }

    /* enable */
    if (off >= ENABLE_OFFSET &&
        off < ENABLE_OFFSET + (uint64_t)ENABLE_STRIDE * VPLIC_MAX_VCPUS) {
        uint32_t ctx  = (uint32_t)((off - ENABLE_OFFSET) / ENABLE_STRIDE);
        uint32_t word = (uint32_t)(((off - ENABLE_OFFSET) % ENABLE_STRIDE) / 4);
        if (ctx < s->nr_vcpus && word < 2) {
            uint32_t shift = word * 32;
            s->enable[ctx] &= ~((uint64_t)0xFFFFFFFFu << shift);
            s->enable[ctx] |= ((uint64_t)v << shift) & VALID_IRQ_MASK;
        }
        goto out;
    }

    /* context：threshold / complete */
    if (off >= CONTEXT_OFFSET) {
        uint32_t ctx = (uint32_t)((off - CONTEXT_OFFSET) / CONTEXT_STRIDE);
        uint32_t reg = (uint32_t)((off - CONTEXT_OFFSET) % CONTEXT_STRIDE);
        if (ctx < s->nr_vcpus) {
            if (reg == CONTEXT_THRESHOLD_OFF)
                s->threshold[ctx] = v;
            else if (reg == CONTEXT_CLAIM_COMPLETE)
                vplic_complete_locked(s, ctx, v);   /* 写 complete = 完成 */
        }
        goto out;
    }

    /* pending 区只读（guest 不能直接写挂起）*/
out:
    spin_unlock_irqrestore(VPLIC_LOCK(s), flags);
}

static const mmio_dev_ops_t g_vplic_ops = {
    .name  = "vplic",
    .base  = VPLIC_BASE,
    .size  = VPLIC_SIZE,
    .read  = vplic_read,
    .write = vplic_write,
};

int vplic_init(vm_t *vm, mmio_device_t *dev, mmio_bus_t *bus, uint32_t nr_vcpus)
{
    if (!vm || !dev || !bus)
        return -1;
    if (nr_vcpus < 1)
        nr_vcpus = 1;
    if (nr_vcpus > VPLIC_MAX_VCPUS)
        nr_vcpus = VPLIC_MAX_VCPUS;

    memset(&vm->vplic, 0, sizeof(vm->vplic));
    vm->vplic.nr_vcpus = nr_vcpus;
    vm->vplic.owner    = vm;

    dev->ops  = &g_vplic_ops;
    dev->priv = &vm->vplic;

    KLOG_INFO("[vplic] vm%u: virtual PLIC ready (%u vCPU, %u sources)\n",
              vm->vmid, nr_vcpus, (unsigned)VPLIC_MAX_IRQS);
    return mmio_bus_register(bus, dev);
}
