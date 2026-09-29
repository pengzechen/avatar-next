/*
 * kernel/task/itimer.c — 每任务 ITIMER_REAL（alarm / setitimer）
 *
 * 语义与设计见 task/itimer.h。这里只记实现上的两个关键点：
 *
 * 1. 到期后**先重装再投信号**。
 *    周期定时器在投 SIGALRM 之前就把 expire 推到下一个周期。这样用户 handler
 *    里再调 setitimer 时看到的是一致状态，而不是被我们事后覆盖。
 *
 * 2. 唤醒阻塞中的任务。
 *    task_send_signal() 只往 pending_sigs 里 OR 一个位，**不会**把 TASK_BLOCKED
 *    的任务挪回就绪队列。而 alarm 的全部意义就在于打断阻塞中的 read/poll ——
 *    不显式 unblock，任务要等到下一个事件（比如管道来了数据）才看得到信号，
 *    "超时脱身"就永远不成立。实测 LTP kill02 正是卡在这里。
 *
 *    在 tick（ISR 上下文）里 unblock 是既有用法：sched_enqueue() 内部有
 *    in_irq_context() 分支，设备 ISR 唤醒任务是常规操作。
 *
 * 3. SMP：每个核的 tick 都会调到这里，所以同一个到期定时器可能被两个核同时
 *    看到。不加锁是**故意**的 —— 三个动作都是幂等的：置同一个 pending 位、
 *    把 expire 推到（近似）同一个新时刻、unblock 有 state != TASK_BLOCKED
 *    的早退。代价是周期定时器在某些 tick 上可能多投一次 SIGALRM。
 *    默认 SMP=1，多核下若要求严格"一个周期一个信号"，再考虑加单核仲裁。
 */

#include "task/itimer.h"

#include "timer/timer.h" /* timer_get_ns */

extern task_t g_task_pool[]; /* 定义在 task.c */
extern uint8_t g_stack_used[TASK_MAX];

static inline uint64_t itimer_now_ns(void)
{
    return timer_get_ns();
}

void itimer_set(task_t *t, uint64_t value_ns, uint64_t interval_ns)
{
    if (!t)
        return;

    t->itimer_interval_ns = interval_ns;
    t->itimer_expire_ns = value_ns ? (itimer_now_ns() + value_ns) : 0;
}

void itimer_get(task_t *t, uint64_t *remain_ns, uint64_t *interval_ns)
{
    if (!t)
        return;

    uint64_t remain = 0;
    if (t->itimer_expire_ns) {
        uint64_t now = itimer_now_ns();
        remain = (t->itimer_expire_ns > now) ? (t->itimer_expire_ns - now) : 0;
    }
    if (remain_ns)
        *remain_ns = remain;
    if (interval_ns)
        *interval_ns = t->itimer_interval_ns;
}

void itimer_tick(void)
{
    uint64_t now = itimer_now_ns();

    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (!g_stack_used[i])
            continue;

        task_t *t = &g_task_pool[i];
        if (t->itimer_expire_ns == 0 || now < t->itimer_expire_ns)
            continue;

        /* 已死/正在分配的槽不该再收信号；顺手把残值清掉，免得槽被复用后
         * 新任务莫名其妙挨一个 SIGALRM（复用的 TCB 会带着旧字段）。 */
        if (t->state == TASK_DEAD || t->state == TASK_ALLOCATING) {
            t->itimer_expire_ns = 0;
            t->itimer_interval_ns = 0;
            continue;
        }

        /* 先重装（周期）或清除（一次性），再投信号 —— 见文件头第 1 点 */
        if (t->itimer_interval_ns)
            t->itimer_expire_ns = now + t->itimer_interval_ns;
        else
            t->itimer_expire_ns = 0;

        task_send_signal(t, SIGALRM);

        /* 见文件头第 2 点：不 unblock 就叫不醒阻塞中的任务 */
        if (t->state == TASK_BLOCKED)
            task_unblock(t);
    }
}