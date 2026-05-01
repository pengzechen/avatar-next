#ifndef X86_64_SPIN_LOCK_IMPL_H
#define X86_64_SPIN_LOCK_IMPL_H

/*
 * x86_64 架构的spinlock实现
 * 使用 xchg 指令实现原子交换
 * 使用 mfence 指令进行内存屏障
 */

/* spinlock_t 和 spinlock_noirq_t 定义在 spinlock.h 中 */

/*
 * x86_64 的原子操作使用 xchg 指令
 * xchg 会自动锁定总线，保证原子性
 */
static inline void
spin_lock(spinlock_t *lock)
{
    uint64_t val = 1;
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
}

/* x86_64 中断控制函数 */
static inline uint64_t
x86_64_irq_save(void)
{
    uint64_t flags;
    asm volatile(
        "   pushfq                \n" /* 保存 RFLAGS 到栈 */
        "   popq %0               \n" /* 弹出到 flags 变量 */
        "   cli                   \n" /* 清除 IF 标志，禁用中断 */
        : "=r"(flags)
        :
        : "memory", "cc");
    return flags;
}

static inline void
x86_64_irq_restore(uint64_t flags)
{
    asm volatile(
        "   pushq %0              \n" /* 将 flags 压栈 */
        "   popfq                 \n" /* 恢复 RFLAGS */
        :
        : "r"(flags)
        : "memory", "cc");
}

/* 带中断保护的 spinlock */
static inline void
spin_lock_irqsave(spinlock_noirq_t *lock)
{
    lock->irq_flags = x86_64_irq_save();
    spin_lock((spinlock_t *)lock);
}

static inline int
spin_trylock_irqsave(spinlock_noirq_t *lock)
{
    lock->irq_flags = x86_64_irq_save();
    return spin_trylock((spinlock_t *)lock);
}

static inline void
spin_unlock_irqrestore(spinlock_noirq_t *lock)
{
    spin_unlock((spinlock_t *)lock);
    x86_64_irq_restore(lock->irq_flags);
}

#endif  // X86_64_SPIN_LOCK_IMPL_H
