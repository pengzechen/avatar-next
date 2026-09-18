#include "task/preempt.h"
#include "arch.h"
#include "task/task.h"
#include "task/cpu.h"
#include "task/sched.h"

#include "exception.h"   /* arch_irq_is_enabled()（统一的中断屏蔽原语）*/

void preempt_disable(void)
{
    task_t *cur = cpu_current()->current_task;
    if (cur)
        cur->preempt_count++;
}

void preempt_enable(void)
{
    cpu_t *c = cpu_current();
    task_t *cur = c->current_task;
    if (cur && cur->preempt_count > 0)
        cur->preempt_count--;

    if (!cur || cur->preempt_count != 0 || c->irq_depth != 0 ||
        !c->need_resched || c->preempt_schedule_depth != 0)
        return;

    /* 如果当前仍处于 irqsave 临界区（中断被屏蔽），不能在这里切换。
     * 判断交给统一的 arch_irq_is_enabled()，不再按架构各写一遍。 */
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
