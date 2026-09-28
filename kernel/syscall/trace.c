/*
 * kernel/syscall/trace.c — 系统调用环形缓冲的实现
 *
 * 设计要点见 trace.h。这里只补充三个实现上的选择：
 *
 * 1. **一条 syscall 只占一个槽位**：入口写满参数、返回值留哨兵，出口回填。
 *    如果入口/出口各写一条，环形缓冲的有效深度直接减半，而且崩在 syscall
 *    中间时会剩一个配不上对的孤立入口行 —— 那正是最需要看清楚的情况。
 *
 * 2. **无锁**：syscall 路径本来就关中断、且不迁移，所以同一个 CPU 只有
 *    它自己在写自己的环。跨 CPU 的先后顺序靠 at_ms + seq 在 dump 时归并。
 *
 * 3. **抓路径走 copy_string_from_user()**：来自 regs[] 的用户指针禁止裸解引用
 *    （用户传个非法地址就会让内核态读到坏地址 → #PF → 整机挂死，LTP 实测过）。
 */

#include "syscall/trace.h"
#include "syscall/syscall_internal.h" /* LINUX_SYS_*, copy_string_from_user */
#include "klog.h"
#include "string.h"
#include "arg.h"
#include "timer/timer.h"
#include "task/cpu.h" /* AVATAR_MAX_CPUS */

/* my_vsnprintf 取 va_list（和 lib/klog.c 用法一致），这里包一层变参入口。
 * 语义是 C99 snprintf：截断时返回"本该写多长"，所以调用方必须自己 clamp。 */
extern int my_vsnprintf(char *buf, int size, const char *fmt, va_list va);

static int trace_snprintf(char *buf, int size, const char *fmt, ...)
{
    va_list va;
    int r;

    va_start(va, fmt);
    r = my_vsnprintf(buf, size, fmt, va);
    va_end(va);

    return r;
}

/* ── 环形缓冲本体 ───────────────────────────────────────────────── */

static syscall_trace_rec_t s_ring[AVATAR_MAX_CPUS][SYSCALL_TRACE_DEPTH];
static uint32_t s_head[AVATAR_MAX_CPUS];    /* 下一个要写的槽位 */
static uint64_t s_seq[AVATAR_MAX_CPUS];     /* 每 CPU 单调序号 */
static uint32_t s_pending[AVATAR_MAX_CPUS]; /* 待回填的槽位 */
static uint8_t s_pending_valid[AVATAR_MAX_CPUS];

static inline uint32_t trace_cpu(void)
{
    uint32_t c = klog_cpu_id();
    return (c < AVATAR_MAX_CPUS) ? c : 0U;
}

uint32_t syscall_trace_count(void)
{
    uint32_t n = 0;

    for (uint32_t c = 0; c < AVATAR_MAX_CPUS; c++) {
        uint64_t s = s_seq[c];
        n += (uint32_t)((s < SYSCALL_TRACE_DEPTH) ? s : SYSCALL_TRACE_DEPTH);
    }
    return n;
}

/* ── syscall 号 → 名字 ──────────────────────────────────────────── */

const char *syscall_trace_name(uint32_t nr)
{
    switch (nr) {
    /* 文件 */
    case LINUX_SYS_OPENAT:
        return "openat";
    case LINUX_SYS_CLOSE:
        return "close";
    case LINUX_SYS_READ:
        return "read";
    case LINUX_SYS_WRITE:
        return "write";
    case LINUX_SYS_READV:
        return "readv";
    case LINUX_SYS_WRITEV:
        return "writev";
    case LINUX_SYS_PREAD64:
        return "pread64";
    case LINUX_SYS_PWRITE64:
        return "pwrite64";
    case LINUX_SYS_LSEEK:
        return "lseek";
    case LINUX_SYS_FSTAT:
        return "fstat";
    case LINUX_SYS_NEWFSTATAT:
        return "newfstatat";
    case LINUX_SYS_READLINKAT:
        return "readlinkat";
    case LINUX_SYS_FACCESSAT:
        return "faccessat";
    case LINUX_SYS_FACCESSAT2:
        return "faccessat2";
    case LINUX_SYS_GETCWD:
        return "getcwd";
    case LINUX_SYS_CHDIR:
        return "chdir";
    case LINUX_SYS_MKDIRAT:
        return "mkdirat";
    case LINUX_SYS_UNLINKAT:
        return "unlinkat";
    case LINUX_SYS_RENAMEAT:
        return "renameat";
    case LINUX_SYS_RENAMEAT2:
        return "renameat2";
    case LINUX_SYS_GETDENTS64:
        return "getdents64";
    case LINUX_SYS_FSYNC:
        return "fsync";
    case LINUX_SYS_FDATASYNC:
        return "fdatasync";
    case LINUX_SYS_FTRUNCATE:
        return "ftruncate";
    case LINUX_SYS_IOCTL:
        return "ioctl";
    case LINUX_SYS_FCNTL:
        return "fcntl";
    case LINUX_SYS_DUP:
        return "dup";
    case LINUX_SYS_DUP3:
        return "dup3";
    case LINUX_SYS_PIPE2:
        return "pipe2";
    case LINUX_SYS_SENDFILE:
        return "sendfile";

    /* 进程 / 内存 */
    case LINUX_SYS_EXECVE:
        return "execve";
    case LINUX_SYS_EXIT:
        return "exit";
    case LINUX_SYS_EXIT_GROUP:
        return "exit_group";
    case LINUX_SYS_CLONE:
        return "clone";
    case LINUX_SYS_WAIT4:
        return "wait4";
    case LINUX_SYS_WAITID:
        return "waitid";
    case LINUX_SYS_BRK:
        return "brk";
    case LINUX_SYS_MMAP:
        return "mmap";
    case LINUX_SYS_MUNMAP:
        return "munmap";
    case LINUX_SYS_MPROTECT:
        return "mprotect";
    case LINUX_SYS_MADVISE:
        return "madvise";
    case LINUX_SYS_GETPID:
        return "getpid";
    case LINUX_SYS_GETPPID:
        return "getppid";
    case LINUX_SYS_GETTID:
        return "gettid";
    case LINUX_SYS_SET_TID_ADDR:
        return "set_tid_address";
    case LINUX_SYS_SET_ROBUST_LIST:
        return "set_robust_list";
    case LINUX_SYS_SCHED_YIELD:
        return "sched_yield";
    case LINUX_SYS_NANOSLEEP:
        return "nanosleep";
    case LINUX_SYS_FUTEX:
        return "futex";
    case LINUX_SYS_UNAME:
        return "uname";
    case LINUX_SYS_GETRANDOM:
        return "getrandom";
    case LINUX_SYS_SYSINFO:
        return "sysinfo";
    case LINUX_SYS_GETRLIMIT:
        return "getrlimit";
    case LINUX_SYS_SETRLIMIT:
        return "setrlimit";
    case LINUX_SYS_PRLIMIT64:
        return "prlimit64";
    case LINUX_SYS_GETRUSAGE:
        return "getrusage";
    case LINUX_SYS_UMASK:
        return "umask";
    case LINUX_SYS_PRCTL:
        return "prctl";
    case LINUX_SYS_CLOCK_GETTIME:
        return "clock_gettime";

    /* 信号 */
    case LINUX_SYS_KILL:
        return "kill";
    case LINUX_SYS_TKILL:
        return "tkill";
    case LINUX_SYS_TGKILL:
        return "tgkill";
    case LINUX_SYS_RT_SIGACTION:
        return "rt_sigaction";
    case LINUX_SYS_RT_SIGPROCMASK:
        return "rt_sigprocmask";
    case LINUX_SYS_RT_SIGPENDING:
        return "rt_sigpending";
    case LINUX_SYS_RT_SIGRETURN:
        return "rt_sigreturn";

    /* 网络 */
    case LINUX_SYS_SOCKET:
        return "socket";
    case LINUX_SYS_BIND:
        return "bind";
    case LINUX_SYS_LISTEN:
        return "listen";
    case LINUX_SYS_ACCEPT:
        return "accept";
    case LINUX_SYS_ACCEPT4:
        return "accept4";
    case LINUX_SYS_CONNECT:
        return "connect";
    case LINUX_SYS_SENDTO:
        return "sendto";
    case LINUX_SYS_RECVFROM:
        return "recvfrom";
    case LINUX_SYS_SENDMSG:
        return "sendmsg";
    case LINUX_SYS_RECVMSG:
        return "recvmsg";
    case LINUX_SYS_SETSOCKOPT:
        return "setsockopt";
    case LINUX_SYS_GETSOCKOPT:
        return "getsockopt";
    case LINUX_SYS_SHUTDOWN:
        return "shutdown";
    case LINUX_SYS_GETSOCKNAME:
        return "getsockname";
    case LINUX_SYS_GETPEERNAME:
        return "getpeername";
    case LINUX_SYS_SOCKETPAIR:
        return "socketpair";

    /* 多路复用 */
    case LINUX_SYS_PSELECT6:
        return "pselect6";
    case LINUX_SYS_PPOLL:
        return "ppoll";
    case LINUX_SYS_EPOLL_CREATE1:
        return "epoll_create1";
    case LINUX_SYS_EPOLL_CTL:
        return "epoll_ctl";
    case LINUX_SYS_EPOLL_PWAIT:
        return "epoll_pwait";
    default:
        return "?";
    }
}

/*
 * 路径型 syscall：哪个参数是用户态路径串。
 * 判断错的后果是往环里存一段无意义的字节，所以宁可少认几个，不要乱认。
 */
static const char *path_arg_of(uint32_t nr, const uint64_t *a)
{
    switch (nr) {
    /* *at 家族：a0 是 dirfd，路径在 a1 */
    case LINUX_SYS_OPENAT:
    case LINUX_SYS_NEWFSTATAT:
    case LINUX_SYS_READLINKAT:
    case LINUX_SYS_FACCESSAT:
    case LINUX_SYS_FACCESSAT2:
    case LINUX_SYS_UNLINKAT:
    case LINUX_SYS_MKDIRAT:
    case LINUX_SYS_RENAMEAT:
    case LINUX_SYS_RENAMEAT2:
        return (const char *)a[1];

    /* 路径直接在 a0 */
    case LINUX_SYS_CHDIR:
    case LINUX_SYS_EXECVE:
        return (const char *)a[0];

    default:
        return NULL;
    }
}

/* ── 记录 ───────────────────────────────────────────────────────── */

void syscall_trace_enter(uint16_t pid, uint32_t nr, uint64_t a0, uint64_t a1,
                         uint64_t a2)
{
    uint32_t c = trace_cpu();
    uint32_t slot = s_head[c];
    uint64_t args[3] = { a0, a1, a2 };

    syscall_trace_rec_t *r = &s_ring[c][slot];

    r->seq = s_seq[c];
    r->a0 = a0;
    r->a1 = a1;
    r->a2 = a2;
    r->ret = (int64_t)SYSCALL_TRACE_PENDING;
    r->at_ms = (uint32_t)timer_get_uptime_ms();
    r->pid = pid;
    r->nr = (uint16_t)nr;
    r->flags = 0;
    r->path[0] = '\0';

    const char *upath = path_arg_of(nr, args);
    if (upath != NULL &&
        copy_string_from_user(upath, r->path, SYSCALL_TRACE_PATH_MAX) > 0) {
        r->path[SYSCALL_TRACE_PATH_MAX - 1] = '\0';
        r->flags |= SYSCALL_TRACE_F_PATH;
    }

    s_head[c] = (slot + 1U) % SYSCALL_TRACE_DEPTH;
    s_seq[c] = s_seq[c] + 1U;
    s_pending[c] = slot;
    s_pending_valid[c] = 1U;
}

void syscall_trace_exit(int64_t ret)
{
    uint32_t c = trace_cpu();

    if (!s_pending_valid[c])
        return;

    s_pending_valid[c] = 0U;
    s_ring[c][s_pending[c]].ret = ret;
    s_ring[c][s_pending[c]].flags |= SYSCALL_TRACE_F_RETURNED;
}

/* ── 渲染 ───────────────────────────────────────────────────────── */

/*
 * 归并各 CPU 的环，按时间序输出**最近的** max 条。
 *
 * 每个 CPU 的环本身按 seq 递增（环形覆盖，旧的被新的顶掉）。这里对每个 CPU
 * 维护一个游标，每轮从各 CPU 的"下一条"里挑 (at_ms, seq) 最小的输出 —— 也就是
 * 按时间序取最近 max 条。不需要临时数组，状态只有每 CPU 一个游标。
 *
 * at_ms 只有 10ms 分辨率，同毫秒内的跨 CPU 顺序由 seq 兜底；真正的全局顺序
 * 需要一个全局原子计数，那会在热路径上加一条争用的 cache line，不值得。
 */
typedef struct {
    uint32_t cur;  /* 当前可读槽位 */
    uint32_t left; /* 本 CPU 还剩多少条没输出 */
    uint64_t seq;
    uint32_t at_ms;
} merge_cur_t;

static int render_one(char *out, int len, const syscall_trace_rec_t *r)
{
    int n;
    const char *name = syscall_trace_name(r->nr);

    if (r->flags & SYSCALL_TRACE_F_PATH)
        n = trace_snprintf(
            out, len, "pid=%u t=%ums %s \"%s\" (0x%llx, 0x%llx, 0x%llx) = ",
            (unsigned)r->pid, (unsigned)r->at_ms, name, r->path,
            (unsigned long long)r->a0, (unsigned long long)r->a1,
            (unsigned long long)r->a2);
    else
        n = trace_snprintf(
            out, len,
            "pid=%u t=%ums %s (0x%llx, 0x%llx, 0x%llx) = ", (unsigned)r->pid,
            (unsigned)r->at_ms, name, (unsigned long long)r->a0,
            (unsigned long long)r->a1, (unsigned long long)r->a2);

    if (n < 0)
        n = 0;
    if (n > len - 1)
        return len - 1;

    int m;
    if (r->ret == (int64_t)SYSCALL_TRACE_PENDING)
        m = trace_snprintf(out + n, len - n, "? (没有返回)\n");
    else if (r->ret < 0)
        m = trace_snprintf(out + n, len - n, "0x%llx (%lld)\n",
                           (unsigned long long)r->ret, (long long)r->ret);
    else
        m = trace_snprintf(out + n, len - n, "0x%llx (%lld)\n",
                           (unsigned long long)r->ret, (long long)r->ret);

    if (m < 0)
        m = 0;
    if (n + m > len - 1)
        return len - 1;
    return n + m;
}

int syscall_trace_render(uint16_t pid_filter, uint32_t max, char *out, int len)
{
    merge_cur_t cur[AVATAR_MAX_CPUS];
    int pos = 0;

    if (out == NULL || len <= 0)
        return 0;

    out[0] = '\0';

    /* 初始化每个 CPU 的游标：指向它自己"最近 DEPTH 条"里**最旧**的那条 */
    for (uint32_t c = 0; c < AVATAR_MAX_CPUS; c++) {
        uint64_t cnt = s_seq[c];
        if (cnt > SYSCALL_TRACE_DEPTH)
            cnt = SYSCALL_TRACE_DEPTH;
        cur[c].left = (uint32_t)cnt;
        /* 没绕圈时从 0 开始；绕过后最旧的一条在 head 处 */
        cur[c].cur = (s_seq[c] > SYSCALL_TRACE_DEPTH) ? s_head[c] : 0U;
    }

    for (uint32_t emitted = 0; emitted < max; emitted++) {
        int pick = -1;
        uint32_t best_ms = 0;
        uint64_t best_seq = 0;

        for (uint32_t c = 0; c < AVATAR_MAX_CPUS; c++) {
            if (cur[c].left == 0U)
                continue;

            const syscall_trace_rec_t *r = &s_ring[c][cur[c].cur];

            if (pid_filter != 0U && r->pid != pid_filter) {
                /* 被过滤掉的也要推进游标，否则死循环 */
                cur[c].cur = (cur[c].cur + 1U) % SYSCALL_TRACE_DEPTH;
                cur[c].left--;
                c--; /* 重新看这个 CPU 的下一条 */
                continue;
            }

            if (pick < 0 || r->at_ms < best_ms ||
                (r->at_ms == best_ms && r->seq < best_seq)) {
                pick = (int)c;
                best_ms = r->at_ms;
                best_seq = r->seq;
            }
        }

        if (pick < 0)
            break;

        const syscall_trace_rec_t *r = &s_ring[pick][cur[pick].cur];

        int w = render_one(out + pos, len - pos, r);
        pos += w;
        if (pos >= len - 1)
            break;

        cur[pick].cur = (cur[pick].cur + 1U) % SYSCALL_TRACE_DEPTH;
        cur[pick].left--;
    }

    return pos;
}

void syscall_trace_dump(uint16_t pid_filter, uint32_t max)
{
    static char buf[4096];
    int n = syscall_trace_render(pid_filter, max, buf, (int)sizeof(buf));

    if (n <= 0)
        return;

    /* 报**实际**条数而不是 max —— 程序只跑了 3 个 syscall 时，
     * 说"最后 48 条"会让人以为中间丢了记录。 */
    uint32_t lines = 0;
    for (const char *q = buf; *q; q++)
        if (*q == '\n')
            lines++;

    if (pid_filter != 0U)
        KLOG_WARN("[SYSCALL] pid=%u 崩溃前的最近 %u 条系统调用：\n",
                  (unsigned)pid_filter, (unsigned)lines);
    else
        KLOG_WARN("[SYSCALL] 最近的 %u 条系统调用：\n", (unsigned)lines);

    /* buf 里已经是多行文本；逐行走 klog，让每行都带上级别前缀和 cpu/文件信息 */
    char *p = buf;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl != NULL)
            *nl = '\0';
        if (*p)
            KLOG_WARN("[SYSCALL] %s\n", p);
        if (nl == NULL)
            break;
        p = nl + 1;
    }
}
