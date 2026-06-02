#ifndef RISCV64_SPIN_LOCK_IMPL_H
#define RISCV64_SPIN_LOCK_IMPL_H

#include "types.h"
#include "spinlock.h"

/*
 * RISC-V 64位架构的spinlock实现
 * 使用LR/SC (Load-Reserved/Store-Conditional) 指令对
 * 使用fence指令进行内存屏障
 */

/* spinlock_t 和 spinlock_noirq_t 定义在 spinlock.h 中 */

/*
 * RISC-V的原子操作使用LR/SC指令对
 * lr.w: 加载保留 Load-Reserved
 * sc.w: 条件存储 Store-Conditional
 * 如果在lr到sc之间有其他核心修改了地址，sc会失败
 */
static inline void
spin_lock(spinlock_t *lock)
{
    uint32_t tmp;
    asm volatile(
        "   li      %0, 1                    \n" /* tmp = 1 */
        "1:  lr.w    t0, (%1)                \n" /* t0 = lock->lock (load-reserved) */
        "   bnez    t0, 1b                   \n" /* if (t0 != 0) goto 1b (already locked) */
        "   sc.w    t0, %0, (%1)             \n" /* tmp = atomic_exchange(lock->lock, 1) */
        "   bnez    t0, 1b                   \n" /* if (t0 != 0) goto 1b (store failed, retry) */
        "   fence   rw, rw                   \n" /* full memory barrier */
        : "=&r"(tmp)
        : "r"(&lock->lock)
        : "memory", "t0");
}

static inline int
spin_trylock(spinlock_t *lock)
{
    uint32_t tmp, result;
    asm volatile(
        "   li      %0, 1                    \n" /* tmp = 1 */
        "   lr.w    t0, (%2)                 \n" /* t0 = lock->lock */
        "   bnez    t0, 1f                   \n" /* if locked, fail */
        "   sc.w    t0, %0, (%2)             \n" /* try to acquire */
        "   bnez    t0, 1f                   \n" /* if failed, retry */
        "   fence   rw, rw                   \n" /* barrier on success */
        "   li      %1, 0                    \n" /* result = 0 (success) */
        "   j       2f                       \n"
        "1:  li      %1, 1                    \n" /* result = 1 (failed) */
        "2:                                  \n"
        : "=&r"(tmp), "=&r"(result)
        : "r"(&lock->lock)
        : "memory", "t0");
    return result;
}

static inline void
spin_unlock(spinlock_t *lock)
{
    asm volatile(
        "   fence   rw, rw                   \n" /* memory barrier */
        "   sw      zero, (%0)               \n" /* lock->lock = 0 */
        :
        : "r"(&lock->lock)
        : "memory");
}

/* RISC-V 中断控制函数 */
static inline uint64_t
riscv_irq_save(void)
{
    uint64_t status;
    asm volatile(
        "   csrrci  %0, sstatus, 2           \n" /* read sstatus and clear SIE bit */
        : "=r"(status)
        :
        : "memory");
    return status;
}

static inline void
riscv_irq_restore(uint64_t status)
{
    asm volatile(
        "   csrw    sstatus, %0              \n" /* restore sstatus */
        :
        : "r"(status)
        : "memory");
}

/* 带中断保护的 spinlock */
static inline void
spin_lock_irqsave(spinlock_noirq_t *lock)
{
    lock->irq_flags = riscv_irq_save();
    spin_lock((spinlock_t *)lock);
}

static inline int
spin_trylock_irqsave(spinlock_noirq_t *lock)
{
    lock->irq_flags = riscv_irq_save();
    return spin_trylock((spinlock_t *)lock);
}

static inline void
spin_unlock_irqrestore(spinlock_noirq_t *lock)
{
    spin_unlock((spinlock_t *)lock);
    riscv_irq_restore(lock->irq_flags);
}

#endif  // RISCV64_SPIN_LOCK_IMPL_H
