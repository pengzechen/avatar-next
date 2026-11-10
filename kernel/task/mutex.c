/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/task/mutex.c — 睡眠锁(Mutex)实现
*/

#include "mutex.h"
#include "task.h"
#include "task/sched.h"
#include "task/switch.h"
#include "klog.h"
#include "arch.h"
#include "barrier.h"

/* ── mutex_init ──────────────────────────────────────────── */

void mutex_init(mutex_t *mutex)
{
    spinlock_irq_init(&mutex->lock);
    mutex->locked = false;
    list_init(&mutex->wait_queue);
    mutex->holder = NULL;
    mutex->count = 0;
}

/* ── mutex_lock ──────────────────────────────────────────── */

/*
   条件检查、入等待队列与置 BLOCKED 全在 mutex->lock 下完成。
   这是修掉旧实现丢唤醒的关键：旧代码先恢复中断、再 task_block()，
   解锁如果落在两者之间，等待者会睡进一把已经释放的锁。
*/
void mutex_lock(mutex_t *mutex)
{
    task_t *cur = task_current();
    uint64_t flags;

    spin_lock_irqsave(&mutex->lock, &flags);
    for (;;) {
        if (!mutex->locked) {
            mutex->locked = true;
            mutex->holder = cur;
            mutex->count = 1; /* 首次持有, 计数为 1 */
            break;
        }

        /* 可重入：同一任务再次加锁, 直接递增计数 */
        if (mutex->holder == cur) {
            mutex->count++;
            break;
        }

        cur->state = TASK_BLOCKED;
        list_insert_last(&mutex->wait_queue, &cur->wait_node);
        spin_unlock_irqrestore(&mutex->lock, flags);

        /*
           入队与 BLOCKED 已经发布，解锁方不会漏掉这次唤醒。
           若在真正切出前就被唤醒，sched_unblock 只置 wake_pending，
           由 sched_schedule() 在切出时补入队列（见 task/completion.c）。
        */
        sched_schedule();

        spin_lock_irqsave(&mutex->lock, &flags);
        /* 被唤醒后重新检查：可能已被别的任务抢到 */
    }
    spin_unlock_irqrestore(&mutex->lock, flags);
}

/* ── mutex_unlock ────────────────────────────────────────── */

void mutex_unlock(mutex_t *mutex)
{
    task_t *cur = task_current();
    task_t *waiter = NULL;
    uint64_t flags;

    spin_lock_irqsave(&mutex->lock, &flags);

    /* 检查是否是锁的持有者 */
    if (!mutex->locked || mutex->holder != cur) {
        KLOG_ERROR("[mutex] task '%s' (id=%u) trying to unlock unheld mutex!\n", cur->name, cur->id);
        spin_unlock_irqrestore(&mutex->lock, flags);
        return;
    }

    /* 可重入：计数仍大于 1, 只递减, 不真正释放 */
    if (--mutex->count > 0) {
        spin_unlock_irqrestore(&mutex->lock, flags);
        return;
    }

    /* 先从等待队列中取出第一个任务 */
    if (!list_is_empty(&mutex->wait_queue)) {
        list_node_t *node = list_delete_first(&mutex->wait_queue);
        waiter = container_of(node, task_t, wait_node);
    }

    /* 释放锁 */
    mutex->holder = NULL;
    barrier_compiler();
    mutex->locked = false;

    /*
       锁序：mutex->lock → 目标核 rq_lock（task_unblock 内部）。
       反方向不存在；completion.c 用同一顺序。
    */
    if (waiter != NULL)
        task_unblock(waiter);

    spin_unlock_irqrestore(&mutex->lock, flags);
}

/* ── mutex_trylock ───────────────────────────────────────── */

bool mutex_trylock(mutex_t *mutex)
{
    task_t *cur = task_current();
    bool success;
    uint64_t flags;

    spin_lock_irqsave(&mutex->lock, &flags);

    if (!mutex->locked) {
        mutex->locked = true;
        mutex->holder = cur;
        mutex->count = 1;
        success = true;
    } else if (mutex->holder == cur) {
        /* 可重入：已是持有者, 递增计数 */
        mutex->count++;
        success = true;
    } else {
        success = false;
    }

    spin_unlock_irqrestore(&mutex->lock, flags);
    return success;
}

/* ── mutex_is_locked ─────────────────────────────────────── */

bool mutex_is_locked(mutex_t *mutex)
{
    uint64_t flags;
    bool locked;

    spin_lock_irqsave(&mutex->lock, &flags);
    locked = mutex->locked;
    spin_unlock_irqrestore(&mutex->lock, flags);
    return locked;
}

/* ── mutex_holder ────────────────────────────────────────── */

task_t *mutex_holder(mutex_t *mutex)
{
    uint64_t flags;
    task_t *holder;

    spin_lock_irqsave(&mutex->lock, &flags);
    holder = mutex->holder;
    spin_unlock_irqrestore(&mutex->lock, flags);
    return holder;
}
