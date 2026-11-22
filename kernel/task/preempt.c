/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#include "task/preempt.h"
#include "arch.h"
#include "task/task.h"
#include "task/cpu.h"
#include "task/sched.h"

#include "exception.h" /* arch_irq_is_enabled()(统一的中断屏蔽原语) */

#ifdef RT_IRQ_TRACE
#include "rt/rt_latency.h"
#endif

void preempt_disable(void)
{
    task_t *cur = cpu_current()->current_task;
#ifdef RT_IRQ_TRACE
    /*
       在 preempt_disable 里取返回地址，得到的是它的调用者（spin_lock
       内联后就是真正持锁的函数），而不是 rt_latency_preempt_off_begin
       内部看到的本函数地址。
    */
    if (cur && cur->preempt_count == 0)
        rt_latency_preempt_off_begin((uint64_t)(uintptr_t)__builtin_return_address(0));
#endif
    if (cur)
        cur->preempt_count++;
}

void preempt_enable(void)
{
    cpu_t *c = cpu_current();
    task_t *cur = c->current_task;
#ifdef RT_IRQ_TRACE
    bool preempt_off_last = cur && cur->preempt_count == 1;
#endif
    if (cur && cur->preempt_count > 0)
        cur->preempt_count--;
#ifdef RT_IRQ_TRACE
    if (preempt_off_last)
        rt_latency_preempt_off_end();
#endif

    if (!cur || cur->preempt_count != 0 || c->irq_depth != 0 || !c->need_resched || c->preempt_schedule_depth != 0)
        return;

    /* 如果当前仍处于 irqsave 临界区(中断被屏蔽), 不能在这里切换。
       判断交给统一的 arch_irq_is_enabled(), 不再按架构各写一遍。
    */
    if (!arch_irq_is_enabled())
        return;

    c->need_resched = false;
    c->preempt_schedule_depth++;
    sched_schedule();
    c->preempt_schedule_depth--;
}

bool preemptible(void)
{
    cpu_t *c = cpu_current();
    task_t *cur = c->current_task;
    return c->irq_depth == 0 && (!cur || cur->preempt_count == 0);
}

bool in_irq_context(void)
{
    return cpu_current()->irq_depth != 0;
}
