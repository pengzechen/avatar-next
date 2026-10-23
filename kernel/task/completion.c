/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/task/completion.c — SMP 安全的一次性事件等待

   关键点(也是它存在的理由)：

     1. 条件检查与入等待队列在同一把锁下完成 —— 唤醒方不可能落在
        「检查完, 还没挂上」的缝隙里。
     2. 唤醒走 task_unblock() → sched_unblock()：如果等待者还在这颗核上
        执行(已置 BLOCKED, 尚未切出), 只置 wake_pending, 绝不提前入队。
     3. done 先置位再唤醒；等待者醒来重新检查 done, 因此不会丢事件。
*/

#include "task/completion.h"
#include "task/sched.h"
#include "task/task.h"
#include "klog.h"
#include "string.h"

void completion_init(completion_t *c)
{
    if (!c)
        return;
    spinlock_irq_init(&c->lock);
    list_init(&c->waiters);
    c->done = 0;
}

void completion_wait(completion_t *c)
{
    task_t *cur;
    uint64_t flags;

    if (!c)
        return;

    cur = task_current();
    if (!cur) {
        KLOG_ERROR("[completion] wait from non-task context\n");
        return;
    }

    spin_lock_irqsave(&c->lock, &flags);
    while (!c->done) {
        cur->state = TASK_BLOCKED;
        list_insert_last(&c->waiters, &cur->wait_node);
        spin_unlock_irqrestore(&c->lock, flags);

        /*
           等待者可能在这一刻被 complete：sched_unblock() 看到 on_cpu=true,
           只置 READY+wake_pending；本核的 sched_schedule() 在切出时兑现。
        */
        sched_schedule();

        spin_lock_irqsave(&c->lock, &flags);
    }
    spin_unlock_irqrestore(&c->lock, flags);
}

void completion_complete_all(completion_t *c)
{
    uint64_t flags;

    if (!c)
        return;

    spin_lock_irqsave(&c->lock, &flags);
    c->done = 1;

    while (!list_is_empty(&c->waiters)) {
        list_node_t *node = list_delete_first(&c->waiters);
        task_t *waiter = container_of(node, task_t, wait_node);

        /*
           锁序：completion.lock → 目标核 rq_lock。反方向不存在
           (sched_schedule / sched_unblock 都不会回头拿 completion.lock)。
        */
        task_unblock(waiter);
    }
    spin_unlock_irqrestore(&c->lock, flags);
}

int completion_done(completion_t *c)
{
    uint64_t flags;
    int done;

    if (!c)
        return 0;

    spin_lock_irqsave(&c->lock, &flags);
    done = c->done;
    spin_unlock_irqrestore(&c->lock, flags);
    return done;
}

int completion_reset(completion_t *c)
{
    uint64_t flags;

    if (!c)
        return -1;

    spin_lock_irqsave(&c->lock, &flags);
    if (!list_is_empty(&c->waiters)) {
        spin_unlock_irqrestore(&c->lock, flags);
        return -1;
    }
    c->done = 0;
    spin_unlock_irqrestore(&c->lock, flags);
    return 0;
}
