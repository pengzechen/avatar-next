/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/time/hrtimer.c — 高精度睡眠/唤醒（x86_64 先用）

   定时事件 = min(下一个 100Hz tick, 本核最早的 task->sleep_expire_ns)。
   x86 用 LAPIC TSC-deadline 精确武装；其它架构不支持 oneshot 时，
   仍然靠 100Hz tick 的 itimer_tick() 唤醒。

   时间基准用 timer_get_ns()（x86 = TSC 换算）。
*/

#include "hrtimer.h"
#include "arch.h"
#include "driver/time.h"
#include "exception.h" /* arch_irq_save/restore */
#include "task/task.h"
#include "task/cpu.h"

extern task_t g_task_pool[];
extern uint8_t g_stack_used[];

#if ARCH_X86_64
/* 由 timer_x86_64_impl.h 实现 */
extern bool timer_arch_oneshot_supported(void);
extern void timer_arch_program_oneshot_ns(uint64_t deadline_ns);
#else
bool timer_arch_oneshot_supported(void)
{
    return false;
}

void timer_arch_program_oneshot_ns(uint64_t deadline_ns)
{
    (void)deadline_ns;
}
#endif

static uint64_t g_next_tick_ns[AVATAR_MAX_CPUS];
static bool g_oneshot[AVATAR_MAX_CPUS];

static uint64_t hrtimer_tick_period_ns(void)
{
    uint64_t ms = g_timer_cfg_tick_ms ? g_timer_cfg_tick_ms : 10ULL;
    return ms * 1000000ULL;
}

static uint32_t hrtimer_cpu_id(void)
{
    uint32_t cpu = get_current_cpu_id();
    return (cpu < AVATAR_MAX_CPUS) ? cpu : 0U;
}

static uint64_t hrtimer_next_deadline(uint32_t cpu, uint64_t now)
{
    uint64_t next = g_next_tick_ns[cpu];
    if (next == 0)
        next = now + hrtimer_tick_period_ns();

    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (!g_stack_used[i])
            continue;
        task_t *t = &g_task_pool[i];
        if (t->cpu_affinity != cpu)
            continue;
        uint64_t d = t->sleep_expire_ns;
        if (d != 0 && d < next)
            next = d;
    }
    return next;
}

static void hrtimer_program_next(uint32_t cpu, uint64_t now)
{
    if (!g_oneshot[cpu])
        return;

    uint64_t next = hrtimer_next_deadline(cpu, now);
    if (next <= now)
        next = now + 1; /* 已经过期：尽量立刻触发 */
    timer_arch_program_oneshot_ns(next);
}

static void hrtimer_expire_sleepers(uint32_t cpu, uint64_t now)
{
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (!g_stack_used[i])
            continue;
        task_t *t = &g_task_pool[i];
        if (t->cpu_affinity != cpu)
            continue;
        uint64_t d = t->sleep_expire_ns;
        if (d == 0 || now < d)
            continue;

        t->sleep_expire_ns = 0;
        if (t->state == TASK_BLOCKED)
            task_unblock(t);
    }
}

void hrtimer_cpu_init(void)
{
    uint32_t cpu = hrtimer_cpu_id();
    uint64_t now = timer_get_ns();

    g_oneshot[cpu] = timer_arch_oneshot_supported();
    g_next_tick_ns[cpu] = now + hrtimer_tick_period_ns();

    if (g_oneshot[cpu])
        hrtimer_program_next(cpu, now);
}

bool hrtimer_isr(void)
{
    uint64_t now = timer_get_ns();
    uint32_t cpu = hrtimer_cpu_id();
    uint64_t period = hrtimer_tick_period_ns();
    bool tick_due = false;

    if (g_next_tick_ns[cpu] == 0)
        g_next_tick_ns[cpu] = now + period;

    if (now >= g_next_tick_ns[cpu]) {
        tick_due = true;
        g_next_tick_ns[cpu] += period;
        /* 被长关中断拖后时不要补跑一串 tick；重新对齐即可。 */
        if (now >= g_next_tick_ns[cpu])
            g_next_tick_ns[cpu] = now + period;
    }

    hrtimer_expire_sleepers(cpu, now);
    hrtimer_program_next(cpu, now);
    return tick_due;
}

void hrtimer_sleep_until(uint64_t deadline_ns)
{
    task_t *cur = task_current();
    if (!cur)
        return;

    uint32_t cpu = hrtimer_cpu_id();

    if (!g_oneshot[cpu]) {
        while (timer_get_ns() < deadline_ns) {
            cur->sleep_expire_ns = deadline_ns;
            task_block(NULL);
        }
        cur->sleep_expire_ns = 0;
        return;
    }

    /*
       关中断做“登记 deadline + 武装 LAPIC + 睡眠”这三步：
       否则定时器可能在任务真正 BLOCKED 之前到期，ISR 看到 RUNNING 的任务
       只清标记不唤醒，随后任务睡下去就永远醒不来。
       task_block() 内部会自己 arch_irq_save/restore；外层保持关中断即可。
    */
    uint64_t flags = arch_irq_save();

    cur->sleep_expire_ns = deadline_ns;
    hrtimer_program_next(cpu, timer_get_ns());

    while (cur->sleep_expire_ns != 0 && timer_get_ns() < deadline_ns)
        task_block(NULL);

    cur->sleep_expire_ns = 0;
    arch_irq_restore(flags);
}

bool hrtimer_oneshot_supported(void)
{
    return g_oneshot[hrtimer_cpu_id()];
}

bool hrtimer_cpu_oneshot_supported(uint32_t cpu)
{
    return cpu < AVATAR_MAX_CPUS && g_oneshot[cpu];
}
