#ifndef KERNEL_TASK_TASK_H
#define KERNEL_TASK_TASK_H

/*
 * kernel/task/task.h — 内核任务控制块与公共 API
 *
 * 设计：
 *  - 仅内核态多任务，无用户态
 *  - 静态任务池，最多 TASK_MAX 个并发任务
 *  - 调度器 (sched.c) 实现轮转调度 (round-robin)
 *  - 上下文切换由 arch_task_switch() 完成（仅保存被调用者保存寄存器）
 */

#include "types.h"
#include "list.h"

/* ── Task states ─────────────────────────────────────────── */
typedef enum {
    TASK_READY   = 0,   /* 在就绪队列中，等待调度               */
    TASK_RUNNING = 1,   /* 当前正在 CPU 上运行                  */
    TASK_BLOCKED = 2,   /* 等待事件，不在就绪队列中              */
    TASK_DEAD    = 3,   /* 已退出，资源待回收                   */
} task_state_t;

/* ── Configuration ───────────────────────────────────────── */
#define TASK_STACK_SIZE  8192u   /* 每个内核任务的栈大小（8 KiB） */
#define TASK_NAME_LEN    16u     /* 任务名最大长度（含 NUL）      */
#define TASK_MAX         16u     /* 最大并发任务数（不含 idle）   */

/* ── Task Control Block ──────────────────────────────────── */
typedef struct task {
    uintptr_t       sp;                  /* 保存的内核栈指针（上下文切换时填入） */
    task_state_t    state;               /* 任务状态                              */
    uint32_t        id;                  /* 唯一任务 ID                           */
    uint8_t         priority;            /* 优先级（0 = 最高，255 = 最低）        */
    char            name[TASK_NAME_LEN]; /* 任务名称                              */
    uint8_t        *stack_base;          /* 内核栈底（低地址）；idle 为 NULL      */
    void          (*entry)(void *);      /* 任务入口函数                          */
    void           *arg;                 /* 传给 entry 的参数                     */
    list_node_t     run_node;            /* 就绪队列节点                          */
} task_t;

/* ── 全局当前任务指针（在 task.c 中定义） ────────────────── */
extern task_t *g_current_task;

/* ── Public API ──────────────────────────────────────────── */

/**
 * task_init - 初始化任务子系统
 *
 * 将当前执行上下文（boot 线程）注册为 idle 任务，
 * 并初始化调度器。必须在调用其他 task_* 函数前调用。
 */
void task_init(void);

/**
 * task_create - 创建内核任务
 * @name:     任务名称（最长 TASK_NAME_LEN-1 字节）
 * @entry:    任务入口函数
 * @arg:      传给 entry 的参数
 * @priority: 优先级（0 = 最高，255 = 最低）
 *
 * 成功返回 task_t*，任务池已满时返回 NULL。
 * 新任务立即加入就绪队列，下次调度时开始运行。
 */
task_t *task_create(const char *name, void (*entry)(void *), void *arg,
                    uint8_t priority);

/**
 * task_yield - 主动让出 CPU
 *
 * 调用者进入就绪队列尾部，调度器切换到下一个就绪任务。
 */
void task_yield(void);

/**
 * task_exit - 终止当前任务（调用后不会返回）
 *
 * 将任务标记为 DEAD，从就绪队列移除，回收栈，
 * 然后切换到下一个任务。
 */
void task_exit(void) __attribute__((noreturn));

/**
 * task_current - 返回当前正在运行的任务指针
 */
task_t *task_current(void);

#endif /* KERNEL_TASK_TASK_H */
