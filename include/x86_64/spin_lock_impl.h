#ifndef X86_64_SPIN_LOCK_IMPL_H
#define X86_64_SPIN_LOCK_IMPL_H

/*
 * x86_64 架构的spinlock实现
 * 使用 xchg 指令实现原子交换
 * 使用 mfence 指令进行内存屏障
 */

#include "task/preempt.h"
#include "x86_64/exception_impl.h"   /* arch_irq_save/restore（统一的中断屏蔽原语）*/

/* spinlock_t 和 spinlock_noirq_t 定义在 spinlock.h 中 */

/*
 * x86_64 的原子操作使用 xchg 指令
 * xchg 会自动锁定总线，保证原子性
 */
static inline void
spin_lock(spinlock_t *lock)
{
    uint64_t val = 1;
    preempt_disable();
    asm volatile(
        "1:  lock xchg %0, %1    \n" /* 原子交换：val = lock->lock; lock->lock = 1 */
        "   test %0, %0          \n" /* 测试 val 是否为 0 */
        "   jnz 1b               \n" /* 如果不为 0，说明锁占用，继续自旋 */
        "   mfence               \n" /* 内存屏障，确保后续操作有序 */
        : "+r"(val), "+m"(lock->lock)
        :
        : "memory", "cc");
}

static inline int
spin_trylock(spinlock_t *lock)
{
    uint64_t val = 1;
    uint64_t result;
    preempt_disable();
    asm volatile(
        "   lock xchg %0, %2    \n" /* 尝试获取锁 */
        "   test %0, %0          \n"
        "   mov $0, %1           \n" /* result = 0 */
        "   jnz 1f               \n" /* 如果非 0，跳转 */
        "   jmp 2f               \n"
        "1:  mov $1, %1          \n" /* result = 1 */
        "2:  mfence              \n"
        : "+r"(val), "=r"(result), "+m"(lock->lock)
        :
        : "memory", "cc");
    if (result != 0)
        preempt_enable();
    return result;
}

static inline void
spin_unlock(spinlock_t *lock)
{
    asm volatile(
        "   mfence               \n" /* 内存屏障 */
        "   movl $0, %0          \n" /* lock->lock = 0 */
        :
        : "m"(lock->lock)
        : "memory", "cc");
    preempt_enable();
}

/*
 * 带中断保护的 spinlock。
 *
 * 中断状态存在**调用点的局部变量**里（由 *flags 带回），不再存进锁对象 ——
 * 存进锁对象时，SMP 下争锁的另一颗 CPU 会把它的 flags 覆盖上去，解锁时
 * 恢复的就是别人的中断状态。中断原语本身统一来自
 * include/x86_64/exception_impl.h（arch_irq_save/restore）。
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

#endif  // X86_64_SPIN_LOCK_IMPL_H
