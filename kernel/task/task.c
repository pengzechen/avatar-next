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
#include "klog.h"
#include "barrier.h"

/* 内联获取系统时间（ns），用于 CPU 时间计账 */
extern volatile uint64_t g_system_ticks;
extern uintptr_t         g_timer_cfg_counter_hz;
extern unsigned          g_timer_cfg_tick_ms;
#if ARCH_X86_64
extern volatile uint64_t g_tsc_freq_hz;
#endif
static inline uint64_t task_get_ns(void)
{
#if ARCH_RISCV64
    uint64_t ticks;
    __asm__ volatile("rdtime %0" : "=r"(ticks));
    uintptr_t freq = g_timer_cfg_counter_hz;
    if (!freq) freq = 10000000UL;
    return (ticks / freq) * 1000000000ULL + (ticks % freq) * 1000000000ULL / freq;
#elif ARCH_AARCH64
    uint64_t ticks;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(ticks));
    uintptr_t freq = g_timer_cfg_counter_hz;
    if (!freq) freq = 62500000UL;
    return (ticks / freq) * 1000000000ULL + (ticks % freq) * 1000000000ULL / freq;
#else
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t ticks = ((uint64_t)hi << 32) | (uint64_t)lo;
    uint64_t freq  = g_tsc_freq_hz;
    if (!freq) {
        uint64_t tick_ms = g_timer_cfg_tick_ms ? g_timer_cfg_tick_ms : 10ULL;
        return g_system_ticks * tick_ms * 1000000ULL;
    }
    return (ticks / freq) * 1000000000ULL + (ticks % freq) * 1000000000ULL / freq;
#endif
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
            g_stack_used[i]           = 1;
            g_task_pool[i].stack_base = g_task_stacks[i];
            KLOG_INFO("[task] alloc_task_slot: slot=%u base=0x%lx\n",
                      i, (unsigned long)g_task_stacks[i]);
            return &g_task_pool[i];
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
            KLOG_DEBUG("[task] Cleaning up dead task slot %u (id=%u)\n",
                      i, g_task_pool[i].id);
            g_stack_used[i] = 0;
            g_task_pool[i].stack_base = NULL;
            return;
        }
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
    extern pmm_t *g_pmm;
    task_t *cur = task_current();
    if (cur && cur->is_user_process)
        cur->user_started = true;

    KLOG_INFO("[user-entry] trampoline: entry=0x%llx usp=0x%llx ksp=0x%llx upgd=0x%llx satp=0x%llx sstatus=0x%llx sie=0x%llx stvec=0x%llx\n",
              user_entry,
              user_sp,
              kernel_sp,
              user_pgd,
              CSR_READ(satp),
              READ_SSTATUS(),
              READ_SIE(),
              READ_STVEC());
    KLOG_INFO("[user-entry] g_pmm = %p (checking before user mode entry)\n", g_pmm);
    
    /* 调试：检查用户页表的内核映射 */
    if (user_pgd != 0) {
        uint64_t *user_l1 = (uint64_t *)phys_to_virt(user_pgd);
        KLOG_INFO("[user-entry] User PGD L1[0x100]=0x%llx L1[0x102]=0x%llx\n",
                  user_l1[0x100], user_l1[0x102]);
        KLOG_INFO("[user-entry] User PGD L1[0]=0x%llx (for VA 0x10000)\n",
                  user_l1[0]);
        if (user_l1[0] != 0 && (user_l1[0] & 1) != 0) {
            uint64_t l0_pa = ((user_l1[0] >> 10) & 0xfffffffffffULL) << 12;
            uint64_t *l0 = (uint64_t *)phys_to_virt(l0_pa);
            KLOG_INFO("[user-entry] L1[0] -> L0 @ PA=0x%llx, L0[0]=0x%llx\n",
                      l0_pa, l0[0]);
            if (l0[0] != 0 && (l0[0] & 1) != 0) {
                uint64_t l_1_pa = ((l0[0] >> 10) & 0xfffffffffffULL) << 12;
                uint64_t *l_1 = (uint64_t *)phys_to_virt(l_1_pa);
                KLOG_INFO("[user-entry] L0[0] -> Leaf-L @ PA=0x%llx, L[16]=0x%llx (VA 0x10000 PTE)\n",
                          l_1_pa, l_1[16]);
            }
        }
    }
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

    /*
     * 将当前 boot 执行上下文注册为 idle 任务。
     * 为防止频繁中断导致 boot 栈溢出，给 idle 分配专用栈。
     * 
     * 重要：idle.sp 必须指向实际的栈顶，因为：
     *   1. 任务退出后切换回 idle 时，arch_task_switch 会加载 idle.sp
     *   2. 如果 sp=0 或无效值，下次中断会在无效地址保存寄存器，触发 page fault
     */
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
#if ARCH_X86_64
    /* x86_64: 保存返回地址和 RBP，切换栈，在新栈上恢复它们 */
    __asm__ volatile(
        "pop    %%rax\n\t"          /* 弹出返回地址到 rax */
        "mov    %%rbp, %%rcx\n\t"   /* 保存旧的 rbp 到 rcx */
        "mov    %0, %%rsp\n\t"      /* 切换到新栈 */
        "push   %%rcx\n\t"          /* 将旧 rbp 压入新栈 */
        "mov    %%rsp, %%rbp\n\t"   /* 设置新的 rbp */
        "push   %%rax\n\t"          /* 将返回地址压入新栈 */
        :
        : "r"(new_sp)
        : "rax", "rcx", "memory"
    );
#elif ARCH_RISCV64
    __asm__ volatile("mv sp, %0" :: "r"(new_sp) : "memory");
#elif ARCH_AARCH64
    __asm__ volatile("mov sp, %0" :: "r"(new_sp) : "memory");
#endif

    KLOG_INFO("[task] switched to idle stack at 0x%lx\n", (unsigned long)new_sp);
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

    KLOG_INFO("[task] Created page table for '%s': PGD=0x%llx\n",
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

        KLOG_INFO("[task] Created RISC-V user PGD=0x%llx for '%s'\n",
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

        KLOG_INFO("[task] Created x86_64 user PGD=0x%llx for '%s'\n",
                  pgd_phys, name);
    }
#else
    /* 其他架构暂时使用共享内核页表 */
    task->pgd = NULL;
    task->user_entry = user_entry;
    KLOG_INFO("[task] Using shared kernel page table for '%s'\n", name);
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

    /* 加入就绪队列 */
    sched_enqueue(task);

    KLOG_INFO("[task] created user process '%s' id=%u prio=%u\n",
              task->name, task->id, (uint32_t)task->priority);
    KLOG_INFO("[task]   user_entry=0x%llx (adjusted), user_sp=0x%llx, pgd=0x%llx\n",
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

    KLOG_DEBUG("[task] fd_table initialized: pid=%u [0]=%d(u=%u) [3]=%d(u=%u) [255]=%d(u=%u) sizeof=%zu\n",
              task->id, task->fd_table[0], (unsigned)task->fd_table[0],
              task->fd_table[3], (unsigned)task->fd_table[3],
              task->fd_table[255], (unsigned)task->fd_table[255],
              sizeof(task->fd_table[0]));

    task->parent_id  = g_current_task ? g_current_task->id : 0;
    task->exit_status = 0;
    task->is_waiting  = false;
    task->wait_pid    = (uint32_t)-1;
    task->utime_ns    = 0;
    task->stime_ns    = 0;
    task->sc_entry_ns = 0;
    task->create_ns   = task_get_ns();

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

    KLOG_INFO("[task] created user process '%s' id=%u prio=%u (pgd=0x%llx)\n",
              task->name, task->id, (uint32_t)task->priority, pgd_phys);
    KLOG_INFO("[task]   user_entry=0x%llx, user_sp=0x%llx, kernel_sp=0x%lx\n", 
              user_entry, user_sp, (unsigned long)task->sp);

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

    KLOG_INFO("[task] created '%s' id=%u prio=%u sp=0x%lx\n",
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
    KLOG_INFO("[task] '%s' (id=%u) exiting\n", cur->name, cur->id);

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
    barrier_compiler();  // 编译器屏障，确保每次都重新读取
    return g_current_task;
}

/* ── task_block ──────────────────────────────────────────── */

void
task_block(list_t *wait_queue)
{
    task_t *cur = task_current();

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
