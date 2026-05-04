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
#define TASK_CWD_LEN     128u    /* 当前工作目录最大长度          */
#define TASK_MAX_FD      256u    /* 每进程最大文件描述符数        */

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
    list_node_t     wait_node;           /* 等待队列节点（用于 mutex/semaphore）  */

    /* === 用户态支持 === */
    bool            is_user_process;     /* true=用户进程, false=内核任务          */
    bool            user_started;        /* true=已至少进入过一次用户态            */
    uint64_t       *pgd;                 /* 页表基址（用户进程的TTBR0）            */
    uint64_t        user_entry;          /* 用户态入口点（虚拟地址）               */
    uint64_t        user_sp;             /* 用户栈指针（虚拟地址）                */
    uint64_t        user_stack_top;      /* 用户栈顶（虚拟地址）                  */
    uint64_t        user_stack_size;     /* 用户栈大小                            */
    uint64_t        heap_end;            /* 进程堆当前末尾（brk 系统调用使用）    */
    uint64_t        mmap_next;           /* 下一个 mmap 分配的起始地址            */

    /* === 进程/文件系统支持 === */
    char            cwd[TASK_CWD_LEN];  /* 当前工作目录（用户进程）               */
    int8_t          fd_table[TASK_MAX_FD]; /* FD → g_fd_pool 索引，-1=未打开     */
    uint32_t        parent_id;           /* 父进程 ID                              */
    int             exit_status;         /* 退出状态（wait4 使用）                 */
    bool            is_waiting;          /* 正在 wait4 子进程                      */
    uint32_t        wait_pid;            /* 等待的子进程 PID（-1=任意）            */
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
 * task_switch_to_idle_stack - 切换到 idle 专用栈
 *
 * 必须在 task_init() 返回后、进入 idle 循环前调用。
 * 防止频繁中断导致 boot 栈溢出。
 */
void task_switch_to_idle_stack(void);

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
 * process_create - 创建用户进程
 * @name:       进程名称（最长 TASK_NAME_LEN-1 字节）
 * @user_entry: 用户态入口点（虚拟地址）
 * @user_sp:    用户栈指针（虚拟地址）
 * @priority:   优先级（0 = 最高，255 = 最低）
 *
 * 成功返回 task_t*，任务池已满时返回 NULL。
 * 新进程立即加入就绪队列，首次调度时跳转到用户态执行。
 *
 * 注意：当前版本使用共享内核页表，后续扩展为独立地址空间。
 */
task_t *process_create(const char *name, uint64_t user_entry,
                       uint64_t user_sp, uint8_t priority);

/**
 * process_create_with_pgd - 使用已有用户页表创建用户进程
 * @name:       进程名称（最长 TASK_NAME_LEN-1 字节）
 * @user_entry: 用户态入口点（用户虚拟地址，直接使用，不做转换）
 * @user_sp:    用户栈指针（虚拟地址）
 * @priority:   优先级（0 = 最高，255 = 最低）
 * @pgd_phys:   已创建好的用户页表物理地址
 *
 * 与 process_create 不同，跳过 vm_create_user_process，
 * 直接使用调用方已准备好的页表。用于 ELF 加载器等自行管理地址空间的场景。
 */
task_t *process_create_with_pgd(const char *name, uint64_t user_entry,
                                uint64_t user_sp, uint8_t priority,
                                uint64_t pgd_phys,
                                uint64_t heap_end, uint64_t mmap_next);

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

/**
 * task_block - 阻塞当前任务
 * @wait_queue: 等待队列（如果为 NULL，则不从队列移除）
 *
 * 将当前任务设置为 BLOCKED 状态，并触发调度。
 * 如果 wait_queue 非 NULL，将任务加入该队列。
 */
void task_block(list_t *wait_queue);

/**
 * task_unblock - 唤醒一个被阻塞的任务
 * @task: 要唤醒的任务
 *
 * 将任务从 BLOCKED 状态改为 READY，并加入就绪队列。
 * 如果任务在等待队列中，调用者应先将其从等待队列移除。
 */
void task_unblock(task_t *task);

#endif /* KERNEL_TASK_TASK_H */
