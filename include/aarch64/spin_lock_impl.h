#ifndef AARCH64_SPIN_LOCK_IMPL_H
#define AARCH64_SPIN_LOCK_IMPL_H

/*
 * AArch64 架构的 spinlock 实现
 * 使用 LDAXR/STLXR（acquire/release 语义）实现原子 CAS。
 * irqsave 变体与 RISC-V/x86_64 保持一致：
 *   先用 C 辅助函数保存并关中断，再调用 spin_lock。
 */

#include "task/preempt.h"
#include "aarch64/exception_impl.h"   /* arch_irq_save/restore（统一的中断屏蔽原语）*/

/* ── spin_lock ────────────────────────────────────────────── */

static inline void
spin_lock(spinlock_t *lock)
{
    uint64_t tmp, one = 1;
    preempt_disable();
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
    preempt_disable();
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
    if (tmp != 0)
        preempt_enable();
    return (int)tmp;
}

static inline void
spin_unlock(spinlock_t *lock)
{
    /* stlr 已含 release 语义，无需额外 dmb */
    asm volatile(
        "stlr  wzr, [%0]" :: "r"(&lock->lock) : "memory");
    preempt_enable();
}

/* ── 带中断保护的 spinlock ────────────────────────────────── */
/*
 * 中断状态存在**调用点的局部变量**里（由 *flags 带回），不再存进锁对象 ——
 * 存进锁对象时，SMP 下争锁的另一颗 CPU 会把它的 flags 覆盖上去，解锁时
 * 恢复的就是别人的中断状态。中断原语本身统一来自
 * include/aarch64/exception_impl.h（arch_irq_save/restore）。
 */

static inline void
spin_lock_irqsave(spinlock_t *lock, uint64_t *flags)
{
    *flags = arch_irq_save();               /* 先关中断，再自旋 */
    spin_lock(lock);
}

static inline int
spin_trylock_irqsave(spinlock_t *lock, uint64_t *flags)
{
    *flags = arch_irq_save();
    if (spin_trylock(lock) == 0)
        return 0;                           /* 成功 */
    arch_irq_restore(*flags);               /* 失败则恢复中断 */
    return 1;
}

static inline void
spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags)
{
    spin_unlock(lock);                      /* 先释放锁 */
    arch_irq_restore(flags);                /* 再恢复中断 */
}

#endif  // AARCH64_SPIN_LOCK_IMPL_H
