/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/task/rt_event.c — RT 事件位等待/置位

   等待队列与 bits 在同一把自旋锁下修改：检查条件、入队、置 BLOCKED
   与 set 方的置位、出队、唤醒不会互相落在对方的缝隙里。

   等待者对象放在 rt_event_wait() 的栈上（内核栈），不动态分配。
   事件 set 方在锁内只做 task_unblock()，所以 ISR 可以直接调用。
*/

#include "task/rt_event.h"
#include "task/task.h"
#include "task/sched.h"
#include "driver/time.h"
#include "barrier.h"

struct rt_event_waiter {
    list_node_t node;
    task_t *task;
    uint32_t mask;
    uint32_t matched; /* set 方判定满足时的 (bits & mask)，用于防丢唤醒 */
    bool wait_all;
};

static bool rt_event_satisfied(uint32_t bits, uint32_t mask, bool wait_all)
{
    return wait_all ? ((bits & mask) == mask) : ((bits & mask) != 0);
}

void rt_event_init(rt_event_t *e)
{
    spinlock_irq_init(&e->lock);
    list_init(&e->waiters);
    e->bits = 0;
}

uint32_t rt_event_get(rt_event_t *e)
{
    uint64_t flags;
    uint32_t bits;

    if (!e)
        return 0;

    spin_lock_irqsave(&e->lock, &flags);
    bits = e->bits;
    spin_unlock_irqrestore(&e->lock, flags);
    return bits;
}

void rt_event_set(rt_event_t *e, uint32_t bits)
{
    if (!e || bits == 0)
        return;

    uint64_t flags;
    spin_lock_irqsave(&e->lock, &flags);
    e->bits |= bits;

    list_node_t *node = list_first(&e->waiters);
    while (node) {
        list_node_t *next = list_node_next(node);
        struct rt_event_waiter *w = list_node_parent(node, struct rt_event_waiter, node);

        if (rt_event_satisfied(e->bits, w->mask, w->wait_all)) {
            w->matched = e->bits & w->mask;
            list_delete(&e->waiters, &w->node);
            /*
               锁序：event->lock → 目标核 rq_lock。和 completion/mutex 一致。
               等待者可能还在 CPU 上（已置 BLOCKED、尚未切出），
               sched_unblock 只置 wake_pending，不会重复入队。
            */
            task_unblock(w->task);
        }
        node = next;
    }

    spin_unlock_irqrestore(&e->lock, flags);
}

void rt_event_clear(rt_event_t *e, uint32_t bits)
{
    if (!e || bits == 0)
        return;

    uint64_t flags;
    spin_lock_irqsave(&e->lock, &flags);
    e->bits &= ~bits;
    spin_unlock_irqrestore(&e->lock, flags);
}

uint32_t rt_event_wait(rt_event_t *e, uint32_t mask, bool wait_all, uint64_t timeout_ns, bool clear)
{
    task_t *cur = task_current();

    if (!e || !cur || mask == 0)
        return 0;

    uint64_t deadline = timeout_ns ? (timer_get_ns() + timeout_ns) : 0;
    struct rt_event_waiter w;
    w.task = cur;
    w.mask = mask;
    w.matched = 0;
    w.wait_all = wait_all;
    list_node_init(&w.node);

    uint64_t flags;
    spin_lock_irqsave(&e->lock, &flags);

    for (;;) {
        if (rt_event_satisfied(e->bits, mask, wait_all)) {
            uint32_t matched = e->bits & mask;
            if (clear)
                e->bits &= ~mask;
            spin_unlock_irqrestore(&e->lock, flags);
            cur->sleep_expire_ns = 0;
            return matched;
        }

        if (deadline && timer_get_ns() >= deadline) {
            if (list_contains(&e->waiters, &w.node))
                list_delete(&e->waiters, &w.node);
            spin_unlock_irqrestore(&e->lock, flags);
            cur->sleep_expire_ns = 0;
            return 0;
        }

        if (!list_contains(&e->waiters, &w.node))
            list_insert_last(&e->waiters, &w.node);
        cur->state = TASK_BLOCKED;
        cur->sleep_expire_ns = deadline; /* 0 = 无限等待 */
        barrier_compiler();
        spin_unlock_irqrestore(&e->lock, flags);

        sched_schedule();

        spin_lock_irqsave(&e->lock, &flags);

        /*
           set 方判定满足后会把 matched 写好再唤醒。不能只看当前 bits：
           另一个 clear=true 的等待者可能已经先清掉了位，否则这次唤醒会被
           误判成“没满足”而重新睡下去。
        */
        if (w.matched) {
            uint32_t matched = w.matched;
            if (clear)
                e->bits &= ~mask;
            spin_unlock_irqrestore(&e->lock, flags);
            cur->sleep_expire_ns = 0;
            return matched;
        }

        /*
           set 方会先把节点摘掉再唤醒；超时/信号唤醒则节点还在，需要自己删。
           之后统一回到循环开头重新检查 bits 和 deadline。
        */
        if (list_contains(&e->waiters, &w.node))
            list_delete(&e->waiters, &w.node);
    }
}
