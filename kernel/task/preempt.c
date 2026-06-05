#include "task/preempt.h"
#include "arch.h"

#if ARCH_RISCV64
#include "task/task.h"
#include "task/cpu.h"
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
    task_t *cur = cpu_current()->current_task;
    if (cur && cur->preempt_count > 0)
        cur->preempt_count--;
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
