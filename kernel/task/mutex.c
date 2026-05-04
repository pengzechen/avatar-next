/*
 * kernel/task/mutex.c — 睡眠锁（Mutex）实现
 */

#include "mutex.h"
#include "task.h"
#include "task/sched.h"
#include "task/switch.h"
#include "klog.h"
#include "arch.h"
#include "barrier.h"

/* ── mutex_init ──────────────────────────────────────────── */

void
mutex_init(mutex_t *mutex)
{
    mutex->locked = false;
    list_init(&mutex->wait_queue);
    mutex->holder = NULL;
}

/* ── mutex_lock ──────────────────────────────────────────── */

void
mutex_lock(mutex_t *mutex)
{
    task_t *cur = task_current();

    /* 参数验证：mutex 必须是内核地址 */
    // if ((uintptr_t)mutex < 0xffffffc000000000) {
    //     KLOG_ERROR("[mutex_lock] FATAL: mutex=%p is user address!\n", mutex);
    //     KLOG_ERROR("[mutex_lock]   task='%s' pid=%u\n", cur->name, cur->id);
    //     platform_panic();
    // }

    /* 关中断，保护临界区 */
    uint64_t flags = arch_irq_save();

    /* 如果锁已被持有，进入等待队列 */
    while (mutex->locked) {
        barrier_compiler();  // 防止编译器重排，确保每次都重新读取 mutex->locked

        /* 检测死锁：同一任务重复获取 */
        if (mutex->holder == cur) {
            KLOG_ERROR("[mutex] DEADLOCK: task '%s' (id=%u) trying to re-acquire mutex!\n",
                      cur->name, cur->id);
            arch_irq_restore(flags);
            /* 死锁，死循环 */
            while (1) {
                task_yield();
            }
        }

        /* 加入等待队列并阻塞 */
        KLOG_DEBUG("[mutex] task '%s' (id=%u) waiting for mutex\n",
                  cur->name, cur->id);

        /* 恢复中断，允许调度器工作 */
        arch_irq_restore(flags);
        task_block(&mutex->wait_queue);
        /* task_block 会切换到其他任务，被唤醒后重新获取锁 */
        flags = arch_irq_save();

        /* 被唤醒后重新检查锁是否可用 */
    }

    /* 获取锁 */
    mutex->locked = true;
    barrier_compiler();  // 编译器屏障，确保操作顺序
    mutex->holder = cur;

    arch_irq_restore(flags);

    // KLOG_DEBUG("[mutex] task '%s' (id=%u) acquired mutex\n",
    //           cur->name, cur->id);
}

/* ── mutex_unlock ────────────────────────────────────────── */

void
mutex_unlock(mutex_t *mutex)
{
    task_t *cur = task_current();

    /* 关中断，保护临界区 */
    uint64_t flags = arch_irq_save();

    /* 检查是否是锁的持有者 */
    if (!mutex->locked || mutex->holder != cur) {
        KLOG_ERROR("[mutex] task '%s' (id=%u) trying to unlock unheld mutex!\n",
                  cur->name, cur->id);
        arch_irq_restore(flags);
        return;
    }

    // KLOG_DEBUG("[mutex] task '%s' (id=%u) releasing mutex\n",
    //           cur->name, cur->id);

    /* 先从等待队列中取出第一个任务（在锁释放前操作 wait_queue） */
    task_t *waiter = NULL;
    if (!list_is_empty(&mutex->wait_queue)) {
        list_node_t *node = list_delete_first(&mutex->wait_queue);
        waiter = container_of(node, task_t, wait_node);

        KLOG_DEBUG("[mutex] waking task '%s' (id=%u)\n",
                  waiter->name, waiter->id);
    }

    /* 释放锁（现在可以安全释放了，wait_queue 已操作完毕） */
    mutex->holder = NULL;
    barrier_compiler();  // 编译器屏障，确保操作顺序
    mutex->locked = false;

    /* 恢复中断 */
    arch_irq_restore(flags);

    /* 最后唤醒等待的任务（锁已释放，任务可以立即竞争） */
    if (waiter != NULL) {
        task_unblock(waiter);
    }
}

/* ── mutex_trylock ───────────────────────────────────────── */

bool
mutex_trylock(mutex_t *mutex)
{
    task_t *cur = task_current();
    bool success;

    /* 关中断，保护临界区 */
    uint64_t flags = arch_irq_save();

    /* 如果锁未被持有，获取它 */
    if (!mutex->locked) {
        mutex->locked = true;
        mutex->holder = cur;
        success = true;

        KLOG_DEBUG("[mutex] task '%s' (id=%u) trylock succeeded\n",
                  cur->name, cur->id);
    } else {
        success = false;

        KLOG_DEBUG("[mutex] task '%s' (id=%u) trylock failed\n",
                  cur->name, cur->id);
    }

    arch_irq_restore(flags);

    return success;
}

/* ── mutex_is_locked ─────────────────────────────────────── */

bool
mutex_is_locked(mutex_t *mutex)
{
    return mutex->locked;
}

/* ── mutex_holder ────────────────────────────────────────── */

task_t *
mutex_holder(mutex_t *mutex)
{
    return mutex->holder;
}
