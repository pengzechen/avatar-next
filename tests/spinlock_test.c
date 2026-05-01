/*
 * Spinlock 跨架构使用示例
 *
 * 编译方式:
 *   AArch64:  gcc -D__aarch64__ -I../include -c spinlock_test.c
 *   RISC-V64: gcc -D__riscv -march=rv64gc -I../include -c spinlock_test.c
 */

#include "spinlock.h"

/* 全局锁示例 */
static spinlock_t global_lock = SPINLOCK_INIT;
static spinlock_noirq_t irq_lock = SPINLOCK_NOIRQ_INIT;

/* 共享资源示例 */
static int shared_counter = 0;

/*
 * 基本自旋锁使用示例
 */
void
example_basic_lock(void)
{
    spin_lock(&global_lock);

    /* 临界区 - 访问共享资源 */
    shared_counter++;

    spin_unlock(&global_lock);
}

/*
 * 带中断保护的自旋锁使用示例
 * 在中断处理程序中也可能访问的共享数据必须使用这种锁
 */
void
example_lock_with_irq(void)
{
    spin_lock_irqsave(&irq_lock);

    /* 临界区 - 中断已禁用，安全访问共享数据 */
    shared_counter++;

    spin_unlock_irqrestore(&irq_lock);
}

/*
 * trylock 使用示例
 */
int
example_trylock(void)
{
    if (spin_trylock(&global_lock) == 0) {
        /* 成功获取锁 */
        shared_counter++;
        spin_unlock(&global_lock);
        return 0;
    } else {
        /* 锁已被占用，执行其他操作 */
        return -1;
    }
}

/*
 * 嵌套锁使用示例（注意：避免死锁！）
 */
void
example_nested_locks(void)
{
    spinlock_t lock1 = SPINLOCK_INIT;
    spinlock_t lock2 = SPINLOCK_INIT;

    /* 始终按照相同顺序获取多个锁，避免死锁 */
    spin_lock(&lock1);
    spin_lock(&lock2);

    /* 临界区 */
    shared_counter++;

    /* 按相反顺序释放锁 */
    spin_unlock(&lock2);
    spin_unlock(&lock1);
}

/*
 * 初始化示例（动态分配的锁）
 */
void
example_dynamic_lock(void)
{
    /* 动态分配示例 */
    /* spinlock_t *my_lock = malloc(sizeof(spinlock_t)); */
    /* spinlock_init(my_lock); */
    /* spin_lock(my_lock); */
    /* // 使用锁 */
    /* spin_unlock(my_lock); */
    /* free(my_lock); */

    /* 栈上分配示例 */
    /* spinlock_t lock; */
    /* spinlock_init(&lock); */
    /* spin_lock(&lock); */
    /* // 使用锁 */
    /* spin_unlock(&lock); */

    (void)0; /* 避免空函数警告 */
}
