/*
 * kernel/task/task.c — 内核任务生命周期管理
 *
 * 提供：
 *   task_init()    — 初始化任务子系统（boot 上下文 → idle 任务）
 *   task_create()  — 从静态池分配任务并加入调度队列
 *   task_yield()   — 主动让出 CPU
 *   task_exit()    — 终止当前任务
 *   task_current() — 返回当前运行任务
 *   task_trampoline() — 新任务的启动入口（汇编通过 ret 跳转到此）
 */

#include "task/task.h"
#include "task/sched.h"
#include "task/switch.h"
#include "task/cpu.h"
#include "klog.h"
#include "barrier.h"
#include "assert.h"
#include "task/preempt.h"
#include "timer/timer.h"

static inline uint64_t task_get_ns(void)
{
    return timer_get_ns();
}

#include "mm_vm.h"
#include "vm_user.h"
#include "user_layout.h"
#if ARCH_RISCV64
#include "riscv64/sysreg.h"
#include "riscv64/satp_utils.h"
#endif

/* ── 静态任务池 ──────────────────────────────────────────── */

task_t   g_task_pool[TASK_MAX];

/*
 * 任务栈池：16 字节对齐，确保 AArch64/x86_64 的 SP 对齐要求。
 * 每个栈独立分配，互不重叠。
 */
uint8_t  g_task_stacks[TASK_MAX][TASK_STACK_SIZE] __attribute__((aligned(16)));
uint8_t  g_stack_used[TASK_MAX];

uint32_t g_task_id_cnt = 0;

/* ── 内核页表物理地址（task_init 时读取，RISC-V satp / x86_64 CR3 均使用）── */
uint64_t g_kernel_pgd_phys = 0;

/* ── idle 任务（boot 执行上下文）────────────────────────── */
static task_t g_idle_task;

/* ── idle 专用栈（防止 boot 栈在频繁中断下溢出）────────────── */
static uint8_t g_idle_stack[TASK_STACK_SIZE] __attribute__((aligned(16)));

/* ── 当前任务指针（在 task.h 中 extern 声明）────────────── */
task_t *g_current_task = NULL;

/* ── 内部：分配/释放任务槽 ──────────────────────────────── */

/* 前向声明 */
static void cleanup_dead_task_slot(void);

static task_t *
alloc_task_slot(void)
{
    /* 先尝试清理已死亡的任务槽（延迟清理策略） */
    cleanup_dead_task_slot();

    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (!g_stack_used[i]) {
            task_t *task = &g_task_pool[i];
            memset(task, 0, sizeof(*task));
            g_stack_used[i] = 1;
            task->stack_base = g_task_stacks[i];
            /* 默认 affinity = ANY：让 sched_enqueue round-robin 分发到所有核。
             * 调用方（如 vcpu_task_create）可在 enqueue 前覆盖。 */
            task->cpu_affinity = CPU_AFFINITY_ANY;
            return task;
        }
    }
    return NULL;
}

/* ── 内部：清理 DEAD 任务槽 ──────────────────────────────────── */
static void
cleanup_dead_task_slot(void)
{
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (g_stack_used[i] && g_task_pool[i].state == TASK_DEAD) {
            task_reap_dead(&g_task_pool[i]);
            return;
        }
    }
}

void
task_reap_dead(task_t *task)
{
    if (!task || task->state != TASK_DEAD)
        return;

    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (&g_task_pool[i] != task)
            continue;

        KLOG_DEBUG("[task] Reaping dead task slot %u (id=%u, pgd=0x%llx)\n",
                   i, task->id, (uint64_t)task->pgd);

        if (task->is_user_process && !task->is_thread && task->pgd != NULL) {
            vm_destroy_user_process((uint64_t)task->pgd);
            task->pgd = NULL;
        }

        g_stack_used[i] = 0;
        task->stack_base = NULL;
        return;
    }
}

static void
free_task_slot(task_t *task)
{
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (&g_task_pool[i] == task) {
            g_stack_used[i] = 0;
            return;
        }
    }
}

/* ── task_trampoline ─────────────────────────────────────── */

/*
 * 新任务首次被调度时，arch_task_switch 的 "ret" 跳转到这里。
 *
 * 此时我们处于 sched_schedule() 调用链的上下文中
 * （要么是从 timer ISR 调度过来，要么是其他任务 yield 触发）。
 * 中断因 arch_irq_save() 而被关闭，需要在启动任务前重新开启。
 */
void
task_trampoline(void)
{
    arch_irq_enable();

    task_t *cur = task_current();
    KLOG_DEBUG("[task] '%s' (id=%u) starting\n", cur->name, cur->id);

    cur->entry(cur->arg);

    /* entry 函数返回时自动退出 */
    task_exit();
}

/* ── task_trampoline_user ───────────────────────────────────── */

/*
 * 用户进程首次被调度时，跳转到这里。
 *
 * 注意：这是汇编函数 task_trampoline_user 的 C 存根，
 * 实际实现 switch.S 中。
 * 参数通过寄存器传递（x19=entry, x20=sp）。
 */
void task_trampoline_user(void);

#if ARCH_RISCV64
void arch_user_entry_debug(uint64_t user_entry, uint64_t user_sp,
                           uint64_t kernel_sp, uint64_t user_pgd)
{
    (void)user_entry;
    (void)user_sp;
    (void)kernel_sp;
    (void)user_pgd;

    task_t *cur = task_current();
    if (cur && cur->is_user_process)
        cur->user_started = true;
}
#endif
/* ── task_init ───────────────────────────────────────────── */

void
task_init(void)
{
    /* 清空静态池 */
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        g_task_pool[i].state = TASK_DEAD;
        g_stack_used[i]      = 0;
    }

    /* 初始化 idle 任务（boot 上下文，使用 idle 专栈） */
    g_idle_task.sp         = (uintptr_t)(g_idle_stack + TASK_STACK_SIZE);
    g_idle_task.state      = TASK_RUNNING;
    g_idle_task.id         = g_task_id_cnt++;
    g_idle_task.priority   = 255; /* 最低优先级 */
    g_idle_task.stack_base = g_idle_stack;
    g_idle_task.entry      = NULL;
    g_idle_task.arg        = NULL;
    g_idle_task.fs_base    = 0;
    list_node_init(&g_idle_task.run_node);
    list_node_init(&g_idle_task.wait_node);

    /* 手动 strcpy（避免依赖外部库） */
    g_idle_task.name[0] = 'i';
    g_idle_task.name[1] = 'd';
    g_idle_task.name[2] = 'l';
    g_idle_task.name[3] = 'e';
    g_idle_task.name[4] = '\0';

    g_current_task = &g_idle_task;

#if ARCH_RISCV64
    /* 保存内核 SATP 对应的 PGD 物理地址，用于切换回内核任务时恢复 */
    g_kernel_pgd_phys = satp_read_pgd_phys();
#elif ARCH_X86_64
    /* 保存内核 PML4 物理地址，用于切换回内核任务时恢复 */
    g_kernel_pgd_phys = read_cr3() & PTE_ADDR_MASK;
#endif

    /* 初始化调度器，传入 idle 任务 */
    sched_init(&g_idle_task);

    KLOG_INFO("[task] subsystem initialized, idle task id=%u stack_base=%p\n",
              g_idle_task.id, (void *)g_idle_task.stack_base);
}

/*
 * task_switch_to_idle_stack - 切换到 idle 专用栈
 *
 * 必须在 task_init() 返回后、进入 idle 循环前调用。
 * 在 task_init() 内部切换会导致返回地址丢失。
 *
 * x86_64 特殊处理：需要保存返回地址和帧指针，然后在新栈上恢复，
 * 否则函数返回时会出现 General Protection Fault。
 */
void
task_switch_to_idle_stack(void)
{
    uintptr_t new_sp = (uintptr_t)(g_idle_stack + TASK_STACK_SIZE);
    KLOG_INFO("[task] switching to idle stack at 0x%lx\n", (unsigned long)new_sp);

    /* 切到 idle 栈后**永不返回**：直接在 inline asm 里跳到 idle 主循环，
     * 跳过编译器生成的 epilogue（否则会从未初始化的新栈读 LR/RBP 导
     * 致跳到 0）。 */
#if ARCH_X86_64
    __asm__ volatile(
        "mov %0, %%rsp\n\t"         /* 切换到新栈 */
        "xor %%rbp, %%rbp\n\t"      /* 清零 RBP，标记栈帧链结束 */
        "jmp task_idle_loop\n\t"    /* 跳到 idle 主循环（不返回） */
        :: "r"(new_sp) : "memory"
    );
#elif ARCH_RISCV64
    __asm__ volatile(
        "mv sp, %0\n\t"
        "mv fp, zero\n\t"
        "j  task_idle_loop\n\t"
        :: "r"(new_sp) : "memory"
    );
#elif ARCH_AARCH64
    __asm__ volatile(
        "mov sp, %0\n\t"
        "mov x29, xzr\n\t"          /* 清零 FP */
        "b   task_idle_loop\n\t"
        :: "r"(new_sp) : "memory"
    );
#endif
    __builtin_unreachable();
}

/*
 * task_idle_loop - idle 主循环
 *
 * 由 task_switch_to_idle_stack 在切栈后直接跳入，永不返回。
 * 启用中断后循环执行 task_yield + 体系结构等待指令。
 */
__attribute__((noreturn)) void
task_idle_loop(void)
{
    arch_irq_enable();
    for (;;) {
        task_yield();
#if ARCH_AARCH64
        __asm__ volatile("wfi");
#elif ARCH_X86_64
        __asm__ volatile("hlt");
#elif ARCH_RISCV64
        __asm__ volatile("wfi");
#endif
    }
}

/* ── task_set_cpu_affinity ───────────────────────────────────
 * 把 task 移到指定 cpu 的运行队列。供 vcpu/EL2 等不能跨核迁移的
 * 任务在 enqueue 后立即重新绑定。
 *
 * 假设：task 当前已经被 sched_enqueue 过（在某个核的 rq 中）。
 * 若尚未入队（READY 之前），可直接设置 cpu_affinity 字段后再 enqueue。
 */
void
task_set_cpu_affinity(task_t *task, uint32_t cpu_id)
{
    if (!task) {
        return;
    }
    sched_dequeue(task);
    task->cpu_affinity = cpu_id;
    sched_enqueue(task);
}

/* ── process_create ─────────────────────────────────────────── */

/*
 * process_create - 创建用户进程
 * @name:       进程名称
 * @user_entry: 用户态入口点（虚拟地址）
 * @user_sp:    用户栈指针（虚拟地址）
 * @priority:   优先级
 *
 * 创建独立地址空间的用户进程。
 */
task_t *
process_create(const char *name, uint64_t user_entry, uint64_t user_code_size,
               uint64_t user_sp, uint8_t priority)
{
    task_t *task = alloc_task_slot();
    if (!task) {
        KLOG_ERROR("[task] process_create: no free task slots (max=%u)\n", TASK_MAX);
        return NULL;
    }

    task->id       = g_task_id_cnt++;
    task->state    = TASK_READY;
    task->priority = priority;

    /* 标记为用户进程 */
    task->is_user_process = true;
    task->user_started    = false;
    task->is_thread       = false;
    task->user_entry      = user_entry;
    task->user_sp         = user_sp;
    task->user_stack_top  = user_sp;
    task->user_stack_size = USER_STACK_SIZE;
    task->fs_base         = 0;
    task->ctid_ptr        = 0;

    /* 创建独立的用户页表 */
#if ARCH_AARCH64
    task->pgd = (uint64_t *)vm_create_user_process(user_entry, user_code_size,
                                                      user_sp, task->user_stack_size);
    if (task->pgd == NULL) {
        KLOG_ERROR("[task] Failed to create user page table for '%s'\n", name);
        free_task_slot(task);
        return NULL;
    }

    /* 设置用户入口地址为用户虚拟地址（USER_CODE_BASE） */
    task->user_entry = USER_CODE_BASE;

    KLOG_DEBUG("[task] Created page table for '%s': PGD=0x%llx\n",
               name, (uint64_t)task->pgd);
#elif ARCH_RISCV64
    {
        uint64_t pgd_phys = vm_create_user_process(user_entry, user_code_size,
                                                   user_sp, task->user_stack_size);
        if (pgd_phys == 0) {
            KLOG_ERROR("[task] Failed to create user page table for '%s'\n", name);
            free_task_slot(task);
            return NULL;
        }

        /*
         * 将内核高地址别名的 L2 条目（[0x100] 和 [0x102]）移植到用户 PGD。
         * 这样 sret 到用户模式后，发生 trap 时 CPU 仍可通过
         *   stvec = 0xffffffc0xxxxxxxx → L2[0x102]（1GB giga-page，无 PTE_U）
         * 到达内核异常向量，而用户无法访问该区域（PTE_U=0）。
         */
        uint64_t *new_pgd  = (uint64_t *)phys_to_virt(pgd_phys);
        uint64_t *kern_pgd = (uint64_t *)phys_to_virt(g_kernel_pgd_phys);
        riscv64_copy_kernel_mappings(new_pgd, kern_pgd);

        task->pgd        = (uint64_t *)pgd_phys;
        task->user_entry = USER_CODE_BASE;

        KLOG_DEBUG("[task] Created RISC-V user PGD=0x%llx for '%s'\n",
               pgd_phys, name);
    }
#elif ARCH_X86_64
    {
        uint64_t pgd_phys = vm_create_user_process(user_entry, user_code_size,
                                                   user_sp, task->user_stack_size);
        if (pgd_phys == 0) {
            KLOG_ERROR("[task] Failed to create user page table for '%s'\n", name);
            free_task_slot(task);
            return NULL;
        }

        /*
         * 将内核高半区 PML4[256..511] 复制到用户页表。
         * 用户态触发 SYSCALL/中断时 CPU 沿内核虚拟地址跳转，
         * 若用户 PML4 中无对应映射则立即触发三重错误。
         */
        uint64_t *kernel_pml4 = (uint64_t *)phys_to_virt(g_kernel_pgd_phys);
        uint64_t *user_pml4   = (uint64_t *)phys_to_virt(pgd_phys);
        x86_copy_kernel_mappings(user_pml4, kernel_pml4);

        task->pgd        = (uint64_t *)pgd_phys;
        task->user_entry = USER_CODE_BASE;

        KLOG_DEBUG("[task] Created x86_64 user PGD=0x%llx for '%s'\n",
               pgd_phys, name);
    }
#else
    /* 其他架构暂时使用共享内核页表 */
    task->pgd = NULL;
    task->user_entry = user_entry;
    KLOG_DEBUG("[task] Using shared kernel page table for '%s'\n", name);
#endif

    list_node_init(&task->run_node);
    list_node_init(&task->wait_node);

    /* 复制任务名称 */
    uint32_t i = 0;
    while (name[i] && i < (uint32_t)(TASK_NAME_LEN - 1)) {
        task->name[i] = name[i];
        i++;
    }
    task->name[i] = '\0';

    /* 使用用户进程专用的栈初始化函数（注意：使用调整后的虚拟地址） */
    task->sp = arch_init_user_stack(task->stack_base, TASK_STACK_SIZE,
                                    task->user_entry, user_sp,
                                    (uint64_t)task->pgd);

    // /* EL0 用户进程暂时固定 BSP：跨核运行需要每核独立 TTBR0 切换，
    //  * 等 vm_user 支持多核后再改为 CPU_AFFINITY_ANY。 */
    // task->cpu_affinity = 0;

    /* 加入就绪队列 */
    sched_enqueue(task);

    KLOG_DEBUG("[task] created user process '%s' id=%u prio=%u\n",
               task->name, task->id, (uint32_t)task->priority);
    KLOG_DEBUG("[task]   user_entry=0x%llx (adjusted), user_sp=0x%llx, pgd=0x%llx\n",
               task->user_entry, user_sp, (uint64_t)task->pgd);

    return task;
}

/* ── process_create_with_pgd ────────────────────────────── */

/*
 * process_create_with_pgd - 使用调用方已构建好的用户页表创建进程
 *
 * 跳过 vm_create_user_process，直接使用 pgd_phys。
 * 适用于 ELF 加载器：加载器已自行完成段映射和栈映射，
 * 不需要再做一次拷贝。user_entry 直接作为用户虚拟入口，不做转换。
 */
task_t *
process_create_with_pgd(const char *name, uint64_t user_entry, uint64_t user_sp,
                         uint8_t priority, uint64_t pgd_phys,
                         uint64_t heap_end_val, uint64_t mmap_next_val)
{
    task_t *task = alloc_task_slot();
    if (!task) {
        KLOG_ERROR("[task] process_create_with_pgd: no free task slots\n");
        return NULL;
    }

    task->id              = g_task_id_cnt++;
    task->state           = TASK_READY;
    task->priority        = priority;
    task->is_user_process = true;
    task->user_started    = false;
    task->user_entry      = user_entry;
    task->user_sp         = user_sp;
    task->user_stack_top  = user_sp;
    task->user_stack_size = USER_STACK_SIZE;
    task->pgd             = (uint64_t *)pgd_phys;
    task->fs_base         = 0;

    /* 初始化进程文件系统相关字段（继承父进程 cwd） */
    if (g_current_task) {
        uint32_t c = 0;
        while (g_current_task->cwd[c] && c < (uint32_t)(TASK_CWD_LEN - 1)) {
            task->cwd[c] = g_current_task->cwd[c];
            c++;
        }
        task->cwd[c] = '\0';
    } else {
        task->cwd[0] = '/';
        task->cwd[1] = '\0';
    }
    for (uint32_t j = 0; j < TASK_MAX_FD; j++)
        task->fd_table[j] = -1;

    task->parent_id  = g_current_task ? g_current_task->id : 0;
    task->exit_status = 0;
    task->is_waiting  = false;
    task->wait_pid    = (uint32_t)-1;
    task->utime_ns    = 0;
    task->stime_ns    = 0;
    task->sc_entry_ns = 0;
    task->create_ns   = task_get_ns();

    /* === 信号字段初始化 === */
    task->pending_sigs      = 0;
    task->blocked_sigs      = 0;
    task->sig_saved_blocked = 0;
    task->pgid              = task->id;   /* 默认：自成一组 */
    task->sig_frame_sp      = 0;
    for (int _si = 0; _si < NSIG; _si++) {
        task->sig_actions[_si].sa_handler  = SIG_DFL;
        task->sig_actions[_si].sa_flags    = 0;
        task->sig_actions[_si].sa_restorer = 0;
        task->sig_actions[_si].sa_mask     = 0;
    }

    list_node_init(&task->run_node);
    list_node_init(&task->wait_node);

    uint32_t i = 0;
    while (name[i] && i < (uint32_t)(TASK_NAME_LEN - 1)) {
        task->name[i] = name[i];
        i++;
    }
    task->name[i] = '\0';

    /* 在入队之前设置堆和 mmap 地址，避免调度器过早切换到该任务时看到 0 */
    task->heap_end  = heap_end_val;
    task->mmap_next = mmap_next_val;

    task->sp = arch_init_user_stack(task->stack_base, TASK_STACK_SIZE,
                                    task->user_entry, user_sp,
                                    (uint64_t)task->pgd);

    sched_enqueue(task);

    KLOG_DEBUG("[task] created user process '%s' id=%u prio=%u (pgd=0x%llx)\n",
               task->name, task->id, (uint32_t)task->priority, pgd_phys);

    return task;
}

/* ── task_create ─────────────────────────────────────────── */

task_t *
task_create(const char *name, void (*entry)(void *), void *arg, uint8_t priority)
{
    task_t *task = alloc_task_slot();
    if (!task) {
        KLOG_ERROR("[task] task_create: no free task slots (max=%u)\n", TASK_MAX);
        return NULL;
    }

    task->id       = g_task_id_cnt++;
    task->state    = TASK_READY;
    task->priority = priority;
    task->entry    = entry;
    task->arg      = arg;
    task->is_user_process = false;
    task->user_started    = false;
    task->is_thread       = false;
    task->pgd      = NULL;
    task->fs_base  = 0;
    task->ctid_ptr = 0;
    task->cwd[0]   = '/';
    task->cwd[1]   = '\0';
    for (uint32_t j = 0; j < TASK_MAX_FD; j++)
        task->fd_table[j] = -1;
    task->parent_id   = 0;
    task->exit_status = 0;
    task->is_waiting  = false;
    task->wait_pid    = (uint32_t)-1;
    list_node_init(&task->run_node);
    list_node_init(&task->wait_node);

    /* 复制任务名称（最多 TASK_NAME_LEN-1 字节） */
    uint32_t i = 0;
    while (name[i] && i < (uint32_t)(TASK_NAME_LEN - 1)) {
        task->name[i] = name[i];
        i++;
    }
    task->name[i] = '\0';

    /* 在任务栈上构造初始切换帧，使首次调度跳到 task_trampoline */
    task->sp = arch_init_task_stack(task->stack_base, TASK_STACK_SIZE);

    /* 加入就绪队列，等待调度 */
    sched_enqueue(task);

    KLOG_DEBUG("[task] created '%s' id=%u prio=%u sp=0x%lx\n",
               task->name, task->id, (uint32_t)task->priority, (unsigned long)task->sp);
    return task;
}

/* ── task_yield ──────────────────────────────────────────── */

void
task_yield(void)
{
    sched_schedule();
}

/* ── task_exit ───────────────────────────────────────────── */

void
task_exit(void)
{
    task_t *cur = task_current();
    KLOG_DEBUG("[task] '%s' (id=%u) exiting\n", cur->name, cur->id);

    cur->state = TASK_DEAD;

    /* Notify waiting parent */
    extern void notify_parent_wait_from_task(task_t *child);
    notify_parent_wait_from_task(cur);

    /*
     * 从就绪队列移除（理论上已不在队列，因为 RUNNING 时会重入队，
     * 但 sched_dequeue 做了安全检查）。
     */
    sched_dequeue(cur);

    /*
     * 注意：不在这里调用 free_task_slot()！
     *
     * 如果在这里释放栈槽（g_stack_used[i] = 0），那么在接下来的
     * sched_schedule() 执行期间，如果发生中断，新的任务可能会被分配
     * 到同一个栈槽，导致两个任务使用同一个栈，造成数据损坏。
     *
     * 正确的做法是：标记为 DEAD 后，让栈槽保持"已占用"状态。
     * 当后续创建新任务时，alloc_task_slot() 会调用
     * cleanup_dead_task_slot() 来清理已死亡的任务槽。
     *
     * 这样可以确保在 task_exit() 执行期间和调度切换期间，
     * 没有其他任务会重用这个栈。
     */

    /*
     * 切换到下一个任务。sched_schedule 检测到 state == DEAD，
     * 不会重新入队，所以不会再次调度到本任务。
     */
    sched_schedule();

    /* 不应到达这里 */
    while (1)
        ;
}

/* ── task_current ────────────────────────────────────────── */

task_t *
task_current(void)
{
    /*
     * Phase 3：从 per-CPU 控制块读取，AP 上才能拿到正确的任务。
     * cpu_current() 内部已有 NULL 回退到 g_cpus[0]，因此在 cpu_init_bsp
     * 之前的极早期路径也是安全的（会返回 g_cpus[0].current_task = NULL）。
     */
    return cpu_current()->current_task;
}

/* ── task_block ──────────────────────────────────────────── */

void
task_block(list_t *wait_queue)
{
    task_t *cur = task_current();
    assert_always(!in_irq_context());

    /* 参数验证：wait_queue 必须是内核地址或 NULL */
    if (wait_queue && (uintptr_t)wait_queue < 0xffffffc000000000) {
        KLOG_ERROR("[task_block] FATAL: wait_queue=%p is user address!\n", wait_queue);
        KLOG_ERROR("[task_block]   task='%s' pid=%u\n", cur->name, cur->id);
        platform_panic();
    }

    /* 暂停 syscall stime 计时（阻塞期间不计入 stime） */
    if (cur->is_user_process && cur->sc_entry_ns != 0) {
        cur->stime_ns += task_get_ns() - cur->sc_entry_ns;
        cur->sc_entry_ns = 0;
    }

    /* 设置阻塞状态 */
    cur->state = TASK_BLOCKED;

    /* 如果提供了等待队列，将任务加入 */
    if (wait_queue) {
        list_insert_last(wait_queue, &cur->wait_node);
    }

    /* 触发调度，切换到其他任务 */
    sched_schedule();

    /* 恢复 stime 计时（从被唤醒时刻起重新开始） */
    if (cur->is_user_process)
        cur->sc_entry_ns = task_get_ns();
}

/* ── task_unblock ────────────────────────────────────────── */

void
task_unblock(task_t *task)
{
    /* 将任务从阻塞状态改为就绪 */
    task->state = TASK_READY;

    /* 加入就绪队列 */
    sched_enqueue(task);
}

/* ── 前台进程组（全局）────────────────────────────────────── */
volatile uint32_t g_fg_pgid = 0;

/* ── task_find_by_id ─────────────────────────────────────── */
task_t *
task_find_by_id(uint32_t id)
{
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (g_stack_used[i] &&
            g_task_pool[i].id    == id &&
            g_task_pool[i].state != TASK_DEAD)
            return &g_task_pool[i];
    }
    return NULL;
}

/* ── task_send_signal ────────────────────────────────────── */
void
task_send_signal(task_t *t, int sig)
{
    if (!t || sig < 1 || sig > NSIG) return;
    t->pending_sigs |= (1ULL << (sig - 1));
}

/* ── task_send_signal_to_pgid ────────────────────────────── */
void
task_send_signal_to_pgid(uint32_t pgid, int sig)
{
    if (!pgid) return;
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (g_stack_used[i] &&
            g_task_pool[i].pgid  == pgid &&
            g_task_pool[i].state != TASK_DEAD &&
            g_task_pool[i].is_user_process)
            task_send_signal(&g_task_pool[i], sig);
    }
}
