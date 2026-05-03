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

#if ARCH_AARCH64
#include "mm/aarch64/vm_user.h"
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

/* ── idle 任务（boot 执行上下文）────────────────────────── */
static task_t g_idle_task;

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
     * idle 的 sp 字段在首次被 arch_task_switch 抢占时自动填入。
     * stack_base = NULL 表示使用 boot 栈，不应被 free。
     */
    g_idle_task.sp         = 0;
    g_idle_task.state      = TASK_RUNNING;
    g_idle_task.id         = g_task_id_cnt++;
    g_idle_task.priority   = 255; /* 最低优先级 */
    g_idle_task.stack_base = NULL;
    g_idle_task.entry      = NULL;
    g_idle_task.arg        = NULL;
    list_node_init(&g_idle_task.run_node);
    list_node_init(&g_idle_task.wait_node);

    /* 手动 strcpy（避免依赖外部库） */
    g_idle_task.name[0] = 'i';
    g_idle_task.name[1] = 'd';
    g_idle_task.name[2] = 'l';
    g_idle_task.name[3] = 'e';
    g_idle_task.name[4] = '\0';

    g_current_task = &g_idle_task;

    /* 初始化调度器，传入 idle 任务 */
    sched_init(&g_idle_task);

    KLOG_INFO("[task] subsystem initialized, idle task id=%u\n", g_idle_task.id);
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
process_create(const char *name, uint64_t user_entry, uint64_t user_sp, uint8_t priority)
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
    task->user_entry      = user_entry;
    task->user_sp         = user_sp;
    task->user_stack_top  = user_sp;
    task->user_stack_size = 0x100000;  /* 1MB 用户栈 */

    /* 创建独立的用户页表 */
#if ARCH_AARCH64
    /* 用户代码的虚拟地址（内核地址空间） */
    uint64_t user_code_vaddr = user_entry;
    uint64_t user_code_size = 0x4000;       /* 16KB 代码段 */

    task->pgd = (uint64_t *)vm_create_user_process(user_code_vaddr, user_code_size,
                                                      user_sp, task->user_stack_size);
    if (task->pgd == NULL) {
        KLOG_ERROR("[task] Failed to create user page table for '%s'\n", name);
        free_task_slot(task);
        return NULL;
    }

    /* 设置用户入口地址为用户虚拟地址（0x10000） */
    task->user_entry = 0x10000;

    KLOG_INFO("[task] Created page table for '%s': PGD=0x%llx\n",
              name, (uint64_t)task->pgd);
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
                                    task->user_entry, user_sp);

    /* 加入就绪队列 */
    sched_enqueue(task);

    KLOG_INFO("[task] created user process '%s' id=%u prio=%u\n",
              task->name, task->id, (uint32_t)task->priority);
    KLOG_INFO("[task]   user_entry=0x%llx, user_sp=0x%llx, pgd=0x%llx\n",
              user_entry, user_sp, (uint64_t)task->pgd);

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
    task->user_entry      = user_entry;
    task->user_sp         = user_sp;
    task->user_stack_top  = user_sp;
    task->user_stack_size = 0x100000;  /* 1MB 用户栈 */
    task->pgd             = (uint64_t *)pgd_phys;

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
                                    task->user_entry, user_sp);

    sched_enqueue(task);

    KLOG_INFO("[task] created user process '%s' id=%u prio=%u (pgd=0x%llx)\n",
              task->name, task->id, (uint32_t)task->priority, pgd_phys);
    KLOG_INFO("[task]   user_entry=0x%llx, user_sp=0x%llx\n", user_entry, user_sp);

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
    task->pgd      = NULL;
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

    /* 设置阻塞状态 */
    cur->state = TASK_BLOCKED;

    /* 如果提供了等待队列，将任务加入 */
    if (wait_queue) {
        list_insert_last(wait_queue, &cur->wait_node);
    }

    /* 触发调度，切换到其他任务 */
    sched_schedule();
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
