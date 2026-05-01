#ifndef AARCH64_SPIN_LOCK_IMPL_H
#define AARCH64_SPIN_LOCK_IMPL_H

/*
 * AArch64 架构的spinlock实现
 * 使用 LDAXR/STLXR 指令对实现原子操作
 * 使用 DMB 指令进行内存屏障
 */

/* spinlock_t 和 spinlock_noirq_t 定义在 spinlock.h 中 */

static inline void
spin_lock(spinlock_t *lock)
{
    uint64_t tmp;
    asm volatile(
        "   mov w2, #1                    \n"
        "1:  ldaxr %w0, [%x1]              \n"
        "   cbnz %w0, 1b                   \n"
        "   stlxr %w0, w2, [%x1]           \n"
        "   cbnz %w0, 1b                   \n"
        "   dmb ish                        \n"
        : "=&r"(tmp)
        : "r"(&lock->lock)
        : "memory", "cc", "w2");
}

static inline int
spin_trylock(spinlock_t *lock)
{
    uint64_t tmp;
    int     result;
    asm volatile(
        "   mov w3, #1                    \n"
        "   ldaxr %w0, [%x2]              \n"
        "   cbnz %w0, 2f                  \n"
        "   stlxr %w0, w3, [%x2]          \n"
        "   cbnz %w0, 2f                  \n"
        "   dmb ish                       \n"
        "   mov %w1, #0                   \n"
        "   b 3f                          \n"
        "2:  mov %w1, #1                  \n"
        "3:                                \n"
        : "=&r"(tmp), "=r"(result)
        : "r"(&lock->lock)
        : "memory", "cc", "w3");
    return result;
}

static inline void
spin_unlock(spinlock_t *lock)
{
    asm volatile(
        "   dmb ish                        \n"
        "   mov w1, #0                    \n"
        "   stlr w1, [%x0]                \n"
        :
        : "r"(&lock->lock)
        : "memory", "cc", "w1");
}

/* 带中断保护的 spinlock */
static inline void
spin_lock_irqsave(spinlock_noirq_t *lock)
{
    uint64_t tmp;
    asm volatile(
        "   mrs %0, daif                  \n"
        "   str %w0, [%1, #8]             \n"
        "   msr daifset, #2               \n"
        "   mov w2, #1                    \n"
        "1:  ldaxr w3, [%2]               \n"
        "   cbnz w3, 1b                   \n"
        "   stlxr w3, w2, [%2]            \n"
        "   cbnz w3, 1b                   \n"
        "   dmb ish                       \n"
        : "=&r"(tmp)
        : "r"(&lock->lock), "r"(&lock->lock)
        : "memory", "cc", "w2", "w3");
}

static inline int
spin_trylock_irqsave(spinlock_noirq_t *lock)
{
    uint64_t tmp;
    int     result;
    asm volatile(
        "   mrs %0, daif                  \n"
        "   str %w0, [%2, #8]             \n"
        "   msr daifset, #2               \n"
        "   mov w2, #1                    \n"
        "   ldaxr w3, [%3]                \n"
        "   cbnz w3, 2f                   \n"
        "   stlxr w3, w2, [%3]            \n"
        "   cbnz w3, 2f                   \n"
        "   dmb ish                       \n"
        "   mov %w1, #0                   \n"
        "   b 3f                          \n"
        "2:  ldr x2, [%2, #8]             \n"
        "   msr daif, x2                  \n"
        "   mov %w1, #1                   \n"
        "3:                                \n"
        : "=&r"(tmp), "=r"(result)
        : "r"(&lock->irq_flags), "r"(&lock->lock)
        : "memory", "cc", "w2", "w3", "x2");
    return result;
}

static inline void
spin_unlock_irqrestore(spinlock_noirq_t *lock)
{
    uint64_t tmp;
    asm volatile(
        "   dmb ish                       \n"
        "   mov w2, #0                    \n"
        "   stlr w2, [%1]                 \n"
        "   ldr %0, [%2]                  \n"
        "   msr daif, %0                  \n"
        : "=&r"(tmp)
        : "r"(&lock->lock), "r"(&lock->irq_flags)
        : "memory", "cc", "w2");
}

#endif  // AARCH64_SPIN_LOCK_IMPL_H
