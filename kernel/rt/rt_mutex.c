/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/rt/rt_mutex.c — 带完整 PI 链的优先级继承睡眠锁

   等待队列按“等待优先级”降序排列（同优先级 FIFO）。任何任务阻塞在
   rt_mutex 上时，都会沿 owner 链把提升逐级传上去：

     H(80) 等 M2，M2 的 owner 是 M；M(50) 又阻塞在 M1 上，M1 的 owner 是 L
       => M 被直接提升到 80
       => L 沿链被传递提升到 80（没有链时 L 只会停在 50）

   实现选择：
     - 一把全局 PI 锁（g_rtm_pi_lock）保护整张“任务-锁-等待者”图。
       链式调整要同时改多个 owner 和等待队列，逐锁获取会引入环与竞态；
       本内核的 RT 锁规模很小，全局锁换来的是可证明的简单性。
     - 每个任务记 held_rt_mutexes（当前持有的锁）和 pi_blocked_on
       （正在等的锁）；任务的提升 = 它持有的所有锁里最高等待者优先级。
     - 释放一把锁时只需重算 holder 的提升，不需要再走链（释放者一定是
       正在运行的任务，不可能同时阻塞在另一把锁上）。

   锁序：g_rtm_pi_lock -> 目标核 rq_lock（sched_set_pi_boost / task_unblock
   内部）。调度器不会反向获取 g_rtm_pi_lock，因此不会形成环。

   已知限制：
     - 不做死锁检测（A→B→A 会真的睡死）；链深度上限 32，超限停止传播。
     - 持有者退出时不会自动释放锁（现有约束，见 RT_LOCKS.md）。
*/

#include "task/rt_mutex.h"
#include "task/task.h"
#include "task/sched.h"
#include "spinlock.h"
#include "klog.h"
#include "barrier.h"

/* 链深度上限：正常链远小于此值；超过基本可以断定成环。 */
#define RTM_PI_CHAIN_MAX 32

static spinlock_t g_rtm_pi_lock = SPINLOCK_INIT;

static uint8_t rtm_prio_of(const task_t *t)
{
    /* NORMAL 等待者不产生提升，按 0 排在队尾。 */
    return task_effective_rt_prio(t);
}

/* ── 持 g_rtm_pi_lock 的内部辅助 ─────────────────────────── */

/* 按等待优先级降序插入（同级 FIFO）。 */
static void rtm_insert_waiter(rt_mutex_t *m, task_t *t)
{
    list_node_t *pos = list_first(&m->waiters);

    while (pos) {
        task_t *w = list_node_parent(pos, task_t, wait_node);
        if (w->pi_waiter_prio < t->pi_waiter_prio)
            break;
        pos = list_node_next(pos);
    }
    list_insert_before(&m->waiters, pos, &t->wait_node);
}

/*
   任务从它当前持有的所有 rt_mutex 的等待者那里应得的最高提升。
   等待队列已按优先级降序，所以每把锁只看队首。
*/
static uint8_t rtm_task_boost(task_t *t)
{
    uint8_t max = 0;
    list_node_t *pos = list_first(&t->held_rt_mutexes);

    while (pos) {
        rt_mutex_t *m = list_node_parent(pos, rt_mutex_t, held_node);

        if (!list_is_empty(&m->waiters)) {
            task_t *w = list_node_parent(list_first(&m->waiters), task_t, wait_node);
            if (w->pi_waiter_prio > max)
                max = w->pi_waiter_prio;
        }
        pos = list_node_next(pos);
    }
    return max;
}

/* 重算 t 的继承优先级；返回是否发生变化。 */
static bool rtm_refresh_boost(task_t *t)
{
    uint8_t boost = rtm_task_boost(t);

    if (boost == t->pi_priority)
        return false;
    sched_set_pi_boost(t, boost);
    return true;
}

/*
   从 owner 开始沿“谁在等谁的锁”向上传递提升。

   每一层：
     1. 重算 owner 的 pi_priority（可能被它的等待者抬高）；
     2. 若没变化，链的其余部分也不会变，直接停；
     3. 若 owner 自己正阻塞在另一把锁上，把它在该锁等待队列里的
        等待优先级更新为新值、重新排序，再继续处理那把锁的 owner。
*/
static void rtm_propagate(task_t *owner)
{
    int depth = 0;

    while (owner && depth++ < RTM_PI_CHAIN_MAX) {
        if (owner->state == TASK_DEAD)
            break;
        if (!rtm_refresh_boost(owner))
            break;

        rt_mutex_t *blocked = owner->pi_blocked_on;
        if (!blocked)
            break;

        owner->pi_waiter_prio = rtm_prio_of(owner);
        if (list_contains(&blocked->waiters, &owner->wait_node))
            list_delete(&blocked->waiters, &owner->wait_node);
        rtm_insert_waiter(blocked, owner);
        owner = blocked->owner;
    }

    if (depth >= RTM_PI_CHAIN_MAX)
        KLOG_WARN_ONCE("[rt_mutex] PI chain too deep (cycle?), propagation stopped\n");
}

/* ── 公共 API ────────────────────────────────────────────── */

void rt_mutex_init(rt_mutex_t *m)
{
    m->owner = NULL;
    list_init(&m->waiters);
    list_node_init(&m->held_node);
}

int rt_mutex_lock(rt_mutex_t *m)
{
    task_t *cur = task_current();
    if (!cur)
        return -RTM_EINVAL;
    if (!m)
        return -RTM_EINVAL;

    uint64_t flags;
    spin_lock_irqsave(&g_rtm_pi_lock, &flags);

    /* 非递归：入口处自己已经持有就报错，不能走进等待队列等自己释放。 */
    if (m->owner == cur) {
        spin_unlock_irqrestore(&g_rtm_pi_lock, flags);
        return -RTM_EDEADLK;
    }

    for (;;) {
        if (!m->owner) {
            /* 可能被唤醒后抢到（上一个 holder 已释放，但没把锁交给我们）。 */
            if (list_contains(&m->waiters, &cur->wait_node))
                list_delete(&m->waiters, &cur->wait_node);
            m->owner = cur;
            cur->pi_blocked_on = NULL;
            cur->pi_waiter_prio = 0;
            if (!list_contains(&cur->held_rt_mutexes, &m->held_node))
                list_insert_last(&cur->held_rt_mutexes, &m->held_node);
            rtm_refresh_boost(cur);
            spin_unlock_irqrestore(&g_rtm_pi_lock, flags);
            return 0;
        }

        /*
           走到这里是阻塞等待后的返回：unlock 会把 owner 直接交给我们，
           并把锁挂进 held 链、算好剩余等待者带来的提升。
        */
        if (m->owner == cur) {
            cur->pi_blocked_on = NULL;
            cur->pi_waiter_prio = 0;
            spin_unlock_irqrestore(&g_rtm_pi_lock, flags);
            return 0;
        }

        if (!list_contains(&m->waiters, &cur->wait_node)) {
            cur->pi_waiter_prio = rtm_prio_of(cur);
            rtm_insert_waiter(m, cur);
        }
        cur->pi_blocked_on = m;
        cur->state = TASK_BLOCKED;

        /* 新等待者可能把整条 owner 链一路抬高。 */
        rtm_propagate(m->owner);

        /*
           入队与 BLOCKED 都在 PI 锁下发布；解锁方不会漏掉这次唤醒。
           若在切出前被唤醒，sched_unblock 只置 wake_pending，由
           sched_schedule() 补入队列。
        */
        barrier_compiler();
        spin_unlock_irqrestore(&g_rtm_pi_lock, flags);

        sched_schedule();

        spin_lock_irqsave(&g_rtm_pi_lock, &flags);
    }
}

bool rt_mutex_trylock(rt_mutex_t *m)
{
    task_t *cur = task_current();
    bool ok = false;

    if (!m || !cur)
        return false;

    uint64_t flags;
    spin_lock_irqsave(&g_rtm_pi_lock, &flags);
    if (!m->owner) {
        m->owner = cur;
        cur->pi_blocked_on = NULL;
        cur->pi_waiter_prio = 0;
        if (!list_contains(&cur->held_rt_mutexes, &m->held_node))
            list_insert_last(&cur->held_rt_mutexes, &m->held_node);
        rtm_refresh_boost(cur);
        ok = true;
    }
    spin_unlock_irqrestore(&g_rtm_pi_lock, flags);
    return ok;
}

bool rt_mutex_unlock(rt_mutex_t *m)
{
    task_t *cur = task_current();
    if (!m || !cur)
        return false;

    uint64_t flags;
    spin_lock_irqsave(&g_rtm_pi_lock, &flags);

    if (m->owner != cur) {
        spin_unlock_irqrestore(&g_rtm_pi_lock, flags);
        /*
           非 owner 解锁是 API 误用，但返回值 false 已经是契约的一部分；
           不要用 KLOG_ERROR —— RT_TEST 的负向用例会故意走这条路径，
           用户态误用也会把启动日志刷成“内核出错了”。DEBUG 留作诊断。
        */
        KLOG_DEBUG("[rt_mutex] task '%s' unlock without ownership\n", cur->name);
        return false;
    }

    task_t *next = NULL;
    if (!list_is_empty(&m->waiters)) {
        list_node_t *node = list_delete_first(&m->waiters);
        next = list_node_parent(node, task_t, wait_node);
    }

    /* 先把锁从当前 holder 的 held 链摘掉，再重算它的提升。 */
    if (list_contains(&cur->held_rt_mutexes, &m->held_node))
        list_delete(&cur->held_rt_mutexes, &m->held_node);

    if (next) {
        m->owner = next;
        next->pi_blocked_on = NULL;
        next->pi_waiter_prio = 0;
        if (!list_contains(&next->held_rt_mutexes, &m->held_node))
            list_insert_last(&next->held_rt_mutexes, &m->held_node);
    } else {
        m->owner = NULL;
    }

    /*
       先撤销本任务的继承，再唤醒新 owner：sched_kick_after_enqueue 需要
       用 cur 已恢复后的优先级做比较，否则“80 的等待者 <= 80 的旧 owner”
       会漏掉这次立即抢占。
    */
    rtm_refresh_boost(cur);

    if (next) {
        /* 新 owner 接住这把锁剩余等待者的提升，再入队才用得上。 */
        rtm_refresh_boost(next);

        /*
           锁序：g_rtm_pi_lock → rq_lock。task_unblock 只在目标核上入队/
           置 wake_pending，不会睡眠。
        */
        task_unblock(next);
    }

    spin_unlock_irqrestore(&g_rtm_pi_lock, flags);
    return true;
}

task_t *rt_mutex_owner(rt_mutex_t *m)
{
    task_t *owner;
    uint64_t flags;

    if (!m)
        return NULL;

    spin_lock_irqsave(&g_rtm_pi_lock, &flags);
    owner = m->owner;
    spin_unlock_irqrestore(&g_rtm_pi_lock, flags);
    return owner;
}
