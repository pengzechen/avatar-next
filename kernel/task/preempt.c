#include "task/preempt.h"
#include "arch.h"

#if ARCH_RISCV64
#include "task/task.h"
#include "task/cpu.h"
#include "task/sched.h"
#include "riscv64/sysreg.h"
#define RV_SSTATUS_SIE  (1UL << 1)
#endif

void preempt_disable(void)
{
#if ARCH_RISCV64
    task_t *cur = cpu_current()->current_task;
    if (cur)
        cur->preempt_count++;
#endif
}

void preempt_enable(void)
{
#if ARCH_RISCV64
    cpu_t *c = cpu_current();
    task_t *cur = c->current_task;
    if (cur && cur->preempt_count > 0)
        cur->preempt_count--;

    if (!cur || cur->preempt_count != 0 || c->irq_depth != 0 ||
        !c->need_resched || c->preempt_schedule_depth != 0)
        return;

    /*
     * 如果当前仍处于 SIE=0 的 irqsave 临界区，不能在这里切换。
     * 恢复 SIE 后 pending timer 会重新进 trap，再由 trap 出口处理。
     */
    if ((CSR_READ(sstatus) & RV_SSTATUS_SIE) == 0)
        return;

    c->need_resched = false;
    c->preempt_schedule_depth++;
    sched_schedule();
    c->preempt_schedule_depth--;
#endif
}

bool preemptible(void)
{
#if ARCH_RISCV64
    cpu_t *c = cpu_current();
    task_t *cur = c->current_task;
    return c->irq_depth == 0 && (!cur || cur->preempt_count == 0);
#else
    return true;
#endif
}

bool in_irq_context(void)
{
#if ARCH_RISCV64
    return cpu_current()->irq_depth != 0;
#else
    return false;
#endif
}
