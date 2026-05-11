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
#include "string.h"

#if ARCH_X86_64
#include "mmu.h"
#include "../../boot/x86_64/tss.h"

#define X86_MSR_IA32_FS_BASE 0xC0000100U

static inline void x86_write_msr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)(value & 0xFFFFFFFFU);
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

static inline void x86_write_fs_base(uint64_t fs_base)
{
    x86_write_msr(X86_MSR_IA32_FS_BASE, fs_base);
}
#endif

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
        // KLOG_TRACE("[sched] pick_next: selected '%s' (id=%u, is_user=%d)\n",
        //           task->name, task->id, task->is_user_process);
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

#if ARCH_RISCV64
    /* RISC-V：切换到任务的页表
     * - 用户任务：切换到用户页表（已包含内核映射）
     * - 内核任务：切换回内核页表
     */
    if (next->is_user_process && next->pgd != 0) {
        uint64_t pgd_phys = (uint64_t)next->pgd;
        uint64_t ppn = pgd_phys >> 12;
        uint64_t satp = (8ULL << 60) | ppn;  /* MODE=Sv39, ASID=0 */
        __asm__ volatile("csrw satp, %0" : : "r"(satp));
        __asm__ volatile("sfence.vma");
    } else if (next->is_user_process == 0 && prev->is_user_process != 0) {
        /* 从用户任务切换到内核任务：恢复内核页表 */
        /* rv_l2_root物理地址 = 0x80205000 */
        uint64_t kernel_ppn = 0x80205;  /* 内核L1页表的PPN */
        uint64_t satp = (8ULL << 60) | kernel_ppn;
        __asm__ volatile("csrw satp, %0" : : "r"(satp));
        __asm__ volatile("sfence.vma");
    }
    /* 用户→用户切换，已在上面处理；内核→内核切换，页表不变 */
#elif ARCH_X86_64
    /* x86_64：通过 CR3 切换页表
     * - 用户任务：切换到用户页表（已包含内核高半区映射）
     * - 内核任务：切换回内核页表
     */
    static uint64_t g_kernel_cr3 = 0;
    
    /* 首次调度时保存内核 CR3 */
    if (g_kernel_cr3 == 0) {
        g_kernel_cr3 = read_cr3();
    }
    
    if (next->is_user_process && next->pgd != 0) {
        /* 切换到用户页表 */
        uint64_t pgd_phys = (uint64_t)next->pgd;
        // KLOG_INFO("[sched] switching CR3 to user PGD=0x%llx for '%s'\n", 
        //            pgd_phys, next->name);
        write_cr3(pgd_phys);
        
        /* 验证 CR3 切换 */
        uint64_t cr3_after = read_cr3();
        // KLOG_INFO("[sched] CR3 after switch = 0x%llx (expected 0x%llx)\n",
        //           cr3_after, pgd_phys);
        
        if ((cr3_after & 0xFFFFFFFFF000ULL) != pgd_phys) {
            KLOG_ERROR("[sched] CR3 switch FAILED!\n");
        } else {
            // KLOG_INFO("[sched] CR3 switched successfully, testing memory access...\n");
            /* 测试内核代码是否可访问（读取当前指令） */
            volatile uint64_t test = *(volatile uint64_t *)&sched_schedule;
            // KLOG_INFO("[sched] Memory access test passed, code accessible: 0x%llx\n", test);
        }
        
        /* 更新 TSS.RSP0 为当前任务的内核栈顶 */
        /* 任务的内核栈顶 = 栈基址 + 栈大小 */
        uint64_t kernel_stack_top = (uint64_t)next->stack_base + TASK_STACK_SIZE;
        x86_tss_set_rsp0(kernel_stack_top);

        /* 恢复该用户任务的 TLS 基址（fs:offset） */
        x86_write_fs_base(next->fs_base);

        KLOG_DEBUG("[sched] Updated TSS.RSP0 to 0x%llx for task '%s'\n",
                  kernel_stack_top, next->name);
    } else if (next->is_user_process == 0 && prev->is_user_process != 0) {
        /* 从用户任务切换到内核任务：恢复内核页表 */
        KLOG_DEBUG("[sched] restoring kernel CR3=0x%llx\n", g_kernel_cr3);
        write_cr3(g_kernel_cr3);
    }
    /* 用户→用户切换，已在上面处理；内核→内核切换，页表不变 */
#endif

    KLOG_DEBUG("[sched] switch: prev='%s' (id=%u) -> next='%s' (id=%u)\n",
              prev->name, prev->id, next->name, next->id);

    if (next->is_user_process) {
        KLOG_DEBUG("[sched] next='%s': entry=0x%llx sp=0x%llx kernel_sp=0x%llx\n",
                   next->name, next->user_entry, next->user_sp, next->sp);
    }

    /*
     * 切换上下文。
     * 对 prev：保存被调用者寄存器 + SP 到 prev->sp，然后跳走。
     * 当 prev 再次被调度时，arch_task_switch 从这里"返回"。
     * 此时 flags 在 prev 的栈帧中，中断仍关闭。
     *
     * 同时切换页表（如果任务有独立页表）。
     */
    uintptr_t switch_sp = next->sp;

    /* 注意：之前 AArch64 使用 sp=-1 标记切换到 idle，但这导致 
     * .Lswitch_to_idle 没有设置栈指针，造成潜在的栈溢出问题。
     * 现在所有架构统一：idle 有专用栈，正常返回即可。*/

#if ARCH_RISCV64
    extern uint64_t g_kernel_pgd_phys;
    uint64_t *next_pgd_for_switch = next->pgd;
    if (next->is_user_process && !next->user_started) {
        /* 首次进入用户进程：延迟satp切换到arch_switch_to_user */
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
    /*
     * 仅在用户进程上下文触发 tick 抢占：
     * - 内核初始化/内核任务路径保持非抢占，避免破坏临界流程
     * - 用户态仍可被时钟中断抢占，实现时间片轮转
     */
    if (!g_current_task || !g_current_task->is_user_process) {
        return false;
    }

    if (g_need_resched) {
        g_need_resched = false;
        sched_schedule();
        return true;
    }
    return false;
}
