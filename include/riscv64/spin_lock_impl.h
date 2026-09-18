#ifndef RISCV64_SPIN_LOCK_IMPL_H
#define RISCV64_SPIN_LOCK_IMPL_H

#include "types.h"
#include "spinlock.h"
#include "task/preempt.h"
#include "riscv64/exception_impl.h"   /* arch_irq_save/restore（统一的中断屏蔽原语）*/

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
    preempt_disable();
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
    preempt_disable();
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
    if (result != 0)
        preempt_enable();
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
    preempt_enable();
}

/*
 * 带中断保护的 spinlock。
 *
 * 中断状态存在**调用点的局部变量**里（由 *flags 带回），不再存进锁对象 ——
 * 存进锁对象时，SMP 下争锁的另一颗 CPU 会把它的 flags 覆盖上去，解锁时
 * 恢复的就是别人的中断状态。中断原语本身统一来自
 * include/riscv64/exception_impl.h（arch_irq_save/restore）。
 */

static inline void
spin_lock_irqsave(spinlock_t *lock, uint64_t *flags)
{
    *flags = arch_irq_save();
    spin_lock(lock);
}

static inline int
spin_trylock_irqsave(spinlock_t *lock, uint64_t *flags)
{
    *flags = arch_irq_save();
    if (spin_trylock(lock) == 0)
        return 0;
    arch_irq_restore(*flags);
    return 1;
}

static inline void
spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags)
{
    spin_unlock(lock);
    arch_irq_restore(flags);
}

#endif  // RISCV64_SPIN_LOCK_IMPL_H
