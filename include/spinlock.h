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
typedef struct
{
    volatile uint64_t lock;
} spinlock_t;

typedef struct
{
    volatile uint64_t lock;
    volatile uint64_t irq_flags;
} spinlock_noirq_t;

/* 初始化宏 */
#define SPINLOCK_INIT           \
    {                           \
        .lock = 0               \
    }
#define SPINLOCK_NOIRQ_INIT           \
    {                           \
        .lock = 0, .irq_flags = 0     \
    }

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
    lock->lock      = 0;
    lock->irq_flags = 0;
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
 * 带中断保护的自旋锁:
 *   spinlock_noirq_t lock = SPINLOCK_NOIRQ_INIT;
 *   spin_lock_irqsave(&lock);
 *   // 临界区（中断已禁用）
 *   spin_unlock_irqrestore(&lock);
 *
 * 非阻塞尝试:
 *   if (spin_trylock(&lock) == 0) {
 *       // 获取锁成功
 *       spin_unlock(&lock);
 *   }
 */

#endif  // SPINLOCK_H
