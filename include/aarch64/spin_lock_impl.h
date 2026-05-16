#ifndef AARCH64_SPIN_LOCK_IMPL_H
#define AARCH64_SPIN_LOCK_IMPL_H

/*
 * AArch64 架构的 spinlock 实现
 * 使用 LDAXR/STLXR（acquire/release 语义）实现原子 CAS。
 * irqsave 变体与 RISC-V/x86_64 保持一致：
 *   先用 C 辅助函数保存并关中断，再调用 spin_lock。
 */

/* ── DAIF 保存 / 恢复辅助函数 ─────────────────────────────── */

static inline uint64_t
aarch64_irq_save(void)
{
    uint64_t daif;
    asm volatile("mrs %0, daif" : "=r"(daif) :: "memory");
    asm volatile("msr daifset, #2" ::: "memory");   /* 关 IRQ (I bit) */
    return daif;
}

static inline void
aarch64_irq_restore(uint64_t daif)
{
    asm volatile("msr daif, %0" :: "r"(daif) : "memory");
}

/* ── spin_lock ────────────────────────────────────────────── */

static inline void
spin_lock(spinlock_t *lock)
{
    uint64_t tmp, one = 1;
    asm volatile(
        /* 先用普通 ldr 等待锁可用（避免总线锁事务持续占用） */
        "1: ldr    %w0, [%2]          \n"
        "   cbnz   %w0, 1b            \n"
        /* 再用 ldaxr/stlxr 原子取锁 */
        "2: ldaxr  %w0, [%2]          \n"
        "   cbnz   %w0, 1b            \n"   /* 锁被抢走，回到等待 */
        "   stlxr  %w0, %w1, [%2]     \n"
        "   cbnz   %w0, 2b            \n"   /* stlxr 失败，重试 */
        : "=&r"(tmp)
        : "r"(one), "r"(&lock->lock)
        : "memory");
}

static inline int
spin_trylock(spinlock_t *lock)
{
    uint64_t tmp, one = 1;
    asm volatile(
        "   ldaxr  %w0, [%2]          \n"
        "   cbnz   %w0, 1f            \n"
        "   stlxr  %w0, %w1, [%2]     \n"
        "   cbnz   %w0, 1f            \n"   /* store 失败 → 返回 1（失败） */
        "   mov    %w0, #0            \n"   /* 成功 → 返回 0 */
        "   b      2f                 \n"
        "1: mov    %w0, #1            \n"   /* 失败 → 返回 1 */
        "2:                           \n"
        : "=&r"(tmp)
        : "r"(one), "r"(&lock->lock)
        : "memory");
    return (int)tmp;
}

static inline void
spin_unlock(spinlock_t *lock)
{
    /* stlr 已含 release 语义，无需额外 dmb */
    asm volatile(
        "stlr  wzr, [%0]" :: "r"(&lock->lock) : "memory");
}

/* ── 带中断保护的 spinlock ────────────────────────────────── */
/*
 * 设计：与 RISC-V/x86_64 一致——先在 C 层保存并关中断，再调用
 * spin_lock。DAIF 保存在 lock->irq_flags（单核串行持锁时安全）。
 */

static inline void
spin_lock_irqsave(spinlock_noirq_t *lock)
{
    lock->irq_flags = aarch64_irq_save();   /* 先关 IRQ，再自旋 */
    spin_lock((spinlock_t *)lock);
}

static inline int
spin_trylock_irqsave(spinlock_noirq_t *lock)
{
    lock->irq_flags = aarch64_irq_save();
    if (spin_trylock((spinlock_t *)lock) == 0)
        return 0;                           /* 成功 */
    aarch64_irq_restore(lock->irq_flags);   /* 失败则恢复中断 */
    return 1;
}

static inline void
spin_unlock_irqrestore(spinlock_noirq_t *lock)
{
    uint64_t saved = lock->irq_flags;
    spin_unlock((spinlock_t *)lock);        /* 先释放锁 */
    aarch64_irq_restore(saved);             /* 再恢复中断 */
}

#endif  // AARCH64_SPIN_LOCK_IMPL_H