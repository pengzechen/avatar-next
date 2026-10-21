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
 *   5. 将 next 设为 RUNNING，更新本 CPU current_task
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
#include "assert.h"
#include "task/preempt.h"
#include "task/itimer.h"
#if ARCH_AARCH64 || ARCH_X86_64
#include "exception.h"
#endif
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
    __asm__ volatile("wrmsr" ::"c"(msr), "a"(lo), "d"(hi));
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
 *   - current_task 是 per-CPU 状态，不再维护全局镜像
 *
 * 单核场景 cpu_current() 始终返回 &g_cpus[0]，行为与旧版本完全一致。
 * 多核场景每个 CPU 独立持有自己的就绪队列；跨核入队需要持有目标核
 *   的 rq_lock（Phase 2/4 启用，本阶段单核暂不加锁，因为关中断已经
 *   足够互斥本核 ISR）。
 */

/* ── sched_init ──────────────────────────────────────────── */

void sched_init(task_t *idle_task)
{
    /* run_queue 已由 cpu_init_bsp() 初始化为空链表，这里只装 idle。*/
    cpu_t *c = cpu_current();
    c->idle_task = idle_task;
    c->current_task = idle_task;
    idle_task->on_cpu = true;
}

/* ── 跨核 resched ────────────────────────────────────────── */

/*
 * sched_resched_cpu — 让目标核立刻重新调度
 *
 * 只置标志 + 发 IPI，不直接操作目标核的队列：真正的切换由目标核在
 * 「中断返回路径」或「vCPU 主循环顶部」完成。目标正在跑 guest 时，
 * IPI 会被硬件路由成一次 VM-exit / HS trap，VMM 退出后自然回到
 * sched_check_and_yield()。
 *
 * 这是「一颗核让另一颗核上的 vCPU 任务停下来」的唯一正确姿势 ——
 * 不要从远端直接改 task->state 或调用 task_exit()。
 */
void sched_resched_cpu(uint32_t cpu_id)
{
    cpu_t *c;

    if (cpu_id >= g_num_cpus || cpu_id >= AVATAR_MAX_CPUS)
        return;

    c = &g_cpus[cpu_id];
    if (!c->online)
        return;

    c->need_resched = true;
    smp_wmb(); /* 标志必须先于 IPI 对目标核可见 */

    if (cpu_id != get_current_cpu_id())
        cpu_send_ipi(cpu_id);
}

void task_kick(task_t *task)
{
    if (!task || task->state != TASK_RUNNING)
        return;
    if (task->cpu_affinity >= g_num_cpus)
        return;
    sched_resched_cpu(task->cpu_affinity);
}

/* ── sched_enqueue ───────────────────────────────────────── */

/* Phase 4a SMP 分发计数器：atomic round-robin。 */
static uint32_t g_rr_counter;

void sched_enqueue(task_t *task)
{
    /*
     * 防御：正在运行的任务不在任何队列里，把它再挂进队列会让它被两个核
     * 同时执行（任务栈/状态互相踩）。合法的调用者只会入队 READY/BLOCKED
     * 的任务 —— 走到这里说明有人破坏了「谁在哪」的不变量，宁可漏入队
     * （任务下次被唤醒时还会再试）也不要写坏运行队列。
     */
    if (task->state == TASK_RUNNING) {
        KLOG_ERROR(
            "[sched] enqueue RUNNING task '%s' id=%u (affinity=%u) —— 拒绝\n",
            task->name, task->id, task->cpu_affinity);
        return;
    }

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
        /*
         * 中断上下文里**绝不要**调 arch_irq_save()/arch_irq_restore()。
         * 那一对是用「整字写回 sstatus」实现关/开中断的；在 ISR 里写回
         * sstatus 会破坏它的状态，实测症状是 S 模式中断从此不再投递 ——
         * 定时器停摆、串口无响应、被唤醒的任务再也跑不起来。
         *
         * 设备 ISR 里唤醒任务是常规操作（收包中断叫醒 net-poll 就是），
         * 而中断本来就是关的，直接插队列即可，不需要再动 sstatus。
         */
        if (in_irq_context()) {
            list_insert_last(&tc->run_queue, &task->run_node);
            return;
        }

        uint64_t flags = arch_irq_save();
        list_insert_last(&tc->run_queue, &task->run_node);
        arch_irq_restore(flags);
        return;
    }

    /*
     * 跨核入队：持目标核 rq_lock。
     *
     * 中断上下文里**不要**用 spin_lock_irqsave：RISC-V 的 arch_irq_save/
     * restore 是整字写 sstatus，在 ISR 里写回会破坏中断状态（见本函数
     * 单核分支的注释）。中断本来就已经关了，用裸 spin_lock 即可。
     */
    uint64_t flags;
    bool irq_ctx = in_irq_context();
    if (irq_ctx)
        spin_lock(&tc->rq_lock);
    else
        spin_lock_irqsave(&tc->rq_lock, &flags);
    list_insert_last(&tc->run_queue, &task->run_node);
    if (irq_ctx)
        spin_unlock(&tc->rq_lock);
    else
        spin_unlock_irqrestore(&tc->rq_lock, flags);

    /*
     * 远端入队后立刻 kick。没有这一步，任务要等目标核下一次 timer tick
     * （最长 10ms）才被看到；对 vCPU 的唤醒/停止来说这个延迟不可接受。
     */
    sched_resched_cpu(target);
}

/* ── sched_unblock ───────────────────────────────────────── */

/*
 * sched_unblock — 唤醒 BLOCKED 任务，且保证「同一任务不会同时在跑又在队列」
 *
 * 旧实现是 `state = READY; sched_enqueue()`。SMP 下有一个真实窗口：
 * 目标任务在另一颗核上刚置了 BLOCKED、还没执行到 sched_schedule 的切出点；
 * 此时唤醒方把它入队，第三颗核就可能把它挑走 —— 同一个任务被两颗核执行。
 *
 * 现在的规则：
 *   - on_cpu == true  → 只置 READY + wake_pending，由它自己的调度器在切出
 *                       的那一刻补入队列；
 *   - on_cpu == false → 直接入队；
 *   - 入队到远端核时发 resched IPI。
 *
 * 调用方必须保证目标任务确实在等待（例如已挂在 wait queue 上），否则
 * state 检查会让这次唤醒变成 no-op。
 */
void sched_unblock(task_t *task)
{
    uint32_t n, target;
    cpu_t *tc;
    uint64_t flags;
    bool enqueued = false;

    if (!task)
        return;

    n = g_num_cpus ? g_num_cpus : 1U;
    target = (task->cpu_affinity < n) ? task->cpu_affinity : 0U;
    tc = &g_cpus[target];

    bool irq_ctx = in_irq_context();

    if (irq_ctx) {
        /* 中断上下文：本核 IRQ 已关，不能动 sstatus */
        spin_lock(&tc->rq_lock);
    } else if (n == 1U) {
        flags = arch_irq_save();
    } else {
        spin_lock_irqsave(&tc->rq_lock, &flags);
    }

    if (task->state != TASK_BLOCKED) {
        if (irq_ctx)
            spin_unlock(&tc->rq_lock);
        else if (n == 1U)
            arch_irq_restore(flags);
        else
            spin_unlock_irqrestore(&tc->rq_lock, flags);
        return;
    }

    task->state = TASK_READY;
    if (task->on_cpu) {
        /* 还在目标核上：切出时由 sched_schedule() 补入队列 */
        task->wake_pending = true;
    } else {
        list_insert_last(&tc->run_queue, &task->run_node);
        enqueued = true;
    }

    if (irq_ctx)
        spin_unlock(&tc->rq_lock);
    else if (n == 1U)
        arch_irq_restore(flags);
    else
        spin_unlock_irqrestore(&tc->rq_lock, flags);

    if (enqueued && n > 1U)
        sched_resched_cpu(target);
}

/* ── sched_dequeue ───────────────────────────────────────── */

void sched_dequeue(task_t *task)
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
        uint64_t flags;
        spin_lock_irqsave(&tc->rq_lock, &flags);
        if (list_contains(&tc->run_queue, &task->run_node)) {
            list_delete(&tc->run_queue, &task->run_node);
            spin_unlock_irqrestore(&tc->rq_lock, flags);
            return;
        }
        spin_unlock_irqrestore(&tc->rq_lock, flags);
    }
    for (uint32_t i = 0; i < n; i++) {
        cpu_t *tc = &g_cpus[i];
        uint64_t flags;
        spin_lock_irqsave(&tc->rq_lock, &flags);
        if (list_contains(&tc->run_queue, &task->run_node)) {
            list_delete(&tc->run_queue, &task->run_node);
            spin_unlock_irqrestore(&tc->rq_lock, flags);
            return;
        }
        spin_unlock_irqrestore(&tc->rq_lock, flags);
    }
}

/* ── 内部：选择下一个任务 ─────────────────────────────────── */

static task_t *pick_next(cpu_t *c)
{
    extern task_t g_task_pool[]; /* 定义在 task.c（非 static） */

    list_node_t *node = list_delete_first(&c->run_queue);
    if (node) {
        task_t *task = container_of(node, task_t, run_node);

        /*
         * 队列里捡出来的指针必须落在任务池里。落到池外 = 运行队列已经被
         * 写坏（历史上真出现过：CR2=0xffffffffffffffc8 的野指针）。
         * 这时候**不要**继续用这个指针（一写就崩），跳过它并让本核跑 idle ——
         * 坏的是一个节点，整机还能继续跑，日志里也留下了现场。
         */
        if (task < g_task_pool || task >= g_task_pool + TASK_MAX) {
            KLOG_ERROR("[sched] CORRUPT runqueue on cpu%u: node=%p -> task=%p "
                       "(out of pool) —— 跳过\n",
                       c->cpu_id, (void *)node, (void *)task);
            return c->idle_task;
        }
        return task;
    }
    return c->idle_task; /* 队列为空，回退到本核 idle */
}

/* ── sched_schedule ──────────────────────────────────────── */

void sched_schedule(void)
{
    assert_always(!in_irq_context());

    /* 关中断，保存当前中断标志 */
    uint64_t flags = arch_irq_save();

    /* 缓存本核指针：irq_save 之后 cpu 不会变，避免重复读 per-CPU 寄存器。 */
    cpu_t *c = cpu_current();

    /* prev 一律从 per-CPU 的 c->current_task 取。 */
    task_t *prev = c->current_task;

    /* 持本核 rq_lock 期间操作 run_queue。外层已 arch_irq_save 关本核 IRQ，
     * 这里用不带 irqsave 的 spin_lock —— 中断状态由外层管理，不能用
     * spin_unlock_irqrestore 提前把 IRQ 打开。
     * 跨核 sched_enqueue 持的是同一把锁，所以 list 操作原子。
     * （以前这里要把 spinlock_noirq_t* 强转成 spinlock_t* 才能用 raw 变体；
     *  锁类型收敛后不需要了。） */
    bool use_rq_lock = (g_num_cpus > 1U);
    if (use_rq_lock)
        spin_lock(&c->rq_lock);

    /* 若当前任务仍在运行且不是 idle，则重新入队尾 */
    if (prev->state == TASK_RUNNING && prev != c->idle_task) {
        prev->state = TASK_READY;
        list_insert_last(&c->run_queue, &prev->run_node);
    }

    task_t *next = pick_next(c);

    /*
     * 切出 prev 之前收口「on_cpu 期间被唤醒」的窗口。
     *
     * waker 看到 on_cpu=true 时只置 wake_pending，不会入队（否则任务会
     * 被第二颗核抢走）。这里在 rq_lock 下把 on_cpu 清掉，并兑现那次唤醒。
     * 之后 waker 再看到的就是 on_cpu=false，会直接入队 —— 两边持同一把
     * 锁，不存在「清了 on_cpu 但没人入队」的缝隙。
     */
    if (next != prev) {
        prev->on_cpu = false;
        if (prev->wake_pending) {
            prev->wake_pending = false;
            prev->state = TASK_READY;
            list_insert_last(&c->run_queue, &prev->run_node);
            /*
             * 这次唤醒是在 prev 还 on_cpu 时发生的，waker 没有发 IPI
             * （`sched_unblock` 的 enqueued=false 分支）。现在 prev 已经
             * 入队，但接下来要切到的可能是 idle —— 不置 need_resched 的话
             * idle 会直接 wfi/hlt，任务要等下一次 tick 才被看到；tick 若
             * 异常就是永久睡死。本核自己入队，不需要 IPI。
             */
            c->need_resched = true;
        }

        /* state/on_cpu 也在同一把锁下发布，避免 waker 看到中间态 */
        next->state = TASK_RUNNING;
        next->on_cpu = true;
    }

    if (use_rq_lock)
        spin_unlock(&c->rq_lock);

    /* 无需切换（唯一任务或队空只有 idle） */
    if (next == prev) {
        prev->state = TASK_RUNNING;
        arch_irq_restore(flags);
        return;
    }

    barrier_compiler();     // 确保 state 在 current_task 之前完成
    c->current_task = next; /* Phase 1：per-CPU 主存储 */
    barrier_compiler();     // 确保 current_task 在 arch_task_switch 之前完成

#if ARCH_X86_64
    /* x86_64：通过 CR3 切换页表
     * - 用户任务：切换到用户页表（已包含内核高半区映射）
     * - 内核任务：切换回内核页表
     */
    if (next->is_user_process && next->pgd != 0) {
        /* 切换到用户页表 */
        write_cr3((uint64_t)next->pgd);

        /* 更新 TSS.RSP0 为当前任务的内核栈顶 */
        uint64_t kernel_stack_top =
            (uint64_t)next->stack_base + TASK_STACK_SIZE;
        x86_tss_set_rsp0(kernel_stack_top);

        /* 恢复该用户任务的 TLS 基址（fs:offset） */
        x86_write_fs_base(next->fs_base);

    } else if (!next->is_user_process && prev->is_user_process) {
        /* 从用户任务切换到内核任务：恢复内核页表 */
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

    } else if (!next->is_user_process && next->pgd == NULL &&
               g_kernel_pgd_phys != 0) {
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

    /* prev 被恢复后恢复其中断状态 */
    arch_irq_restore(flags);
}

/* ── sched_tick ──────────────────────────────────────────── */

void sched_tick(void)
{
    /*
     * 由 timer ISR 调用。
     * 只设置需要重调度的标志，实际切换在异常返回前进行。
     * 这样可以避免在 IRQ 处理程序中直接切换上下文。
     *
     * Phase 1：标志已是 per-CPU，每核独立计数。
     */
    cpu_t *c = cpu_current();

    c->need_resched = true;

    /*
     * idle 记账。挂在 tick 上而不是各个 idle 循环里：g_tick_cb 每个核都会调，
     * 一处就覆盖了 task_idle_loop 和三个架构各自的次级核 idle 循环。
     * /proc/uptime 的第二列靠它，否则永远是 0（top/vmstat 会当成"从不空闲"）。
     */
    if (c->current_task && c->current_task == c->idle_task)
        c->idle_ticks++;

    /*
     * 每任务的 ITIMER_REAL 到期检查也挂在这个 tick 上（100 Hz）。
     * 放在这里而不是另开一个回调：g_tick_cb 只有一个，而 sched_tick 既被
     * timer 驱动经 timer_set_tick_cb 调用，也被 x86_64 的 fallback 路径
     * （boot/x86_64/exception.c）直接调用 —— 挂在这里两条路都覆盖到。
     * 见 task/itimer.c 顶部对"为什么需要它"的说明。
     */
    itimer_tick();
}

/* ── sched_check_and_yield ─────────────────────────────────── */

/*
 * 由异常返回路径调用，检查是否需要重新调度。
 * 如果需要，执行任务切换。
 * 返回 true 表示发生了切换，false 表示没有。
 */
bool sched_check_and_yield(void)
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
        if (in_irq_context())
            return false;
        c->need_resched = false;
        sched_schedule();
        return true;
    }
    return false;
}

/*
 * trap_preempt_kernel_side - 从【内核态】陷入时的抢占判断
 *
 * 三个架构共用。用户进程的内核侧（syscall / 缺页处理，跑在它自己的内核栈上）
 * 不在这里切换：那会让任意 syscall 代码点被交错执行，产生大面积数据竞争；
 * 它的抢占推迟到 syscall 返回边界。内核线程（含 idle）没有这个问题，可以在
 * 这里被 timer 抢占 —— 否则纯内核态的循环会独占 CPU。
 *
 * preemptible() 内含 irq_depth == 0 判断，嵌套中断里不会误切。
 */
static bool trap_preempt_kernel_side(void)
{
    cpu_t *c = cpu_current();

    if (c->current_task && c->current_task->is_user_process)
        return false;
    if (!c->current_task || !c->need_resched || !preemptible() ||
        c->preempt_schedule_depth != 0)
        return false;

    c->need_resched = false;
    c->preempt_schedule_depth++;
    sched_schedule();
    c->preempt_schedule_depth--;
    return true;
}

/*
 * sched_check_and_yield_from_trap - trap 返回前的调度检查
 *
 * 三个架构的异常/中断返回路径都只调这一个函数，所以这里就是【每个架构唯一的
 * timer 抢占点】。frame_ptr 用来判断这次 trap 来自用户态还是内核态：
 *
 *   AArch64   spsr[3:0] == 0 (EL0t)   → 用户态；!= 0 (EL1h) → 内核态
 *   x86_64    CS.RPL     == 3        → 用户态
 *   RISC-V    sstatus.SPP == 0       → 用户态
 *
 * 来自用户态时直接 sched_check_and_yield()：被中断的是用户代码，异常现场已
 * 完整保存在 trap frame 里，在它自己的内核栈上切换没有任何副作用。
 *
 * 注意：调用本函数时 handle_exception 已经做过 irq_depth--，因此
 * sched_schedule() 开头的 assert_always(!in_irq_context()) 不会触发。
 */
bool sched_check_and_yield_from_trap(void *frame_ptr)
{
#if ARCH_AARCH64
    trap_frame_t *frame = (trap_frame_t *)frame_ptr;
    if (frame && ((frame->spsr & 0xfUL) != 0))
        return trap_preempt_kernel_side();
#elif ARCH_X86_64
    trap_frame_t *frame = (trap_frame_t *)frame_ptr;
    if (frame && ((frame->cs & 3u) != 3u))
        return trap_preempt_kernel_side();
#elif ARCH_RISCV64
    trap_frame_t *frame = (trap_frame_t *)frame_ptr;
    if (frame && (frame->sstatus & SSTATUS_SPP))
        return trap_preempt_kernel_side();
#else
    (void)frame_ptr;
#endif
    return sched_check_and_yield();
}
