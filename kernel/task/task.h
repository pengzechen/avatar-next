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

/* ── 信号常量 ────────────────────────────────────────────── */
#define NSIG     64
#define SIG_DFL  0ULL   /* 默认动作（终止） */
#define SIG_IGN  1ULL   /* 忽略 */

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGWINCH 28

#define SA_RESTORER  0x04000000ULL

/* ── 信号动作结构 ────────────────────────────────────────── */
typedef struct {
    uint64_t sa_handler;    /* SIG_DFL / SIG_IGN / 用户 handler 地址 */
    uint64_t sa_flags;      /* SA_RESTORER 等标志                     */
    uint64_t sa_restorer;   /* rt_sigreturn 蹦床地址                  */
    uint64_t sa_mask;       /* handler 执行期间额外屏蔽的信号         */
} sig_action_t;

/* ── Task states ─────────────────────────────────────────── */
typedef enum {
    TASK_ALLOCATING = -1,  /* 槽位已占用，但 TCB 尚未完成初始化     */
    TASK_READY   = 0,   /* 在就绪队列中，等待调度               */
    TASK_RUNNING = 1,   /* 当前正在 CPU 上运行                  */
    TASK_BLOCKED = 2,   /* 等待事件，不在就绪队列中              */
    TASK_DEAD    = 3,   /* 已退出，资源待回收                   */
} task_state_t;

/* ── Configuration ───────────────────────────────────────── */
#define TASK_STACK_SIZE  16384u  /* 每个内核任务的栈大小（16 KiB，增加以防止 syscall 栈溢出） */
#define TASK_NAME_LEN    16u     /* 任务名最大长度（含 NUL）      */
#define TASK_MAX         64u     /* 最大并发任务数（不含 idle）   */
#define TASK_CWD_LEN     128u    /* 当前工作目录最大长度          */
#define TASK_EXE_LEN     128u    /* 可执行文件路径最大长度        */
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

    /* === SMP 支持（Phase 0：仅占位，调度器在 Phase 1 开始读取） === */
    uint32_t        cpu_affinity;        /* 绑定的逻辑 CPU 编号。0..N-1 为硬亲和性；
                                          * CPU_AFFINITY_ANY 代表“任一”，
                                          * 由 sched_enqueue 以 round-robin
                                          * 选一个物理核。 */

    /* === 用户态支持 === */
    bool            is_user_process;     /* true=用户进程, false=内核任务          */
    bool            user_started;        /* true=已至少进入过一次用户态            */
    bool            is_thread;           /* true=线程(共享页表), false=独立进程 */
    uint64_t       *pgd;                 /* 页表基址（用户进程的TTBR0）            */
    uint64_t        user_entry;          /* 用户态入口点（虚拟地址）               */
    uint64_t        user_sp;             /* 用户栈指针（虚拟地址）                */
    uint64_t        user_stack_top;      /* 用户栈顶（虚拟地址）                  */
    uint64_t        user_stack_size;     /* 用户栈大小                            */
    uint64_t        heap_end;            /* 进程堆当前末尾（brk 系统调用使用）    */
    uint64_t        mmap_next;           /* 下一个 mmap 分配的起始地址            */
    uint64_t        fs_base;             /* x86_64 TLS: IA32_FS_BASE              */
    uint64_t        ctid_ptr;            /* CLONE_CHILD_CLEARTID 地址 (0=无)       */

    /* === 进程/文件系统支持 === */
    char            cwd[TASK_CWD_LEN];  /* 当前工作目录（用户进程）               */
    char            exe_path[TASK_EXE_LEN]; /* 可执行文件路径（/proc/self/exe）   */
    int16_t         fd_table[TASK_MAX_FD]; /* FD → g_fd_pool 索引，-1=未打开     */
    uint8_t         fd_cloexec[TASK_MAX_FD / 8]; /* FD_CLOEXEC 位图              */
    uint32_t        parent_id;           /* 父进程 ID                              */
    uint32_t        uid;                 /* real user ID                           */
    uint32_t        euid;                /* effective user ID                      */
    uint32_t        gid;                 /* real group ID                          */
    uint32_t        egid;                /* effective group ID                     */
    int             exit_status;         /* 退出状态（wait4 使用）                 */
    int             exit_signal;         /* 被信号杀死时的信号号（0=正常退出）     */
    bool            is_waiting;          /* 正在 wait4 子进程                      */
    uint32_t        wait_pid;            /* 等待的子进程 PID（-1=任意）            */

    /* === CPU 时间统计 === */
    uint64_t        utime_ns;            /* 用户态 CPU 时间（纳秒），task_exit 前计算  */
    uint64_t        stime_ns;            /* 内核态 CPU 时间（纳秒），syscall 路径累积  */
    uint64_t        sc_entry_ns;         /* 当前 syscall 入口时间戳（0=不在 syscall）  */
    uint64_t        create_ns;           /* 进程创建时间戳（用于 utime = wall−stime）  */

    /* === 信号系统 === */
    uint64_t        pending_sigs;        /* 待投递信号位图，bit(N-1) = 信号 N           */
    uint64_t        blocked_sigs;        /* 被阻塞信号位图（sigprocmask）               */
    uint64_t        sig_saved_blocked;   /* signal 投递前保存的 blocked_sigs            */
    uint32_t        pgid;                /* 进程组 ID                                   */
    uint32_t        sid;                 /* 会话 ID (session leader = sid == id)         */
    int16_t         ctty_pty_idx;        /* controlling tty PTY index, -1 if detached    */
    uint64_t        sig_frame_sp;        /* sigframe 在用户栈上的起始地址（rt_sigreturn）*/
    sig_action_t    sig_actions[NSIG];   /* 每信号的 action（下标 0 对应信号 1）        */

    /* === 内核抢占控制 === */
    uint32_t        preempt_count;       /* >0 时 S-mode timer 不抢占该任务             */
} task_t;

/* 前台进程组 ID（0 = 无前台进程）*/
extern volatile uint32_t g_fg_pgid;

/* ── SMP 调度亲和性 ──────────────────────────────────────────── */
/* "任一核"哨兵值； sched_enqueue 看到后会用 round-robin 选目标。 */
#define CPU_AFFINITY_ANY  ((uint32_t)-1)

/**
 * task_set_cpu_affinity - 将任务迁移到指定逻辑 CPU
 * @task:    目标任务（仅允许在任务还未开始运行、还在某核
 *           run_queue 的状态下调用；如从 task_create 返回后、
 *           任务首次被 schedule 之前）
 * @cpu_id:  目标逻辑 CPU（0..g_num_cpus-1）或 CPU_AFFINITY_ANY
 *
 * 原子地从当前所在核 rq 中取出（若仍在队）并重新入队到目标核。
 * VCPU 任务请绑 cpu_id=0：per-CPU 虚拟化状态（GICH list regs / vtimer /
 * VMCS）尚未跨核迁移。
 */
void task_set_cpu_affinity(task_t *task, uint32_t cpu_id);

/* ── Public API ──────────────────────────────────────────── */

/**
 * task_init - 初始化任务子系统
 *
 * 将当前执行上下文（boot 线程）注册为 idle 任务，
 * 并初始化调度器。必须在调用其他 task_* 函数前调用。
 */
void task_init(void);

/**
 * task_switch_to_idle_stack - 切换到 idle 专用栈并进入 idle 循环
 *
 * 必须在 task_init() 返回后调用。切换栈后**永不返回**，直接进入
 * idle 主循环（task_yield + wfi/wfe/hlt）。
 * 这样可以避免切栈后走 C 函数 epilogue 时从未初始化的新栈读 LR/RBP
 * 导致跳到 0（ELR=0 异常）。
 * 防止频繁中断导致 boot 栈溢出。
 */
void task_switch_to_idle_stack(void) __attribute__((noreturn));

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
task_t *process_create(const char *name, uint64_t user_entry, uint64_t user_code_size,
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
 * task_reap_dead - 回收已退出任务的延迟释放资源
 * @task: state 必须为 TASK_DEAD 的任务
 *
 * 只能在该任务已经离开 CPU 后调用。会释放独立用户页表/用户物理页和静态任务槽。
 */
void task_reap_dead(task_t *task);

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

/**
 * task_find_by_id - 按 ID 查找非 DEAD 任务
 * @id: 任务 ID
 * 返回 task_t* 或 NULL
 */
task_t *task_find_by_id(uint32_t id);

/**
 * task_send_signal - 向任务投递信号（可在中断上下文调用）
 * @t: 目标任务（NULL 则忽略）
 * @sig: 信号号 (1..NSIG)
 */
void task_send_signal(task_t *t, int sig);

/**
 * task_send_signal_to_pgid - 向进程组内所有用户态任务投递信号
 * @pgid: 进程组 ID（0 无效）
 * @sig:  信号号
 * @return: 收到信号的进程数量
 */
int task_send_signal_to_pgid(uint32_t pgid, int sig);

/**
 * signal_check_uart - 扫描 UART 输入，
 * 将普通字符存入环形缓冲区，Ctrl+C 发 SIGINT 给前台进程组。
 * 可在中断和任务上下文中调用。
 */
void signal_check_uart(void);

#endif /* KERNEL_TASK_TASK_H */
