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
#include "task/cpu.h"
#include "klog.h"
#include "barrier.h"
#include "string.h"
#include "spinlock.h"
#include "task/preempt.h"
#if ARCH_RISCV64
#include "riscv64/satp_utils.h"
#include "riscv64/exception.h"
extern uint64_t g_kernel_pgd_phys;
#endif

#if ARCH_X86_64
#include "mmu.h"
#include "../../boot/x86_64/tss.h"
extern uint64_t g_kernel_pgd_phys;

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

/* ── Scheduler state ─────────────────────────────────────── *
 *
 * Phase 1: 调度状态全部迁入 cpu_t，本文件不再持有全局变量。
 *   - run_queue / idle_task / need_resched 都从 cpu_current() 读取
 *   - g_current_task 暂保留为 CPU0 的镜像（Phase 3 移除）
 *
 * 单核场景 cpu_current() 始终返回 &g_cpus[0]，行为与旧版本完全一致。
 * 多核场景每个 CPU 独立持有自己的就绪队列；跨核入队需要持有目标核
 *   的 rq_lock（Phase 2/4 启用，本阶段单核暂不加锁，因为关中断已经
 *   足够互斥本核 ISR）。
 */

/* ── sched_init ──────────────────────────────────────────── */

void
sched_init(task_t *idle_task)
{
    /* run_queue 已由 cpu_init_bsp() 初始化为空链表，这里只装 idle。*/
    cpu_t *c = cpu_current();
    c->idle_task    = idle_task;
    c->current_task = idle_task;   /* 与 g_current_task 同步 */
}

/* ── sched_enqueue ───────────────────────────────────────── */

/* Phase 4a SMP 分发计数器：atomic round-robin。 */
static uint32_t g_rr_counter;

void
sched_enqueue(task_t *task)
{
    uint32_t n = g_num_cpus ? g_num_cpus : 1U;
    uint32_t target;
    if (n == 1U) {
        target = 0U;
        task->cpu_affinity = 0U;
    } else if (task->cpu_affinity == CPU_AFFINITY_ANY) {
        target = __atomic_fetch_add(&g_rr_counter, 1U, __ATOMIC_RELAXED) % n;
        task->cpu_affinity = target;
    } else if (task->cpu_affinity >= n) {
        target = 0U;
        task->cpu_affinity = target;
    } else {
        target = task->cpu_affinity;
    }

    cpu_t *tc = &g_cpus[target];
    list_node_init(&task->run_node);

    if (n == 1U) {
        list_insert_last(&tc->run_queue, &task->run_node);
        return;
    }

    /* 跨核入队：持目标核 rq_lock（spin_lock_irqsave 内部关本核 IRQ）。 */
    spin_lock_irqsave(&tc->rq_lock);
    list_insert_last(&tc->run_queue, &task->run_node);
    spin_unlock_irqrestore(&tc->rq_lock);
}

/* ── sched_dequeue ───────────────────────────────────────── */

void
sched_dequeue(task_t *task)
{
    /* 任务以其 cpu_affinity 为准处于某核 rq。先查 affinity 对应核，
     * 找不到再退一步遍历所有核（防 affinity 字段与实际不一致）。 */
    uint32_t n = g_num_cpus ? g_num_cpus : 1U;
    if (n == 1U) {
        cpu_t *tc = &g_cpus[0];
        uint64_t flags = arch_irq_save();
        if (list_contains(&tc->run_queue, &task->run_node))
            list_delete(&tc->run_queue, &task->run_node);
        arch_irq_restore(flags);
        return;
    }

    if (task->cpu_affinity < n) {
        cpu_t *tc = &g_cpus[task->cpu_affinity];
        spin_lock_irqsave(&tc->rq_lock);
        if (list_contains(&tc->run_queue, &task->run_node)) {
            list_delete(&tc->run_queue, &task->run_node);
            spin_unlock_irqrestore(&tc->rq_lock);
            return;
        }
        spin_unlock_irqrestore(&tc->rq_lock);
    }
    for (uint32_t i = 0; i < n; i++) {
        cpu_t *tc = &g_cpus[i];
        spin_lock_irqsave(&tc->rq_lock);
        if (list_contains(&tc->run_queue, &task->run_node)) {
            list_delete(&tc->run_queue, &task->run_node);
            spin_unlock_irqrestore(&tc->rq_lock);
            return;
        }
        spin_unlock_irqrestore(&tc->rq_lock);
    }
}

/* ── 内部：选择下一个任务 ─────────────────────────────────── */

static task_t *
pick_next(cpu_t *c)
{
    list_node_t *node = list_delete_first(&c->run_queue);
    if (node) {
        task_t *task = container_of(node, task_t, run_node);
        return task;
    }
    return c->idle_task; /* 队列为空，回退到本核 idle */
}

/* ── sched_schedule ──────────────────────────────────────── */

void
sched_schedule(void)
{
    /* 关中断，保存当前中断标志 */
    uint64_t flags = arch_irq_save();

    /* 缓存本核指针：irq_save 之后 cpu 不会变，避免重复读 per-CPU 寄存器。 */
    cpu_t *c = cpu_current();

    /*
     * Phase 3：prev 一律从 per-CPU 的 c->current_task 取，**不**再用
     * g_current_task —— 后者只是 CPU0 的镜像，AP 上读它会拿到别人的任务。
     */
    task_t *prev = c->current_task;

    /* 持本核 rq_lock 期间操作 run_queue。外层已 arch_irq_save 关本核 IRQ，
     * 这里用 raw spin_lock 避免 spin_unlock_irqrestore 过早开 IRQ。
     * 跨核 sched_enqueue 持的是同一把锁，所以 list 操作原子。 */
    bool use_rq_lock = (g_num_cpus > 1U);
    if (use_rq_lock)
        spin_lock((spinlock_t *)&c->rq_lock);

    /* 若当前任务仍在运行且不是 idle，则重新入队尾 */
    if (prev->state == TASK_RUNNING && prev != c->idle_task) {
        prev->state = TASK_READY;
        list_insert_last(&c->run_queue, &prev->run_node);
    }

    task_t *next = pick_next(c);

    if (use_rq_lock)
        spin_unlock((spinlock_t *)&c->rq_lock);

    /* 无需切换（唯一任务或队空只有 idle） */
    if (next == prev) {
        prev->state = TASK_RUNNING;
        arch_irq_restore(flags);
        return;
    }

    next->state     = TASK_RUNNING;
    barrier_compiler();  // 确保 state 在 current_task 之前完成
    c->current_task = next;             /* Phase 1：per-CPU 主存储 */
    /*
     * Phase 3：g_current_task 镜像仅在 CPU0 维护。AP 不写，避免污染 BSP
     * 路径（fork/exec/tty 还在读 g_current_task，AP 上无业务执行这些）。
     */
    if (c->cpu_id == 0) {
        g_current_task = next;
    }
    barrier_compiler();  // 确保 current_task 在 arch_task_switch 之前完成

#if ARCH_X86_64
    /* x86_64：通过 CR3 切换页表
     * - 用户任务：切换到用户页表（已包含内核高半区映射）
     * - 内核任务：切换回内核页表
     */
    if (next->is_user_process && next->pgd != 0) {
        /* 切换到用户页表 */
        write_cr3((uint64_t)next->pgd);

        /* 更新 TSS.RSP0 为当前任务的内核栈顶 */
        uint64_t kernel_stack_top = (uint64_t)next->stack_base + TASK_STACK_SIZE;
        x86_tss_set_rsp0(kernel_stack_top);

        /* 恢复该用户任务的 TLS 基址（fs:offset） */
        x86_write_fs_base(next->fs_base);

        KLOG_DEBUG("[sched] CR3\u21920x%llx TSS.RSP0=0x%llx task='%s'\n",
                  (uint64_t)next->pgd, kernel_stack_top, next->name);
    } else if (!next->is_user_process && prev->is_user_process) {
        /* 从用户任务切换到内核任务：恢复内核页表 */
        KLOG_DEBUG("[sched] restoring kernel PGD=0x%llx\n", g_kernel_pgd_phys);
        write_cr3(g_kernel_pgd_phys);
    }
    /* 用户→用户切换，已在上面处理；内核→内核切换，页表不变 */
#endif

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
    uint64_t *next_pgd_for_switch = next->pgd;
    if (next->is_user_process && !next->user_started) {
        /* 首次进入用户进程：延迟satp切换到arch_switch_to_user */
        next_pgd_for_switch = NULL;

    } else if (!next->is_user_process && next->pgd == NULL && g_kernel_pgd_phys != 0) {
        /* 切换到内核任务：恢复内核页表 */
        next_pgd_for_switch = (uint64_t *)g_kernel_pgd_phys;
    }
    /*
     * RISC-V 用户进程：pgd 是创建时固定的物理地址，不需要由 arch_task_switch
     * 动态保存。若传 &prev->pgd，当用户进程在 arch_switch_to_user 切换 satp
     * 之前被抢占（satp 仍为内核页表）时，prev->pgd 会被覆写为内核页表地址，
     * 导致下次调度回来时以错误的 satp 进入 U-mode → VA 0x10000 不在内核页表 → Inst PF。
     * 因此对用户进程传 NULL（跳过 satp 保存），保持 pgd 不变。
     */
    uint64_t **prev_pgd_save = prev->is_user_process ? NULL : &prev->pgd;
    arch_task_switch(&prev->sp, switch_sp, prev_pgd_save, next_pgd_for_switch);
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
     *
     * Phase 1：标志已是 per-CPU，每核独立计数。
     */
    cpu_current()->need_resched = true;
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
    cpu_t *c = cpu_current();
    if (!c->current_task) {
        return false;
    }
    /*
     * 注意：不再限制仅用户进程可被抢占。idle 任务（is_user_process=false）
     * 也需要被 timer 抢占，以便在就绪队列中出现任务时立即切换。
     * 若 pick_next() 返回 idle 自身，sched_schedule() 会跳过切换。
     */

    if (c->need_resched) {
        c->need_resched = false;
        sched_schedule();
        return true;
    }
    return false;
}

bool
sched_check_and_yield_from_trap(void *frame_ptr)
{
#if ARCH_RISCV64
    trap_frame_t *frame = (trap_frame_t *)frame_ptr;
    if (frame && (frame->sstatus & SSTATUS_SPP)) {
        cpu_t *c = cpu_current();
        if (!c->current_task || !c->need_resched || !preemptible())
            return false;

        c->need_resched = false;
        sched_schedule();
        return true;
    }
#else
    (void)frame_ptr;
#endif
    return sched_check_and_yield();
}
