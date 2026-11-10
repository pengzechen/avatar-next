/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/task/rt_mutex.c — 直接优先级继承的睡眠锁

   等待队列按“有效 RT 优先级”降序排列（同优先级 FIFO）。等待者入队时
   把 owner 提升到不低于自己的优先级；释放时把锁交给队首（最高优先级）
   等待者，并把它的继承优先级设为剩余等待者的最高值。

   调度侧的提升由 sched_set_pi_boost() 完成：它会按需把 READY 任务在
   就绪队列里重新定位，并让被提升的任务在 tick 上按 FIFO 对待。

   锁序：rt_mutex->lock → 目标核 rq_lock（sched_set_pi_boost / task_unblock
   内部）。调度器不会反向获取 rt_mutex->lock，因此不会形成环。

   已知限制（V1）：
     - 只做直接继承。owner 自己阻塞在另一把 rt_mutex 上时，不会沿等待
       链继续提升（没有嵌套 PI 链）。
     - 不跟踪任务持有的全部 rt_mutex；多把锁同时有等待者时，释放其中
       一把会把继承优先级清零，可能丢失另一把锁带来的提升。
     - 不检测 A→B→A 死锁。
   需要完整 PI 链时再按 Linux rt_mutex 的 waiter/owner 链重做。
*/

#include "task/rt_mutex.h"
#include "task/task.h"
#include "task/sched.h"
#include "klog.h"
#include "barrier.h"

static uint8_t rtm_prio_of(const task_t *t)
{
    /* NORMAL 等待者不产生提升，按 0 排在队尾。 */
    return task_effective_rt_prio(t);
}

/* 持 m->lock 调用。 */
static void rtm_insert_waiter(rt_mutex_t *m, task_t *t)
{
    uint8_t p = rtm_prio_of(t);
    list_node_t *pos = list_first(&m->waiters);

    while (pos) {
        task_t *w = list_node_parent(pos, task_t, wait_node);
        if (rtm_prio_of(w) < p)
            break;
        pos = list_node_next(pos);
    }
    list_insert_before(&m->waiters, pos, &t->wait_node);
}

/* 持 m->lock 调用：owner 的提升只增不减，真正的恢复在 unlock。 */
static void rtm_boost_owner(rt_mutex_t *m, uint8_t prio)
{
    task_t *owner = m->owner;

    if (!owner || prio == 0)
        return;
    if (prio > task_effective_rt_prio(owner))
        sched_set_pi_boost(owner, prio);
}

void rt_mutex_init(rt_mutex_t *m)
{
    spinlock_irq_init(&m->lock);
    m->owner = NULL;
    list_init(&m->waiters);
}

int rt_mutex_lock(rt_mutex_t *m)
{
    task_t *cur = task_current();
    if (!cur)
        return -RTM_EINVAL;
    if (!m)
        return -RTM_EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&m->lock, &flags);

    /* 非递归：入口处自己已经持有就报错，不能走进等待队列等自己释放。 */
    if (m->owner == cur) {
        spin_unlock_irqrestore(&m->lock, flags);
        return -RTM_EDEADLK;
    }

    for (;;) {
        if (!m->owner) {
            /* 可能被唤醒后抢到（上一个 holder 已释放，但没把锁交给我们）。 */
            if (list_contains(&m->waiters, &cur->wait_node))
                list_delete(&m->waiters, &cur->wait_node);
            m->owner = cur;
            spin_unlock_irqrestore(&m->lock, flags);
            return 0;
        }

        /*
           走到这里是阻塞等待后的返回：unlock 会把 owner 直接交给我们。
       */
        if (m->owner == cur) {
            spin_unlock_irqrestore(&m->lock, flags);
            return 0;
        }

        if (!list_contains(&m->waiters, &cur->wait_node))
            rtm_insert_waiter(m, cur);
        rtm_boost_owner(m, rtm_prio_of(cur));

        /*
           入队与 BLOCKED 都在 m->lock 下发布；解锁方不会漏掉这次唤醒。
           若在切出前被唤醒，sched_unblock 只置 wake_pending，由
           sched_schedule() 补入队列。
        */
        cur->state = TASK_BLOCKED;
        barrier_compiler();
        spin_unlock_irqrestore(&m->lock, flags);

        sched_schedule();

        spin_lock_irqsave(&m->lock, &flags);
    }
}

bool rt_mutex_trylock(rt_mutex_t *m)
{
    task_t *cur = task_current();
    bool ok = false;

    if (!m || !cur)
        return false;

    uint64_t flags;
    spin_lock_irqsave(&m->lock, &flags);
    if (!m->owner) {
        m->owner = cur;
        ok = true;
    }
    spin_unlock_irqrestore(&m->lock, flags);
    return ok;
}

bool rt_mutex_unlock(rt_mutex_t *m)
{
    task_t *cur = task_current();
    if (!m || !cur)
        return false;

    uint64_t flags;
    spin_lock_irqsave(&m->lock, &flags);

    if (m->owner != cur) {
        spin_unlock_irqrestore(&m->lock, flags);
        KLOG_ERROR("[rt_mutex] task '%s' unlock without ownership\n", cur->name);
        return false;
    }

    task_t *next = NULL;
    if (!list_is_empty(&m->waiters)) {
        list_node_t *node = list_delete_first(&m->waiters);
        next = list_node_parent(node, task_t, wait_node);
    }

    if (next) {
        m->owner = next;

        /*
           新 owner 还要接住剩余等待者的提升；它自己已有的继承优先级
           （来自它持有的另一把 rt_mutex）只增不减。
        */
        uint8_t boost = 0;
        if (!list_is_empty(&m->waiters)) {
            task_t *w = list_node_parent(list_first(&m->waiters), task_t, wait_node);
            boost = rtm_prio_of(w);
        }
        uint8_t existing = __atomic_load_n(&next->pi_priority, __ATOMIC_RELAXED);
        if (boost > existing)
            sched_set_pi_boost(next, boost);
    } else {
        m->owner = NULL;
    }

    /*
       先撤销本任务的继承，再唤醒新 owner：sched_kick_after_enqueue 需要
       用 cur 已恢复后的优先级做比较，否则“80 的等待者 <= 80 的旧 owner”
       会漏掉这次立即抢占。
       多锁场景的已知限制见文件头。
    */
    sched_set_pi_boost(cur, 0);

    /*
       锁序：rt_mutex->lock → rq_lock。task_unblock 只在目标核上入队/
       置 wake_pending，不会睡眠。
    */
    if (next)
        task_unblock(next);

    spin_unlock_irqrestore(&m->lock, flags);
    return true;
}

task_t *rt_mutex_owner(rt_mutex_t *m)
{
    task_t *owner;
    uint64_t flags;

    if (!m)
        return NULL;

    spin_lock_irqsave(&m->lock, &flags);
    owner = m->owner;
    spin_unlock_irqrestore(&m->lock, flags);
    return owner;
}
