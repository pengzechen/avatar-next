#ifndef SPINLOCK_H
#define SPINLOCK_H

/*
 * 跨架构 spinlock 抽象层
 * 根据架构自动选择对应的实现
 */

#include "types.h"
#include "arch.h"

/*
 * 通用数据结构定义
 * 所有架构共享相同的结构体布局
 */
/*
 * 只有一种锁结构。
 *
 * 以前的 spinlock_noirq_t 里还有一个 irq_flags 字段：spin_lock_irqsave()
 * 把调用者的中断状态写进**锁对象**，解锁时再读回来。那把锁被两个 CPU 争抢时，
 * 后写的 flags 会覆盖先写的 —— 解锁时恢复的是别人的中断状态（SMP 下会把
 * 中断错误地放着/关着）。现在中断状态由调用点的局部变量保存（Linux 风格）：
 *
 *     uint64_t flags;
 *     spin_lock_irqsave(&lock, &flags);
 *     ...
 *     spin_unlock_irqrestore(&lock, flags);
 */
typedef struct
{
    volatile uint64_t lock;
} spinlock_t;

/* 兼容别名：名字保留下来表达"这把锁配 irqsave 用"的意图，类型与 spinlock_t 相同 */
typedef spinlock_t spinlock_noirq_t;

/* 初始化宏 */
#define SPINLOCK_INIT           \
    {                           \
        .lock = 0               \
    }
#define SPINLOCK_NOIRQ_INIT  SPINLOCK_INIT

/*
 * 通用初始化函数（内联）
 * 所有架构的实现相同
 */
static inline void
spinlock_init(spinlock_t *lock)
{
    lock->lock = 0;
}

static inline void
spinlock_irq_init(spinlock_noirq_t *lock)
{
    lock->lock = 0;
}

/* 根据架构选择对应的实现 */
#if defined(ARCH_X86_64)
    #include "x86_64/spin_lock_impl.h"
#elif defined(ARCH_AARCH64)
    #include "aarch64/spin_lock_impl.h"
#elif defined(ARCH_RISCV64)
    #include "riscv64/spin_lock_impl.h"
#else
    #error "Unsupported architecture"
#endif

/*
 * 通用接口说明：
 *
 * 基本自旋锁:
 *   spinlock_t lock = SPINLOCK_INIT;
 *   spin_lock(&lock);
 *   // 临界区
 *   spin_unlock(&lock);
 *
 * 带中断保护的自旋锁（中断状态存在调用点的局部变量里，不要存进锁对象）:
 *   spinlock_noirq_t lock = SPINLOCK_NOIRQ_INIT;
 *   uint64_t flags;
 *   spin_lock_irqsave(&lock, &flags);
 *   // 临界区（中断已禁用）
 *   spin_unlock_irqrestore(&lock, flags);
 *
 * 非阻塞尝试:
 *   if (spin_trylock(&lock) == 0) {
 *       // 获取锁成功
 *       spin_unlock(&lock);
 *   }
 */

#endif  // SPINLOCK_H
