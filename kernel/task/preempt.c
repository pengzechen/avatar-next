#include "task/preempt.h"
#include "arch.h"
#include "task/task.h"
#include "task/cpu.h"
#include "task/sched.h"

#if ARCH_RISCV64
#include "riscv64/sysreg.h"
#define RV_SSTATUS_SIE  (1UL << 1)
#endif

#if ARCH_AARCH64
#define AARCH64_DAIF_I  (1UL << 7)
static inline uint64_t aarch64_read_daif(void)
{
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif) :: "memory");
    return daif;
}
#endif

#if ARCH_X86_64
#define X86_RFLAGS_IF  (1UL << 9)
static inline uint64_t x86_read_rflags(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
    return flags;
}
#endif

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

#if ARCH_RISCV64
    /* 如果当前仍处于 SIE=0 的 irqsave 临界区，不能在这里切换。 */
    if ((CSR_READ(sstatus) & RV_SSTATUS_SIE) == 0)
        return;
#elif ARCH_AARCH64
    /* 如果当前仍处于 DAIF.I=1 的 irqsave 临界区，不能在这里切换。 */
    if (aarch64_read_daif() & AARCH64_DAIF_I)
        return;
#elif ARCH_X86_64
    /* 如果当前仍处于 IF=0 的 irqsave 临界区，不能在这里切换。 */
    if ((x86_read_rflags() & X86_RFLAGS_IF) == 0)
        return;
#endif

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
