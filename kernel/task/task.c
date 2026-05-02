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

/* ── 静态任务池 ──────────────────────────────────────────── */

static task_t   g_task_pool[TASK_MAX];

/*
 * 任务栈池：16 字节对齐，确保 AArch64/x86_64 的 SP 对齐要求。
 * 每个栈独立分配，互不重叠。
 */
static uint8_t  g_task_stacks[TASK_MAX][TASK_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t  g_stack_used[TASK_MAX];

static uint32_t g_task_id_cnt = 0;

/* ── idle 任务（boot 执行上下文）────────────────────────── */
static task_t g_idle_task;

/* ── 当前任务指针（在 task.h 中 extern 声明）────────────── */
task_t *g_current_task = NULL;

/* ── 内部：分配/释放任务槽 ──────────────────────────────── */

static task_t *
alloc_task_slot(void)
{
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (!g_stack_used[i]) {
            g_stack_used[i]           = 1;
            g_task_pool[i].stack_base = g_task_stacks[i];
            return &g_task_pool[i];
        }
    }
    return NULL;
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

    /*
     * 从就绪队列移除（理论上已不在队列，因为 RUNNING 时会重入队，
     * 但 sched_dequeue 做了安全检查）。
     */
    sched_dequeue(cur);

    /* 释放任务槽（idle 的 stack_base = NULL，不释放） */
    if (cur->stack_base) {
        free_task_slot(cur);
    }

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
