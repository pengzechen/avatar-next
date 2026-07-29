/*
 * syscall_internal.h - 内核 syscall 实现内部共享头
 *
 * 仅供 kernel/syscall 目录下的 C 文件使用。包含：
 *   - Linux/x86 syscall 号常量
 *   - ioctl request 码
 *   - POSIX errno 值
 *   - 与 musl/glibc 用户空间对齐的 kernel-side struct
 *   - CLONE/FUTEX/MAP/SIG_* 标志
 *
 * 不要在驱动或用户接口中包含。
 */
#ifndef KERNEL_SYSCALL_INTERNAL_H
#define KERNEL_SYSCALL_INTERNAL_H

#include "types.h"
#include "arch.h"

/* 前向声明（避免拉入 task/task.h 的整套依赖） */
struct task;
typedef struct task task_t;

/* ── Linux AArch64 / RISC-V 标准系统调用号 ──────────────────────── */
#define LINUX_SYS_GETCWD          17
#define LINUX_SYS_DUP3            24
#define LINUX_SYS_FCNTL           25
#define LINUX_SYS_IOCTL           29
#define LINUX_SYS_MKDIRAT         34
#define LINUX_SYS_UNLINKAT        35
#define LINUX_SYS_RENAMEAT        38
#define LINUX_SYS_FTRUNCATE       46
#define LINUX_SYS_FACCESSAT       48
#define LINUX_SYS_CHDIR           49
#define LINUX_SYS_OPENAT          56
#define LINUX_SYS_CLOSE           57
#define LINUX_SYS_PIPE2           59
#define LINUX_SYS_GETDENTS64      61
#define LINUX_SYS_LSEEK           62
#define LINUX_SYS_READ            63
#define LINUX_SYS_WRITE           64
#define LINUX_SYS_READV           65
#define LINUX_SYS_WRITEV          66
#define LINUX_SYS_SENDFILE        71   /* AArch64/RISC-V sendfile64 */
#define LINUX_SYS_PSELECT6        72
#define LINUX_SYS_PPOLL           73
#define LINUX_SYS_READLINKAT      78
#define LINUX_SYS_NEWFSTATAT      79
#define LINUX_SYS_FSTAT           80
#define LINUX_SYS_FSYNC           82
#define LINUX_SYS_FDATASYNC       83
#define LINUX_SYS_EXIT            93
#define LINUX_SYS_EXIT_GROUP      94
#define LINUX_SYS_WAITID          95
#define LINUX_SYS_SET_TID_ADDR    96
#define LINUX_SYS_FUTEX           98
#define LINUX_SYS_SET_ROBUST_LIST 99
#define LINUX_SYS_NANOSLEEP       101
#define LINUX_SYS_CLOCK_GETTIME   113
#define LINUX_SYS_SCHED_YIELD     124
#define LINUX_SYS_KILL            129
#define LINUX_SYS_TKILL           130
#define LINUX_SYS_TGKILL          131
#define LINUX_SYS_RT_SIGACTION    134
#define LINUX_SYS_RT_SIGPROCMASK  135
#define LINUX_SYS_RT_SIGPENDING   136
#define LINUX_SYS_RT_SIGRETURN    139
#define LINUX_SYS_SETGID          144
#define LINUX_SYS_SETUID          146
#define LINUX_SYS_SETPGID         154
#define LINUX_SYS_GETPGID         155
#define LINUX_SYS_GETSID          156
#define LINUX_SYS_SETSID          157
#define LINUX_SYS_GETGROUPS       158
#define LINUX_SYS_SETGROUPS       159
#define LINUX_SYS_UNAME           160
#define LINUX_SYS_GETRLIMIT       163
#define LINUX_SYS_SETRLIMIT       164
#define LINUX_SYS_GETRUSAGE       165
#define LINUX_SYS_UMASK           166
#define LINUX_SYS_PRCTL           167
#define LINUX_SYS_GETPID          172
#define LINUX_SYS_GETPPID         173
#define LINUX_SYS_GETUID          174
#define LINUX_SYS_GETEUID         175
#define LINUX_SYS_GETGID          176
#define LINUX_SYS_GETEGID         177
#define LINUX_SYS_GETTID          178
#define LINUX_SYS_SOCKET          198
#define LINUX_SYS_SOCKETPAIR      199
#define LINUX_SYS_BIND            200
#define LINUX_SYS_LISTEN          201
#define LINUX_SYS_ACCEPT          202
#define LINUX_SYS_CONNECT         203
#define LINUX_SYS_GETSOCKNAME     204
#define LINUX_SYS_GETPEERNAME     205
#define LINUX_SYS_SENDTO          206
#define LINUX_SYS_RECVFROM        207
#define LINUX_SYS_SETSOCKOPT      208
#define LINUX_SYS_GETSOCKOPT      209
#define LINUX_SYS_SHUTDOWN        210
#define LINUX_SYS_SENDMSG         211
#define LINUX_SYS_RECVMSG         212
#define LINUX_SYS_ACCEPT4         242
#define LINUX_SYS_BRK             214
#define LINUX_SYS_MUNMAP          215
#define LINUX_SYS_CLONE           220
#define LINUX_SYS_EXECVE          221
#define LINUX_SYS_MMAP            222
#define LINUX_SYS_MPROTECT        226
#define LINUX_SYS_WAIT4           260
#define LINUX_SYS_PRLIMIT64       261
#define LINUX_SYS_GETRANDOM       278

/* ── epoll (AArch64 / RISC-V 号) ─────────────────────────────────── */
#define LINUX_SYS_EPOLL_CREATE1   20
#define LINUX_SYS_EPOLL_CTL       21
#define LINUX_SYS_EPOLL_PWAIT     22

/* x86_64 专属或翻译后的伪号（使用顶部空段避免与 Linux 号冲突） */
#define X86_SYS_ARCH_PRCTL        0x7FFFFFFDULL
#define X86_SYS_RSEQ              0x7FFFFFFCULL
#define X86_SYS_POLL              0x7FFFFFFBULL
#define X86_SYS_SELECT            0x7FFFFFFAULL

/* ── CLONE flags (来自 Linux <sched.h>) ──────────────────────────── */
#define CLONE_VM              0x00000100UL
#define CLONE_FS              0x00000200UL
#define CLONE_FILES           0x00000400UL
#define CLONE_SIGHAND         0x00000800UL
#define CLONE_THREAD          0x00010000UL
#define CLONE_SETTLS          0x00080000UL
#define CLONE_PARENT_SETTID   0x00100000UL
#define CLONE_CHILD_CLEARTID  0x00200000UL

/* ── futex 操作码 ────────────────────────────────────────────────── */
#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_PRIVATE_FLAG    128
#define FUTEX_CLOCK_REALTIME  256

/* ── sigprocmask how 值 ──────────────────────────────────────────── */
#define SIG_BLOCK             0
#define SIG_UNBLOCK           1
#define SIG_SETMASK           2

/* ── ioctl request codes ─────────────────────────────────────────── */
#define TCGETS                0x5401
#define TCSETS                0x5402
#define TCSETSW               0x5403
#define TCSETSF               0x5404
#define TIOCGWINSZ            0x5413
#define TIOCSWINSZ            0x5414
#define TIOCGPGRP             0x540f
#define TIOCSPGRP             0x5410
#define TIOCSCTTY             0x540e
#define TIOCNOTTY             0x5422
#define TIOCGPTN              0x80045430
#define TIOCSPTLCK            0x40045431

/* ── mmap 标志 ───────────────────────────────────────────────────── */
#define MAP_SHARED            0x01
#define MAP_ANONYMOUS         0x20
#define MAP_PRIVATE           0x02
#define MAP_FIXED             0x10
#define MMAP_FAILED           ((uint64_t)(int64_t)-1)

/* ── POSIX errno 值 ──────────────────────────────────────────────── */
#define EPERM                  1
#define ENOSYS                38
#define ESRCH                  3
#define EBADF                  9
#define EIO                    5
#define EINVAL                22
#define ENOENT                 2
#define ENOMEM                12
#define EAGAIN                11
#define EINTR                  4
#define EFAULT                14
#define ERANGE                34
#define ECHILD                10
#define ENOTDIR               20
#define EISDIR                21
#define ENFILE                23
#define EMFILE                24
#define ENOTTY                25
#define ENOTSUP               95
#define EAFNOSUPPORT          97
#define EADDRINUSE            98
#define EADDRNOTAVAIL         99
#define ENETUNREACH          101
#define ECONNABORTED         103
#define ECONNRESET           104
#define ENOBUFS              105
#define EISCONN              106
#define ENOTCONN             107
#define ETIMEDOUT            110
#define ECONNREFUSED         111
#define EINPROGRESS          115
#define EALREADY             114
#define EPIPE                 32

/* ── *at 系列标志 ────────────────────────────────────────────────── */
#define AT_FDCWD              -100
#define AT_REMOVEDIR          0x200

/* ── x86_64 MSR / arch_prctl 常量 ────────────────────────────────── */
#if ARCH_X86_64
#define X86_MSR_IA32_FS_BASE  0xC0000100U
#define X86_ARCH_SET_FS       0x1002UL
#define X86_ARCH_GET_FS       0x1003UL
#endif

/* ── Linux termios (TCGETS/TCSETS) 最小 ABI 视图 ─────────────────── */
struct kernel_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
};

/* ── 内核侧 sigaction 布局（与 musl/glibc 用户空间对齐）─────────── */
struct kernel_sigaction {
    uint64_t sa_handler;    /* SIG_DFL(0) / SIG_IGN(1) / 用户 handler 地址 */
    uint64_t sa_flags;      /* SA_RESTORER 等标志 */
    uint64_t sa_restorer;   /* rt_sigreturn 蹦床（SA_RESTORER 置位时有效）*/
    uint64_t sa_mask;       /* handler 执行期间额外屏蔽的信号位图 (8 bytes) */
    uint8_t  _pad[128 - 8]; /* musl sigset_t 占 128 bytes, 内核只用前 8 */
};

/* ── TIOCGWINSZ 输出 ─────────────────────────────────────────────── */
struct kernel_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

/* ── poll / ppoll 用 ─────────────────────────────────────────────── */
struct kernel_pollfd {
    int   fd;
    short events;
    short revents;
};

/* ── uname 输出 ──────────────────────────────────────────────────── */
struct kernel_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

/* ── writev 用 ───────────────────────────────────────────────────── */
struct kernel_iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};

/* ── clock_gettime / nanosleep 用 ────────────────────────────────── */
struct kernel_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* ── getrlimit / setrlimit / prlimit64 用 ────────────────────────── */
struct kernel_rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

/* ═══════════════════════════════════════════════════════════════════
 * 内部共享 helper：跨多个 syscall 子模块使用
 * ═══════════════════════════════════════════════════════════════════ */

/* ── 硬件单调时钟（arch 无关接口）─────────────────────────────── */
#include "timer/timer.h"

static inline uint64_t kernel_get_ns(void)
{
    return timer_get_ns();
}

/* ── 由 fs/tty.c 提供，跨模块可见 ─────────────────────────────── */
void signal_check_uart(void);
int  uart_ringbuf_empty(void);

/* syscall 入口次数计数（早期调试日志使用）*/
extern volatile uint32_t g_syscall_entry_count;

#if ARCH_X86_64
/* 由 mm/pmap_compat.c 提供，由 task switch / arch_prctl 调用 */
void x86_write_fs_base(uint64_t fs_base);
#endif

/* ── syscall case 处理器：在 syscall.c 的 switch 中以一行调用形式分派
 *    每个 handler 将结果写回 regs[0]
 * ─────────────────────────────────────────────────────────────── */
void poll_handler     (uint64_t regs[6], uint64_t syscall_num, task_t *current);
void select_handler   (uint64_t regs[6], uint64_t syscall_num, task_t *current);
void epoll_create1_handler(uint64_t regs[6], task_t *current);
void epoll_ctl_handler    (uint64_t regs[6], task_t *current);
void epoll_pwait_handler  (uint64_t regs[6], task_t *current);
void getrandom_handler(uint64_t regs[6]);
#if ARCH_X86_64
void arch_prctl_handler(uint64_t regs[6], task_t *current);
#endif

/* ── 信号子系统（kernel/syscall/core/signal.c）─────────────────────── */
/* trap_frame_t 在各架构 include/<arch>/exception.h 中定义为匿名 struct
 * 的 typedef，无法前向声明，因此这里直接 include。 */
#include "exception.h"

void deliver_pending_signals(task_t *t, trap_frame_t *frame);
void signal_deliver_from_trap(void *frame_ptr);
void sigaction_handler   (uint64_t regs[6], task_t *current);
void sigprocmask_handler (uint64_t regs[6], task_t *current);
void sigpending_handler  (uint64_t regs[6], task_t *current);
void sigreturn_handler   (uint64_t regs[6], task_t *current, trap_frame_t *frame);
void kill_handler        (uint64_t regs[6], task_t *current);
void tkill_handler       (uint64_t regs[6]);
void tgkill_handler      (uint64_t regs[6]);

/* ── core/ 子系统：进程/线程/调度 ─────────────────────────────── */
/* 进程生命周期（core/proc_lifecycle.c） */
void exit_handler   (uint64_t regs[6]) __attribute__((noreturn));
void clone_handler  (uint64_t regs[6], task_t *parent, trap_frame_t *frame);
void execve_handler (uint64_t regs[6]);
void wait_handler   (uint64_t regs[6], task_t *me);

/* 进程身份/进程组/会话（core/proc_ids.c） */
void proc_ids_handler(uint64_t syscall_num, uint64_t regs[6], task_t *current);

/* 调度（core/sched.c） */
void sched_yield_handler(uint64_t regs[6]);
void nanosleep_handler  (uint64_t regs[6]);

/* futex 派发（core/futex.c） */
void futex_handler(uint64_t regs[6]);

/* ── 路径/字符串辅助（fs/path.c，核心模块共享） ────────────────── */
void resolve_path(const char *cwd, const char *path, char *out, int outlen);
int  copy_string_from_user(const char *ustr, char *kbuf, int maxlen);
int  copy_string_to_user  (const char *kstr, char *ubuf, int maxlen);
int  copy_from_user_bytes (const void *usrc, void *kdst, uint64_t len);
int  copy_to_user_bytes   (const void *ksrc, void *udst, uint64_t len);

/* ── fs/ 子系统：文件 / 目录 / TTY ─────────────────────────────── */
/* 文件 I/O（fs/file_io.c）*/
void read_handler    (uint64_t regs[6], task_t *current);
void readv_handler   (uint64_t regs[6], task_t *current);
void write_handler   (uint64_t regs[6], task_t *current);
void writev_handler  (uint64_t regs[6], task_t *current);
void lseek_handler   (uint64_t regs[6], task_t *current);
void sendfile_handler(uint64_t regs[6], task_t *current);

/* 文件操作（fs/file_ops.c）*/
void openat_handler(uint64_t regs[6], task_t *current);
void close_handler (uint64_t regs[6], task_t *current);
void dup3_handler  (uint64_t regs[6], task_t *current);
void fcntl_handler (uint64_t regs[6], task_t *current);
void pipe2_handler (uint64_t regs[6], task_t *current);

/* stat / 目录读取（fs/file_stat.c）*/
void fstat_handler      (uint64_t regs[6], task_t *current);
void newfstatat_handler (uint64_t regs[6], task_t *current);
void readlinkat_handler (uint64_t regs[6], task_t *current);
void faccessat_handler  (uint64_t regs[6], task_t *current);
void getdents64_handler (uint64_t regs[6], task_t *current);

/* 目录与命名（fs/dir.c）*/
void getcwd_handler  (uint64_t regs[6], task_t *current);
void chdir_handler   (uint64_t regs[6], task_t *current);
void renameat_handler(uint64_t regs[6], task_t *current);
void unlinkat_handler(uint64_t regs[6], task_t *current);

/* ioctl（fs/ioctl.c）*/
void ioctl_handler(uint64_t regs[6], task_t *current);

/* 网络 socket（net/sock_syscall.c）*/
void socket_handler    (uint64_t regs[6], task_t *current);
void bind_handler      (uint64_t regs[6], task_t *current);
void listen_handler    (uint64_t regs[6], task_t *current);
void accept_handler    (uint64_t regs[6], task_t *current);
void connect_handler   (uint64_t regs[6], task_t *current);
void sendto_handler    (uint64_t regs[6], task_t *current);
void recvfrom_handler  (uint64_t regs[6], task_t *current);
void setsockopt_handler(uint64_t regs[6], task_t *current);
void getsockopt_handler(uint64_t regs[6], task_t *current);
void getpeername_handler(uint64_t regs[6], task_t *current);
void getsockname_handler(uint64_t regs[6], task_t *current);
void shutdown_handler  (uint64_t regs[6], task_t *current);

#endif /* KERNEL_SYSCALL_INTERNAL_H */
