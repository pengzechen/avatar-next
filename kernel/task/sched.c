/*
 * kernel/task/sched.c — 内核轮转调度器
 *
 * 设计：
 *   - 就绪队列（g_run_queue）：list_t，FIFO 顺序
 *   - idle 任务（g_idle）：队列为空时的回退任务，永不入队
 *   - sched_schedule() 关中断保护调度决策和上下文切换
 *
 * 调度语义（以 sched_schedule 为核心）：
 *   1. 关中断，保存当前中断标志（flags）
 *   2. 若当前任务 state == RUNNING 且不是 idle，则将其改为 READY 并入队尾
 *   3. 从队头取下一任务；队空则选 idle
 *   4. 若 next == prev，则恢复 RUNNING 状态，开中断，返回（无切换）
 *   5. 将 next 设为 RUNNING，更新 g_current_task
 *   6. arch_task_switch() — 切换上下文；此行 "返回" 时已是 prev 再次被调度
 *   7. arch_irq_restore(flags) — 恢复 prev 进入调度时的中断状态
 *
 * 关于中断状态的正确性：
 *   - 若 prev 是被 timer ISR 抢占：flags = "关中断"；恢复后仍关，
 *     然后通过 ISR 的 eret/sret/iretq 还原到被中断前（开中断）的状态。
 *   - 若 prev 是主动 yield：flags = "开中断"；恢复后直接开中断。
 *   - 新任务（首次运行）：arch_task_switch 跳转到 task_trampoline，
 *     trampoline 调用 arch_irq_enable() 显式开中断。
 */

#include "task/sched.h"
#include "task/switch.h"
#include "klog.h"
#include "barrier.h"

/* ── Scheduler state ─────────────────────────────────────── */

static list_t   g_run_queue;   /* 就绪任务队列（不含 idle）*/
static task_t  *g_idle;        /* idle 任务，队空时运行    */
static volatile bool g_need_resched = false;  /* 需要重新调度的标志 */

/* ── sched_init ──────────────────────────────────────────── */

void
sched_init(task_t *idle_task)
{
    list_init(&g_run_queue);
    g_idle = idle_task;
}

/* ── sched_enqueue ───────────────────────────────────────── */

void
sched_enqueue(task_t *task)
{
    list_node_init(&task->run_node);
    list_insert_last(&g_run_queue, &task->run_node);
}

/* ── sched_dequeue ───────────────────────────────────────── */

void
sched_dequeue(task_t *task)
{
    if (list_contains(&g_run_queue, &task->run_node)) {
        list_delete(&g_run_queue, &task->run_node);
    }
}

/* ── 内部：选择下一个任务 ─────────────────────────────────── */

static task_t *
pick_next(void)
{
    list_node_t *node = list_delete_first(&g_run_queue);
    if (node) {
        task_t *task = container_of(node, task_t, run_node);
        KLOG_TRACE("[sched] pick_next: selected '%s' (id=%u, is_user=%d)\n",
                  task->name, task->id, task->is_user_process);
        return task;
    }
    // KLOG_DEBUG("[sched] pick_next: queue empty, returning idle\n");
    return g_idle; /* 队列为空，回退到 idle */
}

/* ── sched_schedule ──────────────────────────────────────── */

void
sched_schedule(void)
{
    /* 关中断，保存当前中断标志 */
    uint64_t flags = arch_irq_save();

    task_t *prev = g_current_task;

    /* 若当前任务仍在运行且不是 idle，则重新入队尾 */
    if (prev->state == TASK_RUNNING && prev != g_idle) {
        prev->state = TASK_READY;
        list_insert_last(&g_run_queue, &prev->run_node);
    }

    task_t *next = pick_next();

    /* 无需切换（唯一任务或队空只有 idle） */
    if (next == prev) {
        prev->state = TASK_RUNNING;
        arch_irq_restore(flags);
        return;
    }

    next->state    = TASK_RUNNING;
    barrier_compiler();  // 确保 state 在 g_current_task 之前完成
    g_current_task = next;
    barrier_compiler();  // 确保 g_current_task 在 arch_task_switch 之前完成

    KLOG_DEBUG("[sched] switch: prev='%s' (id=%u) -> next='%s' (id=%u, is_user=%d)\n",
              prev->name, prev->id, next->name, next->id, next->is_user_process);
    if (next->is_user_process) {
        KLOG_INFO("[sched] entering user task: '%s' (id=%u) pgd=0x%llx entry_sp=0x%lx\n",
                  next->name, next->id, (uint64_t)next->pgd, (unsigned long)next->sp);
    }

    /*
     * 切换上下文。
     * 对 prev：保存被调用者寄存器 + SP 到 prev->sp，然后跳走。
     * 当 prev 再次被调度时，arch_task_switch 从这里"返回"。
     * 此时 flags 在 prev 的栈帧中，中断仍关闭。
     *
     * 同时切换页表（如果任务有独立页表）。
     *
     * 特殊处理：如果切换到 idle，传递一个特殊标记（next->sp == (uintptr_t)-1）
     * 让 arch_task_switch 知道不要返回，而是直接进入 idle 循环。
     * 这样可以切断调用链，防止从用户进程退出时的异常帧被后续中断返回恢复。
     */
    uintptr_t switch_sp = next->sp;
    if (next == g_idle) {
        switch_sp = (uintptr_t)-1;  /* 特殊标记：切换到 idle */
    }
#if ARCH_RISCV64
    extern uint64_t g_kernel_pgd_phys;
    uint64_t *next_pgd_for_switch = next->pgd;
    if (next->is_user_process && !next->user_started) {
        KLOG_INFO("[sched] first user entry: defer satp switch to arch_switch_to_user (next_pgd=0x%llx)\n",
                  (uint64_t)next->pgd);
        next_pgd_for_switch = NULL;
    } else if (!next->is_user_process && next->pgd == NULL && g_kernel_pgd_phys != 0) {
        /* 切换到内核任务：恢复内核页表 */
        next_pgd_for_switch = (uint64_t *)g_kernel_pgd_phys;
    }
    arch_task_switch(&prev->sp, switch_sp, &prev->pgd, next_pgd_for_switch);
#else
    arch_task_switch(&prev->sp, switch_sp, &prev->pgd, next->pgd);
#endif

    // KLOG_DEBUG("[sched] returned to prev='%s' (id=%u)\n", prev->name, prev->id);

    /* prev 被恢复后恢复其中断状态 */
    arch_irq_restore(flags);
}

/* ── sched_tick ──────────────────────────────────────────── */

void
sched_tick(void)
{
    /*
     * 由 timer ISR 调用。
     * 只设置需要重调度的标志，实际切换在异常返回前进行。
     * 这样可以避免在 IRQ 处理程序中直接切换上下文。
     */
    g_need_resched = true;
}

/* ── sched_check_and_yield ─────────────────────────────────── */

/*
 * 由异常返回路径调用，检查是否需要重新调度。
 * 如果需要，执行任务切换。
 * 返回 true 表示发生了切换，false 表示没有。
 */
bool
sched_check_and_yield(void)
{
    if (g_need_resched) {
        g_need_resched = false;
        sched_schedule();
        return true;
    }
    return false;
}
