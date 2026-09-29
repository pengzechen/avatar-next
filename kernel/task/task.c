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
#include "assert.h"
#include "barrier.h"
#include "klog.h"
#include "string.h"
#include "task/cpu.h"
#include "task/preempt.h"
#include "task/sched.h"
#include "task/switch.h"
#include "timer/timer.h"

static inline uint64_t task_get_ns(void)
{
    return timer_get_ns();
}

#include "mm_vm.h"
#include "user_layout.h"
#include "vm_user.h"
#if ARCH_RISCV64
#include "riscv64/satp_utils.h"
#include "riscv64/sysreg.h"
#endif

/* ── 静态任务池 ──────────────────────────────────────────── */

task_t g_task_pool[TASK_MAX];

/*
 * 任务栈池：16 字节对齐，确保 AArch64/x86_64 的 SP 对齐要求。
 * 每个栈独立分配，互不重叠。
 */
uint8_t g_task_stacks[TASK_MAX][TASK_STACK_SIZE] __attribute__((aligned(16)));
uint8_t g_stack_used[TASK_MAX];

uint32_t g_task_id_cnt = 0;

/* ── 内核页表物理地址（task_init 时读取，RISC-V satp / x86_64 CR3 均使用）── */
uint64_t g_kernel_pgd_phys = 0;

/* ── idle 任务（boot 执行上下文）────────────────────────── */
static task_t g_idle_task;

/* ── idle 专用栈（防止 boot 栈在频繁中断下溢出）────────────── */
static uint8_t g_idle_stack[TASK_STACK_SIZE] __attribute__((aligned(16)));

/* ── 槽表的锁 ────────────────────────────────────────────── *
 *
 * 保护 g_stack_used[]、各 TCB 里"这个槽归谁/还活着吗"那几个字段
 * （state / parent_id / id）、以及 id 分配。
 *
 * 以前这三样全是裸奔的。后果有两类：
 *   1. 两核同时扫到同一个 DEAD 槽 → 都调 task_reap_dead →
 *      vm_destroy_user_process() 跑两遍（它不幂等，第二次照样
 *      pmm_free_pages）→ PMM 双重释放。
 *   2. 两核同时读到 !g_stack_used[i] → 认领同一个槽 →
 *      两个任务共用一个 task_t 和一块栈。
 *
 * 锁内只做"改状态、占位"这种 O(1) 的事；释放用户页表要走几百页，
 * 放到锁外做（先用 state = TASK_ALLOCATING 把槽占住，别人就不会碰）。
 */
static spinlock_t g_task_pool_lock = SPINLOCK_INIT;

/* 任务 id 分配。原来是无保护的 `g_task_id_cnt++`，两核同时 fork 会撞 id。 */
uint32_t task_alloc_id(void)
{
    return __atomic_fetch_add(&g_task_id_cnt, 1U, __ATOMIC_RELAXED);
}

/*
 * slot_is_reapable_orphan_locked - **持锁调用**：这个 DEAD 槽现在能收吗？
 *
 * 光看 state == TASK_DEAD 是不够的 —— 僵尸要留给父进程 wait4 取退出码，
 * 谁都能收就把退出码丢了（实测：父进程 waitpid 自己的直接子进程拿到
 * ECHILD，SMP=1 下都有 8%）。只有"没人会再来 wait 它"的才收：
 *
 *   - parent_id == 0：压根没有父进程（idle / 内核线程）
 *   - 父进程已经不在了：槽被释放，或父进程自己也 DEAD
 *
 * 其余情况等它爹来收。这也顺带修掉 fork 路径那个"回收任意 DEAD 槽"的
 * 循环 —— 它让 A 进程的 fork 把 B 进程还没 wait 的僵尸收走。
 */
static bool slot_is_reapable_orphan_locked(uint32_t idx)
{
    task_t *t = &g_task_pool[idx];
    if (!g_stack_used[idx] || t->state != TASK_DEAD)
        return false;
    if (t->parent_id == 0)
        return true;

    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (!g_stack_used[i] || g_task_pool[i].id != t->parent_id)
            continue;
        /* 找到父进程：它只要还活着（非 DEAD），就轮不到我们收 */
        return g_task_pool[i].state == TASK_DEAD;
    }
    return true; /* 父进程的槽已经没了 */
}

/*
 * slot_take_and_free - 收掉一个已经**被占住**的槽（state == TASK_ALLOCATING）
 *
 * 调用者必须已经在锁内把它标成 TASK_ALLOCATING（占位），
 * 这里在锁外做重活，最后回锁内释放槽位。
 */
static void slot_take_and_free(uint32_t idx)
{
    task_t *task = &g_task_pool[idx];
    uint64_t flags;

    KLOG_DEBUG("[task] Reaping dead task slot %u (id=%u, pgd=0x%llx)\n", idx,
               task->id, (uint64_t)task->pgd);

    /* 锁外：释放用户页表（几百页，不能占着全池的锁） */
    if (task->is_user_process && !task->shares_pgd && task->pgd != NULL) {
        vm_destroy_user_process((uint64_t)task->pgd);
        task->pgd = NULL;
    }

    spin_lock_irqsave(&g_task_pool_lock, &flags);
    g_stack_used[idx] = 0;
    task->stack_base = NULL;
    spin_unlock_irqrestore(&g_task_pool_lock, flags);
}

/* ── 内部：分配/释放任务槽 ──────────────────────────────── */

/*
 * task_alloc_slot - 认领一个空闲任务槽（已清零、栈已刷图案）
 *
 * fork 路径也走这里（以前它内联抄了一份自己的循环，于是漏掉了
 * cpu_affinity / preempt_count 的初始化 —— 那两个字段是复用的 TCB 里
 * 上一个占位者的残值，preempt_count 非 0 会让新任务永久不可抢占）。
 * 清零的代价（sizeof(task_t) + 16KB 栈）相比 fork 要拷的几百页可以忽略。
 */
task_t *task_alloc_slot(void)
{
    /* 先试着回收"没人会来 wait"的僵尸槽 */
    for (;;) {
        uint64_t flags;
        uint32_t victim = TASK_MAX;
        spin_lock_irqsave(&g_task_pool_lock, &flags);
        for (uint32_t i = 0; i < TASK_MAX; i++) {
            if (!slot_is_reapable_orphan_locked(i))
                continue;
            g_task_pool[i].state = TASK_ALLOCATING; /* 占住 */
            victim = i;
            break;
        }
        spin_unlock_irqrestore(&g_task_pool_lock, flags);
        if (victim == TASK_MAX)
            break;
        slot_take_and_free(victim);
    }

    uint64_t flags;
    spin_lock_irqsave(&g_task_pool_lock, &flags);
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (g_stack_used[i])
            continue;
        task_t *task = &g_task_pool[i];
        g_stack_used[i] = 1;
        task->state = TASK_ALLOCATING;
        task->stack_base = g_task_stacks[i];
        spin_unlock_irqrestore(&g_task_pool_lock, flags);

        /* 槽已归本调用者，锁外清零 */
        memset(task, 0, sizeof(*task));
        task->state = TASK_ALLOCATING;
        task->stack_base = g_task_stacks[i];
        /*
         * 把整块栈刷成已知图案：配合 task_stack_used() 量"这个任务实际用掉
         * 多少栈"。arch_init_task_stack() 稍后会在栈顶写好初始帧，其余部分
         * 保持图案 —— 高水位就是"从栈底往上第一处非图案的位置"。
         *
         * 为什么值得留：内核任务栈是**静态数组挨着放的**（g_task_stacks[N][...]），
         * 溢出会直接踩坏邻居任务的栈/TCB，症状是"隔壁任务莫名其妙崩"，
         * 而不是栈溢出的任务自己崩 —— 实测宿主 busybox 就是这么被写坏的
         * （见 vcpu_task_fn 里那段说明）。有数字才能定 size，不然只能靠翻倍赌。
         */
        memset(task->stack_base, TASK_STACK_MAGIC, TASK_STACK_SIZE);
        /* 默认 affinity = ANY：让 sched_enqueue round-robin 分发到所有核。
         * 调用方（如 vcpu_task_create）可在 enqueue 前覆盖。 */
        task->cpu_affinity = CPU_AFFINITY_ANY;
        return task;
    }
    spin_unlock_irqrestore(&g_task_pool_lock, flags);
    return NULL;
}

/*
 * task_reap_dead - 回收一个已退出任务的资源
 *
 * 调用者（wait4 / exec wrapper）已经读过它的退出码，所以这里不做
 * "父进程还在不在"的判断 —— 但**会**防重入：先把 state 从 DEAD 改成
 * ALLOCATING 占住，并发的第二个调用者就进不来了。
 */
void task_reap_dead(task_t *task)
{
    if (!task)
        return;
    uint32_t idx = (uint32_t)(task - g_task_pool);
    if (idx >= TASK_MAX)
        return;

    uint64_t flags;
    spin_lock_irqsave(&g_task_pool_lock, &flags);
    bool mine = (task->state == TASK_DEAD);
    if (mine)
        task->state = TASK_ALLOCATING; /* 占住，防双重释放 */
    spin_unlock_irqrestore(&g_task_pool_lock, flags);

    if (!mine)
        return; /* 别人已经收过了 */

    slot_take_and_free(idx);
}

/*
 * task_stack_used — 该任务栈的高水位（字节，含初始帧）
 *
 * 从栈底往上找第一处非 TASK_STACK_MAGIC 的字节，它到栈顶的距离就是峰值用量。
 * 任务还没跑过时返回 0。**只用于诊断**，不要在热路径里调（O(栈大小)）。
 */
size_t task_stack_used(const task_t *task)
{
    if (!task || !task->stack_base)
        return 0;
    const uint8_t *p = task->stack_base;
    for (size_t i = 0; i < TASK_STACK_SIZE; i++) {
        if (p[i] != TASK_STACK_MAGIC)
            return TASK_STACK_SIZE - i;
    }
    return 0;
}

/* 创建失败时的回滚：把刚认领的槽还回去。同样要走槽表的锁，
 * 否则和并发认领者之间又是一个"两个人都以为槽是空的"窗口。 */
static void free_task_slot(task_t *task)
{
    uint64_t flags;
    spin_lock_irqsave(&g_task_pool_lock, &flags);
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (&g_task_pool[i] != task)
            continue;
        task->state = TASK_ALLOCATING;
        task->stack_base = NULL;
        g_stack_used[i] = 0;
        break;
    }
    spin_unlock_irqrestore(&g_task_pool_lock, flags);
}

/* ── task_trampoline ─────────────────────────────────────── */

/*
 * task_trampoline - 内核线程首次被调度时的落地点
 *
 * 新内核线程的栈帧由 arch_init_task_stack 伪造，x30(LR)=task_trampoline，
 * 首次被调度时 arch_task_switch 的 "ret" 跳到这里。
 *
 * 触发来源：其他任务主动 task_yield()，或 timer 中断返回路径上
 * sched_check_and_yield() 触发重调度。注意切换**不在** ISR 内进行——
 * sched_schedule() 开头断言 !in_irq_context()。
 *
 * 为何必须在此显式开中断：内核线程全程在 EL1 运行，需开中断以可被
 * timer 抢占（否则会独占 CPU、饿死其他任务）。而首次运行**不经过**
 * sched_schedule() 末尾的 arch_irq_restore()——前驱在 arch_irq_save()
 * 里替它关了中断，却没有人替它开。因此这里必须 arch_irq_enable()。
 *
 * 对比：EL0 任务（用户进程）走 task_trampoline_user / arch_fork_resume_user，
 * 那里**不**显式开中断——中断由 eret 跨入 EL0 时按 SPSR 恢复。区别在于
 * 本函数的 entry() 在 EL1 运行，而那两者最终下到 EL0。
 */
void task_trampoline(void)
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
 * 参数通过架构约定的 callee-saved 寄存器传递。
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

void task_init(void)
{
    /* 清空静态池 */
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        g_task_pool[i].state = TASK_DEAD;
        g_stack_used[i] = 0;
    }

    /* 初始化 idle 任务（boot 上下文，使用 idle 专栈） */
    g_idle_task.sp = (uintptr_t)(g_idle_stack + TASK_STACK_SIZE);
    g_idle_task.state = TASK_RUNNING;
    g_idle_task.id = task_alloc_id();
    g_idle_task.priority = 255; /* 最低优先级 */
    g_idle_task.stack_base = g_idle_stack;
    g_idle_task.entry = NULL;
    g_idle_task.arg = NULL;
    g_idle_task.fs_base = 0;
    list_node_init(&g_idle_task.run_node);
    list_node_init(&g_idle_task.wait_node);

    /* 手动 strcpy（避免依赖外部库） */
    g_idle_task.name[0] = 'i';
    g_idle_task.name[1] = 'd';
    g_idle_task.name[2] = 'l';
    g_idle_task.name[3] = 'e';
    g_idle_task.name[4] = '\0';

#if ARCH_RISCV64
    /* 保存内核 SATP 对应的 PGD 物理地址，用于切换回内核任务时恢复 */
    g_kernel_pgd_phys = satp_read_pgd_phys();
#elif ARCH_X86_64
    /* 保存内核 PML4 物理地址，用于切换回内核任务时恢复 */
    g_kernel_pgd_phys = read_cr3() & PTE_ADDR_MASK;
#endif

    /* 初始化调度器，传入 idle 任务 */
    sched_init(&g_idle_task);

    KLOG_TASK("[task] subsystem initialized, idle task id=%u stack_base=%p\n",
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
void task_switch_to_idle_stack(void)
{
    uintptr_t new_sp = (uintptr_t)(g_idle_stack + TASK_STACK_SIZE);
    KLOG_TASK("[task] switching to idle stack at 0x%lx\n",
              (unsigned long)new_sp);

    /* 切到 idle 栈后**永不返回**：直接在 inline asm 里跳到 idle 主循环，
   * 跳过编译器生成的 epilogue（否则会从未初始化的新栈读 LR/RBP 导
   * 致跳到 0）。 */
#if ARCH_X86_64
    __asm__ volatile("mov %0, %%rsp\n\t"      /* 切换到新栈 */
                     "xor %%rbp, %%rbp\n\t"   /* 清零 RBP，标记栈帧链结束 */
                     "jmp task_idle_loop\n\t" /* 跳到 idle 主循环（不返回） */
                     ::"r"(new_sp)
                     : "memory");
#elif ARCH_RISCV64
    __asm__ volatile("mv sp, %0\n\t"
                     "mv fp, zero\n\t"
                     "j  task_idle_loop\n\t" ::"r"(new_sp)
                     : "memory");
#elif ARCH_AARCH64
    __asm__ volatile("mov sp, %0\n\t"
                     "mov x29, xzr\n\t" /* 清零 FP */
                     "b   task_idle_loop\n\t" ::"r"(new_sp)
                     : "memory");
#endif
    __builtin_unreachable();
}

/*
 * task_idle_loop - idle 主循环
 *
 * 由 task_switch_to_idle_stack 在切栈后直接跳入，永不返回。
 * 启用中断后循环执行 task_yield + 体系结构等待指令。
 */
__attribute__((noreturn)) void task_idle_loop(void)
{
    /* idle 是内核线程，全程在 EL1，开中断（既为可被抢占，也因 wfi
   * 需中断唤醒）。同 task_trampoline：首次进入不经过 arch_irq_restore，
   * 故在此显式开。 */
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
void task_set_cpu_affinity(task_t *task, uint32_t cpu_id)
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
task_t *process_create(const char *name, uint64_t user_entry,
                       uint64_t user_code_size, uint64_t user_sp,
                       uint8_t priority)
{
    task_t *task = task_alloc_slot();
    if (!task) {
        KLOG_ERROR("[task] process_create: no free task slots (max=%u)\n",
                   TASK_MAX);
        return NULL;
    }

    task->id = task_alloc_id();
    task->state = TASK_ALLOCATING;
    task->priority = priority;

    /* 标记为用户进程 */
    task->is_user_process = true;
    task->user_started = false;
    task->is_thread = false;
    task->shares_pgd = false;
    task->tgid = task->id;
    task->user_entry = user_entry;
    task->user_sp = user_sp;
    task->user_stack_top = user_sp;
    task->user_stack_size = USER_STACK_SIZE;
    task->fs_base = 0;
    task->ctid_ptr = 0;

    /* 创建独立的用户页表 */
#if ARCH_AARCH64
    task->pgd = (uint64_t *)vm_create_user_process(
        user_entry, user_code_size, user_sp, task->user_stack_size);
    if (task->pgd == NULL) {
        KLOG_ERROR("[task] Failed to create user page table for '%s'\n", name);
        free_task_slot(task);
        return NULL;
    }

    /* 设置用户入口地址为用户虚拟地址（USER_CODE_BASE） */
    task->user_entry = USER_CODE_BASE;

    KLOG_DEBUG("[task] Created page table for '%s': PGD=0x%llx\n", name,
               (uint64_t)task->pgd);
#elif ARCH_RISCV64
    {
        uint64_t pgd_phys = vm_create_user_process(
            user_entry, user_code_size, user_sp, task->user_stack_size);
        if (pgd_phys == 0) {
            KLOG_ERROR("[task] Failed to create user page table for '%s'\n",
                       name);
            free_task_slot(task);
            return NULL;
        }

        /*
     * 将内核高地址别名的 L2 条目（[0x100] 和 [0x102]）移植到用户 PGD。
     * 这样 sret 到用户模式后，发生 trap 时 CPU 仍可通过
     *   stvec = 0xffffffc0xxxxxxxx → L2[0x102]（1GB giga-page，无 PTE_U）
     * 到达内核异常向量，而用户无法访问该区域（PTE_U=0）。
     */
        uint64_t *new_pgd = (uint64_t *)phys_to_virt(pgd_phys);
        uint64_t *kern_pgd = (uint64_t *)phys_to_virt(g_kernel_pgd_phys);
        riscv64_copy_kernel_mappings(new_pgd, kern_pgd);

        task->pgd = (uint64_t *)pgd_phys;
        task->user_entry = USER_CODE_BASE;

        KLOG_DEBUG("[task] Created RISC-V user PGD=0x%llx for '%s'\n", pgd_phys,
                   name);
    }
#elif ARCH_X86_64
    {
        uint64_t pgd_phys = vm_create_user_process(
            user_entry, user_code_size, user_sp, task->user_stack_size);
        if (pgd_phys == 0) {
            KLOG_ERROR("[task] Failed to create user page table for '%s'\n",
                       name);
            free_task_slot(task);
            return NULL;
        }

        /*
     * 将内核高半区 PML4[256..511] 复制到用户页表。
     * 用户态触发 SYSCALL/中断时 CPU 沿内核虚拟地址跳转，
     * 若用户 PML4 中无对应映射则立即触发三重错误。
     */
        uint64_t *kernel_pml4 = (uint64_t *)phys_to_virt(g_kernel_pgd_phys);
        uint64_t *user_pml4 = (uint64_t *)phys_to_virt(pgd_phys);
        x86_copy_kernel_mappings(user_pml4, kernel_pml4);

        task->pgd = (uint64_t *)pgd_phys;
        task->user_entry = USER_CODE_BASE;

        KLOG_DEBUG("[task] Created x86_64 user PGD=0x%llx for '%s'\n", pgd_phys,
                   name);
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
    task->sp =
        arch_init_user_stack(task->stack_base, TASK_STACK_SIZE,
                             task->user_entry, user_sp, (uint64_t)task->pgd);

    /* 初始化完成后才发布为 READY，避免查找/信号路径看到半初始化 TCB。 */
    task->state = TASK_READY;

    /* 加入就绪队列 */
    sched_enqueue(task);

    KLOG_DEBUG("[task] created user process '%s' id=%u prio=%u\n", task->name,
               task->id, (uint32_t)task->priority);
    KLOG_DEBUG(
        "[task]   user_entry=0x%llx (adjusted), user_sp=0x%llx, pgd=0x%llx\n",
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
task_t *process_create_with_pgd(const char *name, uint64_t user_entry,
                                uint64_t user_sp, uint8_t priority,
                                uint64_t pgd_phys, uint64_t heap_end_val,
                                uint64_t mmap_next_val)
{
    task_t *task = task_alloc_slot();
    if (!task) {
        KLOG_ERROR("[task] process_create_with_pgd: no free task slots\n");
        return NULL;
    }

    task->id = task_alloc_id();
    task->state = TASK_ALLOCATING;
    task->priority = priority;
    task->is_user_process = true;
    task->user_started = false;
    task->is_thread = false;
    task->shares_pgd = false;
    task->tgid = task->id;
    task->user_entry = user_entry;
    task->user_sp = user_sp;
    task->user_stack_top = user_sp;
    task->user_stack_size = USER_STACK_SIZE;
    task->pgd = (uint64_t *)pgd_phys;
    task->fs_base = 0;

    /* 初始化进程文件系统相关字段（继承父进程 cwd） */
    task_t *parent = task_current();
    if (parent) {
        uint32_t c = 0;
        while (parent->cwd[c] && c < (uint32_t)(TASK_CWD_LEN - 1)) {
            task->cwd[c] = parent->cwd[c];
            c++;
        }
        task->cwd[c] = '\0';
    } else {
        task->cwd[0] = '/';
        task->cwd[1] = '\0';
    }
    for (uint32_t j = 0; j < TASK_MAX_FD; j++)
        task->fd_table[j] = -1;
    memset(task->fd_cloexec, 0, sizeof(task->fd_cloexec));

    task->parent_id = parent ? parent->id : 0;
    task->uid = parent ? parent->uid : 0;
    task->euid = parent ? parent->euid : 0;
    task->gid = parent ? parent->gid : 0;
    task->egid = parent ? parent->egid : 0;
    task->exit_status = 0;
    task->is_waiting = false;
    task->wait_pid = (uint32_t)-1;
    task->utime_ns = 0;
    task->stime_ns = 0;
    task->sc_entry_ns = 0;
    task->create_ns = task_get_ns();

    /* === 信号字段初始化 === */
    task->pending_sigs = 0;
    task->blocked_sigs = 0;
    task->sig_saved_blocked = 0;
    task->pgid = task->id; /* 默认：自成一组 */
    task->sid = task->id;  /* 默认：自成会话 */
    task->ctty_pty_idx = -1;
    task->sig_frame_sp = 0;
    for (int _si = 0; _si < NSIG; _si++) {
        task->sig_actions[_si].sa_handler = SIG_DFL;
        task->sig_actions[_si].sa_flags = 0;
        task->sig_actions[_si].sa_restorer = 0;
        task->sig_actions[_si].sa_mask = 0;
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
    task->heap_end = heap_end_val;
    task->mmap_next = mmap_next_val;
    /*
     * 初值即基址：exec 传进来的 mmap_next_val 就是本进程 mmap 区的起点
     * （task_execve 里算出来的 mmap_base）。fork 拷贝地址空间时用它当
     * 扫描下界，见 task.h 里 mmap_base 的注释。
     */
    task->mmap_base = mmap_next_val;

    task->sp =
        arch_init_user_stack(task->stack_base, TASK_STACK_SIZE,
                             task->user_entry, user_sp, (uint64_t)task->pgd);

    /*
     * 这里**故意不入队**：调用方（execve）在新任务创建之后还要填 fd 表、
     * pgid/sid/uid、信号掩码等状态，而 TCB 里 fd_table 的初值是 -1
     * —— 那意味着 fd 0/1/2 退回控制台。如果在入队之后才填，SMP 下别的核
     * 可能抢先把这个新任务调度上去，让它带着未初始化的状态跑。
     *
     * 实测症状（`ls /bin | grep bin` 这类管道）：新任务抢先跑起来时
     * fd 0/1/2 还是控制台，于是 ls 把目录列表直接打到串口、grep 去读控制台
     * 并永久阻塞，shell 卡死在 wait 上 —— 表现为"管道没生效 / 挂死"。
     * 上面提前设 heap_end/mmap_next 是同一个道理，只是那次只管了这两个字段。
     * 状态齐了由调用方调 process_start() 入队。
     */
    KLOG_TASK("[task] created user process '%s' id=%u prio=%u (pgd=0x%llx)\n",
              task->name, task->id, (uint32_t)task->priority, pgd_phys);

    return task;
}

/*
 * process_start - 把 process_create_with_pgd 建好的进程挂进运行队列
 *
 * 与 process_create_with_pgd 配对使用：创建时任务停在 TASK_ALLOCATING，
 * 调用方把"必须在新任务跑起来之前就位"的状态全部填完，再调这里放行。
 * 中途失败想放弃这个任务就用 task_reap_dead()/free_task_slot() 收尾。
 */
void process_start(task_t *task)
{
    if (!task)
        return;
    task->state = TASK_READY;
    sched_enqueue(task);
}

/* ── task_create ─────────────────────────────────────────── */

task_t *task_create(const char *name, void (*entry)(void *), void *arg,
                    uint8_t priority)
{
    return task_create_affinity(name, entry, arg, priority, CPU_AFFINITY_ANY);
}

/*
 * task_create_affinity - 创建任务，**入队前**就定好跑在哪颗核上
 *
 * 与 task_create() 的区别只在 `cpu_affinity` 的设置时机，但这正是关键：
 * 先 task_create()（内部已经 sched_enqueue，round-robin 可能挑中别的核）
 * 再 task_set_cpu_affinity() 有窗口 —— 如果新任务在那两步之间已经被目标核
 * 挑走开始执行，sched_dequeue() 就找不到它（RUNNING 的任务不在任何队列里），
 * 紧接着 sched_enqueue() 又把它挂到指定核的队列上，于是同一个任务
 * **既在他核上运行、又躺在指定核的运行队列里** —— 会被两个核同时执行，
 * 任务状态与运行队列双双被写坏。
 *
 * 窗口对两种落点都存在，只是后果不同：落在他核 = 同一任务被两个核同时跑；
 * 落在目标核 = 同一任务"正在跑"又"躺在自己核的队列里"，之后会被重复调度。
 * 实测症状（SMP=2、helper 模式下 Ctrl+] 停 guest 再重启，约 1/2 命中）：
 *   CPU exception #14 at RIP=sched_schedule+0xf2  (next->state = TASK_RUNNING)
 *   CR2 = 0xffffffffffffffc8                      ← pick_next() 拿到野指针
 */
task_t *task_create_affinity(const char *name, void (*entry)(void *), void *arg,
                             uint8_t priority, uint32_t cpu_affinity)
{
    task_t *task = task_alloc_slot();
    if (!task) {
        KLOG_ERROR("[task] task_create: no free task slots (max=%u)\n",
                   TASK_MAX);
        return NULL;
    }

    task->id = task_alloc_id();
    task->state = TASK_ALLOCATING;
    task->priority = priority;
    task->entry = entry;
    task->arg = arg;
    task->is_user_process = false;
    task->user_started = false;
    task->is_thread = false;
    task->shares_pgd = false;
    task->tgid = task->id;
    task->pgd = NULL;
    task->fs_base = 0;
    task->ctid_ptr = 0;
    task->cwd[0] = '/';
    task->cwd[1] = '\0';
    for (uint32_t j = 0; j < TASK_MAX_FD; j++)
        task->fd_table[j] = -1;
    memset(task->fd_cloexec, 0, sizeof(task->fd_cloexec));
    task->parent_id = 0;
    task->uid = 0;
    task->euid = 0;
    task->gid = 0;
    task->egid = 0;
    task->ctty_pty_idx = -1;
    task->exit_status = 0;
    task->is_waiting = false;
    task->wait_pid = (uint32_t)-1;
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

    /* 初始化完成后才发布为 READY，避免查找路径看到半初始化 TCB。 */
    task->state = TASK_READY;

    /* 入队之前定好核（见本函数上方注释：入队后再改有窗口） */
    task->cpu_affinity = cpu_affinity;

    /* 加入就绪队列，等待调度 */
    sched_enqueue(task);

    KLOG_TASK("[task] created '%s' id=%u prio=%u sp=0x%lx\n", task->name,
              task->id, (uint32_t)task->priority, (unsigned long)task->sp);
    return task;
}

/* ── task_yield ──────────────────────────────────────────── */

void task_yield(void)
{
    sched_schedule();
}

/* ── task_exit ───────────────────────────────────────────── */

void task_exit(void)
{
    task_t *cur = task_current();
    KLOG_TASK("[task] '%s' (id=%u) exiting\n", cur->name, cur->id);

    /*
     * 置 DEAD 要在槽表的锁里做：否则会有一个窗口 —— 别的核正好在
     * task_alloc_slot 的孤儿回收扫描里读这个槽，看到"父进程还活着"，
     * 于是不收；而本任务马上就要变成 DEAD 的父进程。反过来也一样。
     * 拿锁之后，"谁是死是活"和"能不能收"就是同一个瞬间的判定了。
     */
    {
        uint64_t _pf;
        spin_lock_irqsave(&g_task_pool_lock, &_pf);
        cur->state = TASK_DEAD;
        spin_unlock_irqrestore(&g_task_pool_lock, _pf);
    }

    /* Notify waiting parent */
    extern void notify_parent_wait_from_task(task_t * child);
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
   * 当后续创建新任务时，task_alloc_slot() 会回收"没人会再来 wait"的
   * 僵尸槽（判据见 slot_is_reapable_orphan_locked）。父进程自己那一份
   * 由 wait4 / exec wrapper 读完成码后调 task_reap_dead() 收。
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

task_t *task_current(void)
{
    /*
   * Phase 3：从 per-CPU 控制块读取，AP 上才能拿到正确的任务。
   * cpu_current() 内部已有 NULL 回退到 g_cpus[0]，因此在 cpu_init_bsp
   * 之前的极早期路径也是安全的（会返回 g_cpus[0].current_task = NULL）。
   */
    return cpu_current()->current_task;
}

/* ── task_block ──────────────────────────────────────────── */

void task_block(list_t *wait_queue)
{
    task_t *cur = task_current();
    assert_always(!in_irq_context());

    /* 参数验证：wait_queue 必须是内核地址或 NULL */
    if (wait_queue && (uintptr_t)wait_queue < 0xffffffc000000000) {
        KLOG_ERROR("[task_block] FATAL: wait_queue=%p is user address!\n",
                   wait_queue);
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

void task_unblock(task_t *task)
{
    if (task->state != TASK_BLOCKED)
        return;

    task->state = TASK_READY;
    sched_enqueue(task);
}

/* ── 前台进程组（全局）────────────────────────────────────── */
volatile uint32_t g_fg_pgid = 0;

/* ── task_find_by_id ─────────────────────────────────────── */
task_t *task_find_by_id(uint32_t id)
{
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (g_stack_used[i] && g_task_pool[i].id == id &&
            g_task_pool[i].state != TASK_DEAD &&
            g_task_pool[i].state != TASK_ALLOCATING)
            return &g_task_pool[i];
    }
    return NULL;
}

/* ── task_send_signal ────────────────────────────────────── */
void task_send_signal(task_t *t, int sig)
{
    if (!t || sig < 1 || sig > NSIG)
        return;
    t->pending_sigs |= (1ULL << (sig - 1));
}

/* ── task_send_signal_to_pgid ────────────────────────────── */
int task_send_signal_to_pgid(uint32_t pgid, int sig)
{
    if (!pgid)
        return 0;
    int count = 0;
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (g_stack_used[i] && g_task_pool[i].pgid == pgid &&
            g_task_pool[i].state != TASK_DEAD &&
            g_task_pool[i].state != TASK_ALLOCATING &&
            g_task_pool[i].is_user_process) {
            task_send_signal(&g_task_pool[i], sig);
            count++;
        }
    }
    return count;
}
