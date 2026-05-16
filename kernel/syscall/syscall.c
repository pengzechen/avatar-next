#include "syscall/syscall.h"
#include "loader/bin_loader.h"
#include "loader/elf_loader.h"
#include "klog.h"
#include "task/task.h"
#include "user_layout.h"
#include "task/sched.h"
#include "task/switch.h"
#include "mm_vm.h"
#include "pmm.h"
#include "string.h"
#include "arch.h"
#include "exception.h"
#include "syscall_abi.h"
#include "uart/uart.h"
#include <ext4.h>
#include <ext4_errno.h>
#if ARCH_RISCV64
#include "riscv64/satp_utils.h"
#endif


/* ── Linux AArch64 标准系统调用号 ───────────────────────────────── */
#define LINUX_SYS_GETCWD         17
#define LINUX_SYS_DUP3           24
#define LINUX_SYS_FCNTL          25
#define LINUX_SYS_IOCTL          29
#define LINUX_SYS_UNLINKAT       35
#define LINUX_SYS_RENAMEAT       38
#define LINUX_SYS_FACCESSAT      48
#define LINUX_SYS_CHDIR          49
#define LINUX_SYS_OPENAT         56
#define LINUX_SYS_CLOSE          57
#define LINUX_SYS_PIPE2          59
#define LINUX_SYS_GETDENTS64     61
#define LINUX_SYS_LSEEK          62
#define LINUX_SYS_READ           63
#define LINUX_SYS_WRITE          64
#define LINUX_SYS_WRITEV         66
#define LINUX_SYS_READLINKAT     78
#define LINUX_SYS_NEWFSTATAT     79
#define LINUX_SYS_FSTAT          80
#define LINUX_SYS_FSYNC          82
#define LINUX_SYS_FDATASYNC      83
#define LINUX_SYS_EXIT           93
#define LINUX_SYS_EXIT_GROUP     94
#define LINUX_SYS_WAITID         95
#define LINUX_SYS_SET_TID_ADDR   96
#define LINUX_SYS_SET_ROBUST_LIST 99
#define LINUX_SYS_NANOSLEEP      101
#define LINUX_SYS_CLOCK_GETTIME  113
#define LINUX_SYS_SCHED_YIELD    124
#define LINUX_SYS_KILL           129
#define LINUX_SYS_TGKILL         131
#define LINUX_SYS_RT_SIGACTION   134
#define LINUX_SYS_RT_SIGPROCMASK 135
#define LINUX_SYS_RT_SIGRETURN   139
#define LINUX_SYS_SETGID         144
#define LINUX_SYS_SETUID         146
#define LINUX_SYS_SETPGID        154
#define LINUX_SYS_GETPGID        155
#define LINUX_SYS_GETSID         156
#define LINUX_SYS_SETSID         157
#define LINUX_SYS_GETGROUPS      158
#define LINUX_SYS_SETGROUPS      159
#define LINUX_SYS_UNAME          160
#define LINUX_SYS_GETRLIMIT      163
#define LINUX_SYS_SETRLIMIT      164
#define LINUX_SYS_GETRUSAGE      165
#define LINUX_SYS_UMASK          166
#define LINUX_SYS_PRCTL          167
#define LINUX_SYS_GETPID         172
#define LINUX_SYS_GETPPID        173
#define LINUX_SYS_GETUID         174
#define LINUX_SYS_GETEUID        175
#define LINUX_SYS_GETGID         176
#define LINUX_SYS_GETEGID        177
#define LINUX_SYS_GETTID         178
#define LINUX_SYS_SOCKET         198
#define LINUX_SYS_BRK            214
#define LINUX_SYS_MUNMAP         215
#define LINUX_SYS_CLONE          220
#define LINUX_SYS_EXECVE         221
#define LINUX_SYS_MMAP           222
#define LINUX_SYS_MPROTECT       226
#define LINUX_SYS_PSELECT6        72
#define LINUX_SYS_PPOLL           73
#define LINUX_SYS_WAIT4          260
#define LINUX_SYS_PRLIMIT64      261
#define LINUX_SYS_GETRANDOM      278

#define X86_SYS_ARCH_PRCTL       0x7FFFFFFDULL
#define X86_SYS_RSEQ             0x7FFFFFFCULL
#define X86_SYS_POLL             0x7FFFFFFBULL

#if ARCH_X86_64
#define X86_MSR_IA32_FS_BASE     0xC0000100U
#define X86_ARCH_SET_FS          0x1002UL
#define X86_ARCH_GET_FS          0x1003UL

static inline void x86_write_msr(uint32_t msr, uint64_t value)
{
    uint32_t lo = (uint32_t)(value & 0xFFFFFFFFU);
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

static inline void x86_write_fs_base(uint64_t fs_base)
{
    x86_write_msr(X86_MSR_IA32_FS_BASE, fs_base);
}
#endif

/* ioctl request codes */
#define TCGETS           0x5401
#define TCSETS           0x5402
#define TCSETSW          0x5403
#define TCSETSF          0x5404
#define TIOCGWINSZ       0x5413
#define TIOCSWINSZ       0x5414
#define TIOCGPGRP        0x540f
#define TIOCSPGRP        0x5410
#define TIOCGPTN         0x80045430
#define TIOCSPTLCK       0x40045431

struct kernel_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

/* Linux termios (TCGETS/TCSETS) minimal ABI view */
struct kernel_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
};

struct kernel_pollfd {
    int   fd;
    short events;
    short revents;
};

#define MAP_ANONYMOUS  0x20
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MMAP_FAILED    ((uint64_t)(int64_t)-1)

/* POSIX errno values */
#define ENOSYS   38
#define ESRCH     3
#define EBADF     9
#define EIO       5
#define EINVAL   22
#define ENOENT    2
#define ENOMEM   12
#define EFAULT   14
#define ERANGE   34
#define ECHILD   10
#define ENOTDIR  20
#define EISDIR   21
#define ENFILE   23
#define EMFILE   24
#define ENOTTY   25
#define ENOTSUP  95
#define AT_FDCWD -100
#define AT_REMOVEDIR 0x200

/* ─────────────────────────────────────────────────────────────────
 * 全局文件描述符对象池
 * fd_table[i] in task_t holds an index into this pool (-1 = not open).
 * FD 0/1/2 (stdin/stdout/stderr) are handled specially (UART).
 * ───────────────────────────────────────────────────────────────── */
#define FD_POOL_SIZE  48

typedef enum {
    FDT_FREE = 0,
    FDT_FILE,
    FDT_DIR,
} fd_type_t;

typedef struct {
    fd_type_t  type;
    int        flags;
    char       path[128];   /* for fstat / readlink */
    union {
        ext4_file file;
        ext4_dir  dir;
    };
} fd_obj_t;

static fd_obj_t g_fd_pool[FD_POOL_SIZE];

/* Alloc/free a pool slot */
static int fd_pool_alloc(void)
{
    for (int i = 0; i < FD_POOL_SIZE; i++) {
        if (g_fd_pool[i].type == FDT_FREE) {
            KLOG_DEBUG("[fd] pool_alloc: allocated slot %d\n", i);
            return i;
        }
    }
    KLOG_ERROR("[fd] pool_alloc: no free slots (FD_POOL_SIZE=%d)\n", FD_POOL_SIZE);
    return -1;
}

static void fd_pool_free(int idx)
{
    if (idx >= 0 && idx < FD_POOL_SIZE) {
        KLOG_DEBUG("[fd] pool_free: freeing slot %d\n", idx);
        g_fd_pool[idx].type = FDT_FREE;
    }
}

/* Allocate a new fd number for the current task, backed by pool slot idx */
static int task_alloc_fd(task_t *task, int pool_idx)
{
    /* fd 0,1,2 reserved for stdin/stdout/stderr */
    for (int fd = 3; fd < (int)TASK_MAX_FD; fd++) {
        /* 使用 (int8_t)-1 避免类型提升问题 */
        if (task->fd_table[fd] == (int8_t)-1) {
            task->fd_table[fd] = (int8_t)pool_idx;
            KLOG_DEBUG("[fd] task_alloc_fd: pid=%u allocated fd=%d for pool_idx=%d\n",
                      task->id, fd, pool_idx);
            return fd;
        }
    }

    /* 打印 fd_table 的前几个槽位用于调试 */
    KLOG_ERROR("[fd] task_alloc_fd: pid=%u no free fd (TASK_MAX_FD=%d)\n",
              task->id, TASK_MAX_FD);
    KLOG_ERROR("[fd] fd_table dump: [0]=%d [1]=%d [2]=%d [3]=%d [4]=%d [5]=%d\n",
              task->fd_table[0], task->fd_table[1], task->fd_table[2],
              task->fd_table[3], task->fd_table[4], task->fd_table[5]);

    return -1;
}

/* Get pool object for an fd (-1 on error) */
static fd_obj_t *task_get_fd(task_t *task, int fd)
{
    if (fd < 0 || fd >= (int)TASK_MAX_FD)
        return NULL;
    int idx = task->fd_table[fd];
    if (idx < 0 || idx >= FD_POOL_SIZE)
        return NULL;
    if (g_fd_pool[idx].type == FDT_FREE)
        return NULL;
    return &g_fd_pool[idx];
}

/* ─────────────────────────────────────────────────────────────────
 * 路径规范化辅助：将相对路径拼接到 cwd，返回绝对路径
 * 输出写入 out（最大 128 字节，含 NUL）。
 * ───────────────────────────────────────────────────────────────── */
static void resolve_path(const char *cwd, const char *path, char *out, int outlen)
{
    char tmp[128];

    if (path == NULL || path[0] == '\0') {
        /* 空路径 */
        out[0] = '/';
        out[1] = '\0';
        return;
    }

    if (path[0] == '/') {
        /* 绝对路径 */
        int i = 0;
        while (path[i] && i < (int)sizeof(tmp) - 1) {
            tmp[i] = path[i];
            i++;
        }
        tmp[i] = '\0';
    } else {
        /* 相对路径：cwd + "/" + path */
        int i = 0;
        while (cwd[i] && i < (int)sizeof(tmp) - 1) {
            tmp[i] = cwd[i];
            i++;
        }
        if (i > 0 && tmp[i - 1] != '/' && i < (int)sizeof(tmp) - 1)
            tmp[i++] = '/';
        int j = 0;
        while (path[j] && i < (int)sizeof(tmp) - 1) {
            tmp[i++] = path[j++];
        }
        tmp[i] = '\0';
    }

    /* 规范化绝对路径：处理 //、.、.. */
    {
        int seg_start[64];
        int seg_len[64];
        int seg_count = 0;
        int i = 0;

        while (tmp[i] != '\0') {
            while (tmp[i] == '/')
                i++;
            if (tmp[i] == '\0')
                break;

            int start = i;
            while (tmp[i] != '\0' && tmp[i] != '/')
                i++;
            int len = i - start;

            if (len == 1 && tmp[start] == '.') {
                continue;
            }
            if (len == 2 && tmp[start] == '.' && tmp[start + 1] == '.') {
                if (seg_count > 0)
                    seg_count--;
                continue;
            }

            if (seg_count < (int)(sizeof(seg_start) / sizeof(seg_start[0]))) {
                seg_start[seg_count] = start;
                seg_len[seg_count] = len;
                seg_count++;
            }
        }

        int pos = 0;
        if (outlen <= 0)
            return;

        out[pos++] = '/';
        for (int s = 0; s < seg_count && pos < outlen - 1; s++) {
            for (int k = 0; k < seg_len[s] && pos < outlen - 1; k++) {
                out[pos++] = tmp[seg_start[s] + k];
            }
            if (s != seg_count - 1 && pos < outlen - 1) {
                out[pos++] = '/';
            }
        }
        out[pos] = '\0';
    }
}

/*
 * 解析 *at 系统调用路径：
 * - 绝对路径：直接使用
 * - 相对路径：基于 dirfd 目录或当前 cwd
 */
static int resolve_path_at(task_t *task, int dirfd, const char *pathname,
                           char *abspath, int abspath_len)
{
    if (!pathname)
        return -ENOENT;

    if (pathname[0] == '/') {
        resolve_path(task->cwd, pathname, abspath, abspath_len);
        return 0;
    }

    if (dirfd == AT_FDCWD) {
        resolve_path(task->cwd, pathname, abspath, abspath_len);
        return 0;
    }

    fd_obj_t *base = task_get_fd(task, dirfd);
    if (!base || base->type != FDT_DIR)
        return -EBADF;

    resolve_path(base->path, pathname, abspath, abspath_len);
    return 0;
}

/* ─────────────────────────────────────────────────────────────────
 * struct stat / dirent definitions
 * ───────────────────────────────────────────────────────────────── */
#if ARCH_X86_64
/* Linux x86_64 ABI: sizeof(struct stat) = 144 */
struct kernel_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atime_sec;
    uint64_t st_atime_nsec;
    int64_t  st_mtime_sec;
    uint64_t st_mtime_nsec;
    int64_t  st_ctime_sec;
    uint64_t st_ctime_nsec;
    int64_t  __unused[3];
};
#else
/* Linux AArch64/RISC-V64 ABI */
struct kernel_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t _pad1;
    int64_t  st_size;
    int32_t  st_blksize;
    int32_t  _pad2;
    int64_t  st_blocks;
    int64_t  st_atime_sec;
    uint64_t st_atime_nsec;
    int64_t  st_mtime_sec;
    uint64_t st_mtime_nsec;
    int64_t  st_ctime_sec;
    uint64_t st_ctime_nsec;
    uint32_t _unused[2];
};
#endif

struct kernel_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[1]; /* variable length */
};

struct kernel_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct kernel_iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};

struct kernel_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct kernel_rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

/* ─────────────────────────────────────────────────────────────────
 * Helper: copy NULL-terminated string from user space into kernel buf
 * ───────────────────────────────────────────────────────────────── */
static int copy_string_from_user(const char *ustr, char *kbuf, int maxlen)
{
    if (!ustr) return -1;
    int i = 0;
    while (i < maxlen - 1) {
        kbuf[i] = ustr[i];
        if (ustr[i] == '\0') return i;
        i++;
    }
    kbuf[i] = '\0';
    return i;
}

/* ─────────────────────────────────────────────────────────────────
 * Helper: copy kernel string to user buf
 * ───────────────────────────────────────────────────────────────── */
static int copy_string_to_user(const char *kstr, char *ubuf, int maxlen)
{
    if (!ubuf) return -1;
    int i = 0;
    while (i < maxlen - 1 && kstr[i]) {
        ubuf[i] = kstr[i];
        i++;
    }
    ubuf[i] = '\0';
    return i;
}

/* ─────────────────────────────────────────────────────────────────
 * Helper: fill kernel_stat from ext4 inode info
 * ───────────────────────────────────────────────────────────────── */
static void fill_stat_from_ext4(struct kernel_stat *st, const char *path)
{
    memset(st, 0, sizeof(*st));
    st->st_dev     = 1;
    st->st_nlink   = 1;
    st->st_blksize = 4096;

    uint32_t mode = 0;
    ext4_mode_get(path, &mode);

    /* Try to get file size via ext4_fopen */
    ext4_file f;
    if (ext4_fopen2(&f, path, 0 /* O_RDONLY */) == EOK) {
        st->st_size   = (int64_t)ext4_fsize(&f);
        st->st_blocks = (st->st_size + 511) / 512;
        uint32_t ino  = 0;
        ext4_raw_inode_fill(path, &ino, NULL);
        st->st_ino  = ino;
        ext4_fclose(&f);
        /* regular file */
        st->st_mode = 0100755;  /* -rwxr-xr-x */
    } else {
        /* maybe directory */
        ext4_dir d;
        if (ext4_dir_open(&d, path) == EOK) {
            st->st_mode = 0040755;   /* drwxr-xr-x */
            ext4_dir_close(&d);
        } else {
            st->st_mode = 0100644;
        }
    }
    (void)mode;
}

/* ─────────────────────────────────────────────────────────────────
 * Unblock parent waiting for a specific child (or any child)
 * ───────────────────────────────────────────────────────────────── */
/* g_task_pool / g_stack_used defined as non-static in task.c */
extern task_t  g_task_pool[TASK_MAX];
extern uint8_t g_stack_used[TASK_MAX];

static void notify_parent_wait(task_t *child)
{
    for (uint32_t i = 0; i < TASK_MAX; i++) {
        if (!g_stack_used[i]) continue;
        task_t *t = &g_task_pool[i];
        if (!t->is_waiting) continue;
        if (t->id != child->parent_id) continue;
        if (t->wait_pid != (uint32_t)-1 && t->wait_pid != child->id) continue;
        t->is_waiting = false;
        task_unblock(t);
        return;
    }
}

/* Called from task.c's task_exit() to notify waiting parent */
void notify_parent_wait_from_task(task_t *child)
{
    notify_parent_wait(child);
}

/* Debug counter - check if we reach syscall_handler */
volatile uint32_t g_syscall_entry_count = 0;

#if ARCH_X86_64
/* ─────────────────────────────────────────────────────────────────
 * Linux x86_64 → 内部统一系统调用号翻译
 *
 * busybox-x86_64 使用 Linux x86_64 syscall 号，而下面的 switch 分支
 * 使用 AArch64/RISC-V 的通用 Linux syscall 号（两者已在 AARCH64/RISC-V
 * 上共享）。这个翻译层把 x86_64 号映射到对应分支，同时对参数布局不同
 * 的 syscall（open/stat/access/readlink 等）原地调整 regs[]。
 * ───────────────────────────────────────────────────────────────── */
static void x86_translate_syscall(uint64_t *nr, uint64_t regs[9])
{
    switch (*nr) {
    /* ── 基础 I/O ── */
    case 0:   *nr = LINUX_SYS_READ;  break;   /* read */
    case 1:   *nr = LINUX_SYS_WRITE; break;   /* write */
    case 2:   /* open(path,flags,mode) → openat(AT_FDCWD,path,flags,mode) */
        regs[3] = regs[2]; regs[2] = regs[1]; regs[1] = regs[0];
        regs[0] = (uint64_t)(int64_t)AT_FDCWD;
        *nr = LINUX_SYS_OPENAT; break;
    case 3:   *nr = LINUX_SYS_CLOSE; break;   /* close */
    case 4:   /* stat(path,buf) → newfstatat(AT_FDCWD,path,buf,0) */
        regs[3] = 0; regs[2] = regs[1]; regs[1] = regs[0];
        regs[0] = (uint64_t)(int64_t)AT_FDCWD;
        *nr = LINUX_SYS_NEWFSTATAT; break;
    case 5:   *nr = LINUX_SYS_FSTAT; break;   /* fstat */
    case 6:   /* lstat(path,buf) → newfstatat(AT_FDCWD,path,buf,0) */
        regs[3] = 0; regs[2] = regs[1]; regs[1] = regs[0];
        regs[0] = (uint64_t)(int64_t)AT_FDCWD;
        *nr = LINUX_SYS_NEWFSTATAT; break;
    case 7:   *nr = X86_SYS_POLL;          break; /* poll */
    case 8:   *nr = LINUX_SYS_LSEEK;       break; /* lseek */
    case 9:   *nr = LINUX_SYS_MMAP;        break; /* mmap */
    case 10:  *nr = LINUX_SYS_MPROTECT;    break; /* mprotect */
    case 11:  *nr = LINUX_SYS_MUNMAP;      break; /* munmap */
    case 12:  *nr = LINUX_SYS_BRK;         break; /* brk */
    case 13:  *nr = LINUX_SYS_RT_SIGACTION;  break; /* rt_sigaction */
    case 14:  *nr = LINUX_SYS_RT_SIGPROCMASK; break; /* rt_sigprocmask */
    case 15:  *nr = LINUX_SYS_RT_SIGRETURN;  break; /* rt_sigreturn */
    case 16:  *nr = LINUX_SYS_IOCTL;       break; /* ioctl */
    case 17:  *nr = LINUX_SYS_READ;        break; /* pread64 → read(stub) */
    case 20:  *nr = LINUX_SYS_WRITEV;      break; /* writev */
    case 21:  /* access(path,mode) → faccessat(AT_FDCWD,path,mode,0) */
        regs[3] = 0; regs[2] = regs[1]; regs[1] = regs[0];
        regs[0] = (uint64_t)(int64_t)AT_FDCWD;
        *nr = LINUX_SYS_FACCESSAT; break;
    case 22:  *nr = LINUX_SYS_PIPE2;       break; /* pipe → pipe2(flags=0) */
    case 24:  *nr = LINUX_SYS_SCHED_YIELD; break; /* sched_yield */
    case 32:  /* dup(oldfd): x86_64. 不直接支持，返回 ENOSYS */
        /* busybox sh 很少用裸 dup()，ENOSYS 不影响启动 */
        break; /* *nr stays 32, hits default: → ENOSYS */
    case 33:  /* dup2(oldfd,newfd) → dup3(oldfd,newfd,0) */
        regs[2] = 0;
        *nr = LINUX_SYS_DUP3; break;
    case 35:  *nr = LINUX_SYS_NANOSLEEP;   break; /* nanosleep */
    case 39:  *nr = LINUX_SYS_GETPID;      break; /* getpid */
    case 41:  *nr = LINUX_SYS_SOCKET;      break; /* socket */
    case 56:  *nr = LINUX_SYS_CLONE;       break; /* clone */
    case 57:  *nr = LINUX_SYS_CLONE;       break; /* fork → clone */
    case 58:  *nr = LINUX_SYS_CLONE;       break; /* vfork → clone */
    case 59:  *nr = LINUX_SYS_EXECVE;      break; /* execve */
    case 60:  *nr = LINUX_SYS_EXIT;        break; /* exit */
    case 61:  *nr = LINUX_SYS_WAIT4;       break; /* wait4 */
    case 62:  *nr = LINUX_SYS_KILL;        break; /* kill */
    case 63:  *nr = LINUX_SYS_UNAME;       break; /* uname */
    case 72:  *nr = LINUX_SYS_FCNTL;       break; /* fcntl */
    case 74:  *nr = LINUX_SYS_FSYNC;       break; /* fsync */
    case 75:  *nr = LINUX_SYS_FDATASYNC;   break; /* fdatasync */
    case 79:  *nr = LINUX_SYS_GETCWD;      break; /* getcwd */
    case 80:  *nr = LINUX_SYS_CHDIR;       break; /* chdir */
    case 82:  *nr = 0x7FFFFFFEULL; break;      /* fchmod → stub 0 */
    case 83:  *nr = 0x7FFFFFFEULL; break;      /* fchown → stub 0 */
    case 89:  /* readlink(path,buf,bufsiz) → readlinkat(AT_FDCWD,path,buf,bufsiz) */
        regs[3] = regs[2]; regs[2] = regs[1]; regs[1] = regs[0];
        regs[0] = (uint64_t)(int64_t)AT_FDCWD;
        *nr = LINUX_SYS_READLINKAT; break;
    case 95:  *nr = LINUX_SYS_UMASK;       break; /* umask */
    case 97:  *nr = LINUX_SYS_GETRLIMIT;   break; /* getrlimit */
    case 98:  *nr = LINUX_SYS_GETRUSAGE;   break; /* getrusage */
    case 99:  *nr = 0x7FFFFFFEULL; break;      /* sysinfo → stub 0 */
    case 100: *nr = 0x7FFFFFFEULL; break;      /* times → stub 0 */
    case 102: *nr = LINUX_SYS_GETUID;      break; /* getuid */
    case 104: *nr = LINUX_SYS_GETGID;      break; /* getgid */
    case 105: *nr = LINUX_SYS_SETUID;      break; /* setuid */
    case 106: *nr = LINUX_SYS_SETGID;      break; /* setgid */
    case 107: *nr = LINUX_SYS_GETEUID;     break; /* geteuid */
    case 108: *nr = LINUX_SYS_GETEGID;     break; /* getegid */
    case 109: *nr = LINUX_SYS_SETPGID;     break; /* setpgid */
    case 110: *nr = LINUX_SYS_GETPPID;     break; /* getppid */
    case 111: /* getpgrp() → getpgid(0) */
        regs[0] = 0; *nr = LINUX_SYS_GETPGID; break;
    case 112: *nr = LINUX_SYS_SETSID;      break; /* setsid */
    case 115: *nr = LINUX_SYS_GETGROUPS;   break; /* getgroups */
    case 116: *nr = LINUX_SYS_SETGROUPS;   break; /* setgroups */
    case 121: *nr = LINUX_SYS_GETPGID;     break; /* getpgid */
    case 124: *nr = LINUX_SYS_GETSID;      break; /* getsid */
    case 131: *nr = 0x7FFFFFFEULL;         break; /* sigaltstack → stub 0 */
    case 157: *nr = LINUX_SYS_PRCTL;       break; /* prctl */
    case 158: *nr = X86_SYS_ARCH_PRCTL;    break; /* arch_prctl */
    case 160: *nr = LINUX_SYS_SETRLIMIT;   break; /* setrlimit */
    case 165: *nr = LINUX_SYS_GETRUSAGE;   break; /* getrusage(again) */
    case 186: *nr = LINUX_SYS_GETTID;      break; /* gettid */
    case 217: *nr = LINUX_SYS_GETDENTS64;  break; /* getdents64 */
    case 218: *nr = LINUX_SYS_SET_TID_ADDR; break; /* set_tid_address */
    case 228: *nr = LINUX_SYS_CLOCK_GETTIME; break; /* clock_gettime */
    case 231: *nr = LINUX_SYS_EXIT_GROUP;  break; /* exit_group */
    case 234: *nr = LINUX_SYS_TGKILL;      break; /* tgkill */
    case 247: *nr = LINUX_SYS_WAITID;      break; /* waitid */
    case 257: *nr = LINUX_SYS_OPENAT;      break; /* openat */
    case 258: *nr = 0x7FFFFFFEULL; break;      /* mkdirat → stub 0 */
    case 262: *nr = LINUX_SYS_NEWFSTATAT;  break; /* newfstatat */
    case 263: *nr = LINUX_SYS_UNLINKAT;    break; /* unlinkat */
    case 264: *nr = LINUX_SYS_RENAMEAT;    break; /* renameat */
    case 267: *nr = LINUX_SYS_READLINKAT;  break; /* readlinkat */
    case 268: *nr = 0x7FFFFFFEULL; break;      /* fchmodat → stub 0 */
    case 269: *nr = LINUX_SYS_FACCESSAT;   break; /* faccessat */
    case 270: *nr = LINUX_SYS_PSELECT6;    break; /* pselect6 */
    case 271: *nr = LINUX_SYS_PPOLL;       break; /* ppoll */
    case 273: *nr = LINUX_SYS_SET_ROBUST_LIST; break; /* set_robust_list */
    case 292: *nr = LINUX_SYS_DUP3;        break; /* dup3 */
    case 293: *nr = LINUX_SYS_PIPE2;       break; /* pipe2 */
    case 302: *nr = LINUX_SYS_PRLIMIT64;   break; /* prlimit64 */
    case 318: *nr = LINUX_SYS_GETRANDOM;   break; /* getrandom */
    case 334: *nr = X86_SYS_RSEQ;          break; /* rseq */
    /* 其他：保持原号（会落到 default: 返回 ENOSYS） */
    default: break;
    }
}
#endif /* ARCH_X86_64 */

/* ─────────────────────────────────────────────────────────────────
 * Main syscall dispatcher
 * ───────────────────────────────────────────────────────────────── */
void syscall_handler(trap_frame_t *frame)
{
    g_syscall_entry_count++;  /* Increment counter */
    uint64_t syscall_num = syscall_abi_nr(frame);
    uint64_t raw_syscall_num = syscall_num;
    KLOG_DEBUG("[syscall] number: %d, entry #%u: frame=%p\n", syscall_num, g_syscall_entry_count, frame);

    /* 调试：打印 trap_frame 原始内容 */
    // if (g_syscall_entry_count <= 3) {
    //     KLOG_ERROR("[syscall_handler] frame=%p\n", frame);
    //     KLOG_ERROR("  x10(a0)=0x%llx x11(a1)=0x%llx x12(a2)=0x%llx\n",
    //                frame->x[10], frame->x[11], frame->x[12]);
    //     KLOG_ERROR("  x17(a7)=0x%llx sepc=0x%llx\n", frame->x[17], frame->sepc);
    // }

    uint64_t regs[9] = {0};
    for (int i = 0; i < 6; i++) {
        regs[i] = syscall_abi_arg(frame, i);
    }
    // uint64_t syscall_num = syscall_abi_nr(frame);

#if ARCH_X86_64
    /* 将 Linux x86_64 syscall 号翻译为下方 switch 使用的内部编号 */
    x86_translate_syscall(&syscall_num, regs);
    /* 0x7FFFFFFEULL 是 stub 标记：直接返回 0 */
    if (syscall_num == 0x7FFFFFFEULL) {
        if (g_syscall_entry_count <= 16) {
            KLOG_DEBUG("[syscall:x86] raw=%llu -> stub0 args=[0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx]\n",
                       raw_syscall_num, regs[0], regs[1], regs[2], regs[3], regs[4], regs[5]);
        }
        syscall_abi_set_ret(frame, 0);
        return;
    }
#endif

    if (g_syscall_entry_count <= 16) {
        KLOG_DEBUG("[syscall] dispatch raw=%llu mapped=%llu args=[0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx]\n",
                   raw_syscall_num, syscall_num,
                   regs[0], regs[1], regs[2], regs[3], regs[4], regs[5]);
    }

    /* 调试：记录系统调用号 */
    if (g_syscall_entry_count <= 10) {
        // KLOG_ERROR("[syscall] #%u: nr=%llu a0=0x%llx a1=0x%llx a2=0x%llx\n",
        //            g_syscall_entry_count, syscall_num, regs[0], regs[1], regs[2]);
    }

    task_t *current = task_current();

    switch (syscall_num) {

    /* ── 自定义系统调用（向后兼容）────────────────────────── */
    case SYS_EXIT:
        sys_exit((int)regs[0]);
        break;
    case SYS_YIELD:
        regs[0] = sys_yield();
        break;
    case SYS_GETPID:
        regs[0] = sys_getpid();
        break;
    case SYS_SLEEP:
        regs[0] = sys_sleep(regs[0]);
        break;
    case SYS_EXECVE: {
        const char *pathname = (const char *)regs[0];
        char **argv = (char **)regs[1];
        char **envp = (char **)regs[2];
        regs[0] = sys_execve(pathname, argv, envp);
        break;
    }
    case SYS_BRK: {
        void *addr = (void *)regs[0];
        regs[0] = (uint64_t)sys_brk(addr);
        break;
    }
    case SYS_SBRK: {
        int64_t inc = (int64_t)regs[0];
        regs[0] = (uint64_t)sys_sbrk(inc);
        break;
    }
    case SYS_WRITE: {
        regs[0] = sys_write((const char *)regs[0], regs[1]);
        break;
    }
    case SYS_READ: {
        regs[0] = sys_read((char *)regs[0], regs[1]);
        break;
    }
    case SYS_OPEN: {
        regs[0] = sys_open((const char *)regs[0], (int)regs[1], (int)regs[2]);
        break;
    }
    case SYS_CLOSE: {
        regs[0] = sys_close((int)regs[0]);
        break;
    }
    case SYS_GETTIMEOFDAY: {
        regs[0] = sys_gettimeofday((struct timeval *)regs[0], (void *)regs[1]);
        break;
    }

    /* ── Linux AArch64 标准系统调用 ─────────────────────────────── */

    /* --- 进程管理 --- */
    case LINUX_SYS_EXIT:
    case LINUX_SYS_EXIT_GROUP:
        sys_exit((int)regs[0]);
        break;

    case LINUX_SYS_CLONE: {
        /* clone(flags, stack, parent_tid, tls, child_tid) */
        uint64_t flags = regs[0];
        /* uint64_t child_stack = regs[1]; -- mostly 0 for fork */
        (void)flags;

        /* Allocate child task slot */
        /* We implement vfork-like clone: child shares page table,
           parent blocks until child exits or execs */
        task_t *parent = task_current();

        /* Allocate a new task */
        extern uint8_t g_task_stacks[TASK_MAX][TASK_STACK_SIZE];
        task_t *child = NULL;

        /* Inline alloc from pool */
        {
            /* cleanup dead first */
            for (uint32_t i = 0; i < TASK_MAX; i++) {
                if (g_stack_used[i] && g_task_pool[i].state == TASK_DEAD) {
                    g_stack_used[i] = 0;
                    g_task_pool[i].stack_base = NULL;
                    break;
                }
            }
            for (uint32_t i = 0; i < TASK_MAX; i++) {
                if (!g_stack_used[i]) {
                    g_stack_used[i] = 1;
                    g_task_pool[i].stack_base = g_task_stacks[i];
                    child = &g_task_pool[i];
                    break;
                }
            }
        }

        if (!child) {
            KLOG_ERROR("[clone] no free task slots\n");
            regs[0] = (uint64_t)(int64_t)-ENOMEM;
            break;
        }

        extern uint32_t g_task_id_cnt;
        child->id              = g_task_id_cnt++;
        child->state           = TASK_READY;
        child->priority        = parent->priority;
        child->is_user_process = true;

        /*
         * fork 语义：为子进程创建独立地址空间并复制父进程用户页。
         * 只复制用户地址范围，避免共享用户栈导致返回地址被覆盖。
         */
        uint64_t child_pgd_phys = pmm_alloc_pages(g_pmm, 1);
        if (child_pgd_phys == 0) {
            KLOG_ERROR("[clone] no memory for child pgd\n");
            g_stack_used[child - g_task_pool] = 0;
            regs[0] = (uint64_t)(int64_t)-ENOMEM;
            break;
        }
        void *child_pgd_virt = phys_to_virt(child_pgd_phys);
        void *parent_pgd_virt = phys_to_virt((uint64_t)parent->pgd);
        memset(child_pgd_virt, 0, PAGE_SIZE);

#if ARCH_X86_64
        /*
         * x86_64: 子进程 PML4 必须包含内核高半区映射（PML4[256..511]），
         * 否则调度器 write_cr3(child_pgd) 后内核代码立即不可达。
         * 从父进程页表复制（父进程已在 elf_loader 中正确继承）。
         */
        x86_copy_kernel_mappings((uint64_t *)child_pgd_virt,
                                 (uint64_t *)parent_pgd_virt);
#endif

#if ARCH_RISCV64
        /*
         * RISC-V: 子进程页表必须包含内核高半区映射，否则
         * sched_schedule 切换 satp 到子进程页表后，内核代码
         * 立即无法访问，导致指令页错误 → 系统挂起。
         * 从父进程页表复制 L1[0x100] 和 L1[0x102]（内核高半区 1GB 大页叶子）。
         */
        {
            uint64_t *child_l1  = (uint64_t *)child_pgd_virt;
            uint64_t *kernel_l1 = (uint64_t *)phys_to_virt(satp_read_pgd_phys());
            riscv64_copy_kernel_mappings(child_l1, kernel_l1);
        }
#endif

        bool clone_copy_ok = true;

        /* helper: 复制 [start, end) 内已映射页 */
        #define CLONE_COPY_RANGE(start, end)                                              \
            do {                                                                           \
                uint64_t __s = ALIGN_DOWN((start), PAGE_SIZE);                            \
                uint64_t __e = ALIGN_UP((end), PAGE_SIZE);                                \
                for (uint64_t va = __s; clone_copy_ok && va < __e; va += PAGE_SIZE) {     \
                    uint64_t src_pa = mm_vm_get_paddr(parent_pgd_virt, va);               \
                    if (src_pa == 0)                                                       \
                        continue;                                                          \
                    uint64_t dst_pa = pmm_alloc_pages(g_pmm, 1);                          \
                    if (dst_pa == 0) {                                                     \
                        clone_copy_ok = false;                                             \
                        break;                                                             \
                    }                                                                      \
                    memcpy(phys_to_virt(dst_pa), phys_to_virt(src_pa), PAGE_SIZE);        \
                    if (mm_vm_map_pages(child_pgd_virt, va, dst_pa, 1, 0) != 0) {         \
                        pmm_free_pages(g_pmm, dst_pa, 1);                                  \
                        clone_copy_ok = false;                                             \
                        break;                                                             \
                    }                                                                      \
                }                                                                          \
            } while (0)

        /* 代码/数据/堆 */
        CLONE_COPY_RANGE(0x0, parent->heap_end);

        /* mmap 区域（从 USER_MMAP_BASE_EXEC 线性向上分配） */
        if (clone_copy_ok && parent->mmap_next > USER_MMAP_BASE_EXEC)
            CLONE_COPY_RANGE(USER_MMAP_BASE_EXEC, parent->mmap_next);

        /* 用户栈 */
        if (clone_copy_ok)
            CLONE_COPY_RANGE(parent->user_stack_top - parent->user_stack_size,
                             parent->user_stack_top);

        #undef CLONE_COPY_RANGE

        if (!clone_copy_ok) {
            KLOG_ERROR("[clone] failed to copy user address space\n");
            g_stack_used[child - g_task_pool] = 0;
            pmm_free_pages(g_pmm, child_pgd_phys, 1);
            regs[0] = (uint64_t)(int64_t)-ENOMEM;
            break;
        }

        child->pgd             = (uint64_t *)child_pgd_phys;

        child->user_entry      = parent->user_entry;
        child->user_sp         = parent->user_sp;
        child->user_stack_top  = parent->user_stack_top;
        child->user_stack_size = parent->user_stack_size;
        child->heap_end        = parent->heap_end;
        child->mmap_next       = parent->mmap_next;
        child->fs_base         = parent->fs_base;
        child->parent_id       = parent->id;
        child->exit_status     = 0;
        child->is_waiting      = false;
        child->wait_pid        = (uint32_t)-1;

        /* Copy cwd */
        {
            int k = 0;
            while (parent->cwd[k] && k < (int)TASK_CWD_LEN - 1) {
                child->cwd[k] = parent->cwd[k];
                k++;
            }
            child->cwd[k] = '\0';
        }

        /* Copy fd_table */
        for (uint32_t k = 0; k < TASK_MAX_FD; k++)
            child->fd_table[k] = parent->fd_table[k];

        /* Task name */
        {
            int k = 0;
            while (parent->name[k] && k < (int)TASK_NAME_LEN - 1) {
                child->name[k] = parent->name[k];
                k++;
            }
            child->name[k] = '\0';
        }

        list_node_init(&child->run_node);
        list_node_init(&child->wait_node);

        /* Set up child kernel stack to return 0 to user */
        child->sp = arch_init_fork_child_stack(
            child->stack_base, TASK_STACK_SIZE,
            frame   /* full trap_frame: all user regs */
        );

        /* Enqueue child */
        sched_enqueue(child);

        KLOG_INFO("[clone] parent=%u child=%u elr=0x%llx usp=0x%llx\n",
                  parent->id, child->id, syscall_abi_ip(frame), syscall_abi_user_sp(frame));

        /* Parent returns child PID */
        regs[0] = (uint64_t)child->id;
        break;
    }

    case LINUX_SYS_EXECVE: {
        const char *pathname = (const char *)regs[0];
        char **argv = (char **)regs[1];
        char **envp = (char **)regs[2];
        regs[0] = sys_execve(pathname, argv, envp);
        break;
    }

    case LINUX_SYS_WAIT4:
    case LINUX_SYS_WAITID: {
        /* wait4(pid, wstatus, options, rusage) */
        int wait_pid  = (int)(int32_t)regs[0];
        int *wstatus  = (int *)regs[1];
        int options   = (int)regs[2];

        /* Linux: WNOHANG = 1 */
        const int WNOHANG = 1;

        task_t *me = task_current();

        /* 先判断是否存在匹配的子进程（无则立即 ECHILD） */
        bool has_matching_child = false;
        for (uint32_t i = 0; i < TASK_MAX; i++) {
            if (!g_stack_used[i]) continue;
            task_t *t = &g_task_pool[i];
            if (t->parent_id != me->id) continue;
            if (wait_pid > 0 && (int)t->id != wait_pid) continue;
            has_matching_child = true;
            break;
        }

        if (!has_matching_child) {
            regs[0] = (uint64_t)(int64_t)-ECHILD;
            break;
        }

        /* Look for an already-dead child */
        task_t *found = NULL;
        {
            for (uint32_t i = 0; i < TASK_MAX; i++) {
                if (!g_stack_used[i]) continue;
                task_t *t = &g_task_pool[i];
                if (t->parent_id != me->id) continue;
                if (wait_pid > 0 && (int)t->id != wait_pid) continue;
                if (t->state == TASK_DEAD) {
                    found = t;
                    break;
                }
            }
        }

        if (found) {
            if (wstatus)
                *wstatus = (found->exit_status & 0xFF) << 8;
            regs[0] = (uint64_t)found->id;
            /* Mark slot truly free */
            for (uint32_t i = 0; i < TASK_MAX; i++) {
                if (&g_task_pool[i] == found) {
                    g_stack_used[i]         = 0;
                    g_task_pool[i].stack_base = NULL;
                    break;
                }
            }
        } else {
            /* No dead child yet */
            if (options & WNOHANG) {
                regs[0] = 0;
                break;
            }

            /* 阻塞等待目标子进程退出 */
            me->is_waiting = true;
            me->wait_pid   = (wait_pid > 0) ? (uint32_t)wait_pid : (uint32_t)-1;
            task_block(NULL);

            /* When we wake up, a child has died */
            for (uint32_t i = 0; i < TASK_MAX; i++) {
                if (!g_stack_used[i]) continue;
                task_t *t = &g_task_pool[i];
                if (t->parent_id != me->id) continue;
                if (wait_pid > 0 && (int)t->id != wait_pid) continue;
                if (t->state == TASK_DEAD) {
                    found = t;
                    break;
                }
            }
            if (found) {
                if (wstatus)
                    *wstatus = (found->exit_status & 0xFF) << 8;
                regs[0] = (uint64_t)found->id;
                for (uint32_t i = 0; i < TASK_MAX; i++) {
                    if (&g_task_pool[i] == found) {
                        g_stack_used[i] = 0;
                        g_task_pool[i].stack_base = NULL;
                        break;
                    }
                }
            } else {
                regs[0] = (uint64_t)(int64_t)-ECHILD;
            }
        }
        break;
    }

    case LINUX_SYS_SET_TID_ADDR:
    case LINUX_SYS_GETTID:
        regs[0] = (uint64_t)task_current()->id;
        break;

    case LINUX_SYS_GETPID:
        regs[0] = (uint64_t)task_current()->id;
        break;

    case LINUX_SYS_GETPPID:
        regs[0] = (uint64_t)task_current()->parent_id;
        break;

    case LINUX_SYS_GETUID:
    case LINUX_SYS_GETEUID:
    case LINUX_SYS_GETGID:
    case LINUX_SYS_GETEGID:
        regs[0] = 0; /* root */
        break;

    case LINUX_SYS_SETUID:
    case LINUX_SYS_SETGID:
        regs[0] = 0;
        break;

    case LINUX_SYS_SETPGID:
        regs[0] = 0; /* stub: success */
        break;

    case LINUX_SYS_GETPGID:
    case LINUX_SYS_GETSID:
    case LINUX_SYS_SETSID:
        regs[0] = (uint64_t)task_current()->id;
        break;

    case LINUX_SYS_GETGROUPS:
        regs[0] = 0;
        break;

    case LINUX_SYS_SETGROUPS:
        regs[0] = 0;
        break;

    case X86_SYS_ARCH_PRCTL: {
#if ARCH_X86_64
        uint64_t code = regs[0];
        uint64_t addr = regs[1];

        if (code == X86_ARCH_SET_FS) {
            current->fs_base = addr;
            x86_write_fs_base(addr);
            regs[0] = 0;
        } else if (code == X86_ARCH_GET_FS) {
            if (addr == 0) {
                regs[0] = (uint64_t)(int64_t)-EFAULT;
            } else {
                *(uint64_t *)addr = current->fs_base;
                regs[0] = 0;
            }
        } else {
            regs[0] = (uint64_t)(int64_t)-EINVAL;
        }
#else
        regs[0] = (uint64_t)(int64_t)-ENOSYS;
#endif
        break;
    }

    case X86_SYS_RSEQ:
        regs[0] = (uint64_t)(int64_t)-ENOSYS;
        break;

    case LINUX_SYS_KILL: {
        int pid = (int)(int32_t)regs[0];
        int sig = (int)regs[1];
        task_t *me = task_current();

        /* 最小实现：仅支持向当前进程发送信号 */
        if (pid > 0 && (uint32_t)pid != me->id) {
            regs[0] = (uint64_t)(int64_t)-ESRCH;
            break;
        }

        if (sig == 0) {
            regs[0] = 0; /* existence check */
            break;
        }

        /* 终止当前进程：与 Linux 习惯一致，返回 128+signal */
        sys_exit(128 + sig);
        break;
    }

    case LINUX_SYS_TGKILL: {
        int tgid = (int)(int32_t)regs[0];
        int tid  = (int)(int32_t)regs[1];
        int sig  = (int)regs[2];
        task_t *me = task_current();

        /* 最小实现：仅支持 tgkill(self_tgid, self_tid, sig) */
        if ((uint32_t)tid != me->id ||
            (tgid > 0 && (uint32_t)tgid != me->id)) {
            regs[0] = (uint64_t)(int64_t)-ESRCH;
            break;
        }

        if (sig == 0) {
            regs[0] = 0;
            break;
        }

        sys_exit(128 + sig);
        break;
    }

    case LINUX_SYS_SCHED_YIELD:
        task_yield();
        regs[0] = 0;
        break;

    /* --- 文件描述符 --- */
    case LINUX_SYS_READ: {
        int      fd    = (int)regs[0];
        char    *buf   = (char *)regs[1];
        uint64_t count = regs[2];
        if (!buf || count == 0) { regs[0] = 0; break; }

        if (fd == 0) {
            /* stdin: yield-poll 读 UART，避免 spin 独占 CPU 阻塞子进程
             * 每次循环先用非阻塞 uart_rx_ready() 检查；无数据则 task_yield()
             * 让调度器切换到其他就绪任务（如子进程），再回来轮询。 */
            uint64_t n = 0;
            while (n < count) {
                while (!uart_rx_ready())
                    task_yield();
                buf[n] = uart_getc();
                if (buf[n] == '\r') buf[n] = '\n';
                if (buf[n++] == '\n') break;
            }
            regs[0] = n;
        } else {
            fd_obj_t *obj = task_get_fd(task_current(), fd);
            if (!obj || obj->type != FDT_FILE) {
                regs[0] = (uint64_t)(int64_t)-EBADF;
                break;
            }
            size_t rcnt = 0;
            int rc = ext4_fread(&obj->file, buf, (size_t)count, &rcnt);
            regs[0] = (rc == EOK) ? (uint64_t)rcnt : (uint64_t)(int64_t)-EIO;
        }
        break;
    }

    case LINUX_SYS_WRITE: {
        int          fd    = (int)regs[0];
        const char  *buf   = (const char *)regs[1];
        uint64_t     count = regs[2];
        if (!buf) { regs[0] = 0; break; }
        if (fd == 1 || fd == 2) {
            regs[0] = sys_write(buf, count);
        } else {
            fd_obj_t *obj = task_get_fd(task_current(), fd);
            if (!obj || obj->type != FDT_FILE) {
                regs[0] = (uint64_t)(int64_t)-EBADF;
                break;
            }
            size_t wcnt = 0;
            int rc = ext4_fwrite(&obj->file, buf, (size_t)count, &wcnt);
            regs[0] = (rc == EOK) ? (uint64_t)wcnt : (uint64_t)(int64_t)-EIO;
        }
        break;
    }

    case LINUX_SYS_WRITEV: {
        int fd = (int)regs[0];
        struct kernel_iovec *iov = (struct kernel_iovec *)regs[1];
        int iovcnt = (int)regs[2];
        if (!iov || iovcnt <= 0) { regs[0] = 0; break; }
        uint64_t total = 0;
        for (int i = 0; i < iovcnt; i++) {
            if (!iov[i].iov_base || iov[i].iov_len == 0) continue;
            if (fd == 1 || fd == 2 || fd == 0) {
                sys_write((const char *)iov[i].iov_base, iov[i].iov_len);
            }
            total += iov[i].iov_len;
        }
        regs[0] = total;
        break;
    }

    case LINUX_SYS_LSEEK: {
        int     fd     = (int)regs[0];
        int64_t offset = (int64_t)regs[1];
        int     whence = (int)regs[2];
        fd_obj_t *obj = task_get_fd(task_current(), fd);
        if (!obj || obj->type != FDT_FILE) {
            regs[0] = (uint64_t)(int64_t)-EBADF;
            break;
        }
        int rc = ext4_fseek(&obj->file, offset, (uint32_t)whence);
        if (rc == EOK)
            regs[0] = (uint64_t)ext4_ftell(&obj->file);
        else
            regs[0] = (uint64_t)(int64_t)-EINVAL;
        break;
    }

    case LINUX_SYS_FACCESSAT: {
        /* faccessat(dirfd, pathname, mode, flags) */
        int dirfd = (int)regs[0];
        const char *pathname = (const char *)regs[1];
        (void)regs[2]; /* mode: 当前无权限模型，存在即允许 */
        (void)regs[3]; /* flags */

        if (!pathname) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            break;
        }

        task_t *me = task_current();
        char abspath[128];
        int rpa = resolve_path_at(me, dirfd, pathname, abspath, sizeof(abspath));
        if (rpa < 0) {
            regs[0] = (uint64_t)(int64_t)rpa;
            break;
        }

        /* 使用 lwext4 原生存在性检查，避免路径类型误判 */
        int rc = ext4_inode_exist(abspath, EXT4_DE_UNKNOWN);
        regs[0] = (rc == EOK) ? 0 : (uint64_t)(int64_t)-ENOENT;
        break;
    }

    case LINUX_SYS_RENAMEAT: {
        /* renameat(olddirfd, oldpath, newdirfd, newpath) */
        int olddirfd = (int)regs[0];
        const char *oldpath = (const char *)regs[1];
        int newdirfd = (int)regs[2];
        const char *newpath = (const char *)regs[3];

        if (!oldpath || !newpath) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            break;
        }

        task_t *me = task_current();
        char oldabs[128];
        char newabs[128];

        int ro = resolve_path_at(me, olddirfd, oldpath, oldabs, sizeof(oldabs));
        if (ro < 0) {
            regs[0] = (uint64_t)(int64_t)ro;
            break;
        }

        int rn = resolve_path_at(me, newdirfd, newpath, newabs, sizeof(newabs));
        if (rn < 0) {
            regs[0] = (uint64_t)(int64_t)rn;
            break;
        }

        int rc = ext4_frename(oldabs, newabs);
        regs[0] = (rc == EOK) ? 0 : (uint64_t)(int64_t)-ENOENT;
        break;
    }

    case LINUX_SYS_UNLINKAT: {
        /* unlinkat(dirfd, pathname, flags) */
        int dirfd = (int)regs[0];
        const char *pathname = (const char *)regs[1];
        int flags = (int)regs[2];

        if (!pathname) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            break;
        }

        task_t *me = task_current();
        char abspath[128];
        int rpa = resolve_path_at(me, dirfd, pathname, abspath, sizeof(abspath));
        if (rpa < 0) {
            regs[0] = (uint64_t)(int64_t)rpa;
            break;
        }

        int rc;
        if (flags & AT_REMOVEDIR) {
            rc = ext4_dir_rm(abspath);
        } else {
            rc = ext4_fremove(abspath);
        }
        regs[0] = (rc == EOK) ? 0 : (uint64_t)(int64_t)-ENOENT;
        break;
    }

    case LINUX_SYS_OPENAT: {
        /* openat(dirfd, pathname, flags, mode) */
        int dirfd = (int)regs[0];
        const char *pathname = (const char *)regs[1];
        int         flags    = (int)regs[2];
        /* int mode = (int)regs[3]; */
        if (!pathname) { regs[0] = (uint64_t)(int64_t)-ENOENT; break; }

        task_t *me = task_current();
        char   abspath[128];
        int rpa = resolve_path_at(me, dirfd, pathname, abspath, sizeof(abspath));
        if (rpa < 0) {
            regs[0] = (uint64_t)(int64_t)rpa;
            break;
        }

        int pool = fd_pool_alloc();
        if (pool < 0) { regs[0] = (uint64_t)(int64_t)-EMFILE; break; }

        fd_obj_t *obj = &g_fd_pool[pool];

        /* Try as file first */
        int rc = ext4_fopen2(&obj->file, abspath, flags);
        if (rc == EOK) {
            obj->type  = FDT_FILE;
            obj->flags = flags;
            int k = 0;
            while (abspath[k] && k < 127) { obj->path[k] = abspath[k]; k++; }
            obj->path[k] = '\0';
            int fd = task_alloc_fd(me, pool);
            if (fd < 0) { ext4_fclose(&obj->file); fd_pool_free(pool); regs[0] = (uint64_t)(int64_t)-EMFILE; break; }
            regs[0] = (uint64_t)fd;
        } else {
            /* Try as directory */
            rc = ext4_dir_open(&obj->dir, abspath);
            if (rc == EOK) {
                obj->type  = FDT_DIR;
                obj->flags = flags;
                int k = 0;
                while (abspath[k] && k < 127) { obj->path[k] = abspath[k]; k++; }
                obj->path[k] = '\0';
                int fd = task_alloc_fd(me, pool);
                if (fd < 0) { ext4_dir_close(&obj->dir); fd_pool_free(pool); regs[0] = (uint64_t)(int64_t)-EMFILE; break; }
                regs[0] = (uint64_t)fd;
            } else {
                fd_pool_free(pool);
                regs[0] = (uint64_t)(int64_t)-ENOENT;
            }
        }
        break;
    }

    case LINUX_SYS_CLOSE: {
        int fd = (int)regs[0];
        if (fd == 0 || fd == 1 || fd == 2) { regs[0] = 0; break; }
        task_t *me = task_current();
        if (fd < 0 || fd >= (int)TASK_MAX_FD || me->fd_table[fd] == -1) {
            regs[0] = (uint64_t)(int64_t)-EBADF;
            break;
        }
        int idx = me->fd_table[fd];
        fd_obj_t *obj = &g_fd_pool[idx];
        KLOG_DEBUG("[fd] close: pid=%u fd=%d pool_idx=%d type=%d\n",
                  me->id, fd, idx, obj->type);
        if (obj->type == FDT_FILE)
            ext4_fclose(&obj->file);
        else if (obj->type == FDT_DIR)
            ext4_dir_close(&obj->dir);
        fd_pool_free(idx);
        me->fd_table[fd] = -1;
        regs[0] = 0;
        break;
    }

    case LINUX_SYS_DUP3: {
        int oldfd = (int)regs[0];
        int newfd = (int)regs[1];
        task_t *me = task_current();
        /* Simple: just alias newfd → same pool entry as oldfd */
        if (oldfd == newfd) { regs[0] = newfd; break; }
        if (oldfd < 0 || oldfd >= (int)TASK_MAX_FD) { regs[0] = (uint64_t)(int64_t)-EBADF; break; }
        if (newfd < 0 || newfd >= (int)TASK_MAX_FD) { regs[0] = (uint64_t)(int64_t)-EBADF; break; }
        /* Close newfd if open */
        if (me->fd_table[newfd] != -1) {
            int idx = me->fd_table[newfd];
            fd_obj_t *o = &g_fd_pool[idx];
            if (o->type == FDT_FILE) ext4_fclose(&o->file);
            else if (o->type == FDT_DIR) ext4_dir_close(&o->dir);
            fd_pool_free(idx);
            me->fd_table[newfd] = -1;
        }
        /* For stdin/stdout/stderr source */
        if (oldfd <= 2) {
            me->fd_table[newfd] = -1;  /* keep as special */
            regs[0] = newfd;
        } else {
            me->fd_table[newfd] = me->fd_table[oldfd];
            regs[0] = newfd;
        }
        break;
    }

    case LINUX_SYS_FCNTL: {
        /* Return 0 for most ops */
        regs[0] = 0;
        break;
    }

    case LINUX_SYS_PIPE2: {
        /* Stub: return -ENOSYS for now (ash uses pipes for pipelines) */
        regs[0] = (uint64_t)(int64_t)-ENOSYS;
        break;
    }

    case LINUX_SYS_GETDENTS64: {
        int fd   = (int)regs[0];
        char *buf = (char *)regs[1];
        uint64_t count = regs[2];
        if (!buf || count < 32) { regs[0] = (uint64_t)(int64_t)-EINVAL; break; }

        fd_obj_t *obj = task_get_fd(task_current(), fd);
        if (!obj || obj->type != FDT_DIR) {
            regs[0] = (uint64_t)(int64_t)-ENOTDIR;
            break;
        }

        uint64_t written = 0;
        while (written + 32 < count) {
            const ext4_direntry *de = ext4_dir_entry_next(&obj->dir);
            if (!de) break;

            uint8_t namelen = de->name_length;
            /* reclen: must be 8-byte aligned */
            uint16_t reclen = (uint16_t)(19 + namelen + 1);
            reclen = (reclen + 7) & ~7;
            if (written + reclen > count) break;

            struct kernel_dirent64 *kd = (struct kernel_dirent64 *)(buf + written);
            kd->d_ino    = de->inode;
            kd->d_off    = (int64_t)(written + reclen);
            kd->d_reclen = reclen;
            /* Map ext4 inode_type to d_type */
            switch (de->inode_type) {
                case EXT4_DE_REG_FILE: kd->d_type = 8; break;
                case EXT4_DE_DIR:      kd->d_type = 4; break;
                case EXT4_DE_SYMLINK:  kd->d_type = 10; break;
                default:               kd->d_type = 0; break;
            }
            /* copy name */
            for (uint8_t k = 0; k < namelen; k++)
                kd->d_name[k] = (char)de->name[k];
            kd->d_name[namelen] = '\0';
            written += reclen;
        }
        regs[0] = written;
        break;
    }

    case LINUX_SYS_FSTAT: {
        int fd = (int)regs[0];
        struct kernel_stat *st = (struct kernel_stat *)regs[1];
        if (!st) { regs[0] = (uint64_t)(int64_t)-EFAULT; break; }

        if (fd == 0 || fd == 1 || fd == 2) {
            /* stdin/stdout/stderr: return char device stat */
            memset(st, 0, sizeof(*st));
            st->st_mode = 0020666;  /* character device */
            st->st_rdev = (5 << 8) | (fd == 0 ? 0 : 1); /* /dev/tty */
            regs[0] = 0;
            break;
        }
        fd_obj_t *obj = task_get_fd(task_current(), fd);
        if (!obj) { regs[0] = (uint64_t)(int64_t)-EBADF; break; }
        fill_stat_from_ext4(st, obj->path);
        regs[0] = 0;
        break;
    }

    case LINUX_SYS_NEWFSTATAT: {
        /* newfstatat(dirfd, pathname, statbuf, flags) */
        int dirfd = (int)regs[0];
        const char         *pathname = (const char *)regs[1];
        struct kernel_stat *st       = (struct kernel_stat *)regs[2];
        if (!st) { regs[0] = (uint64_t)(int64_t)-EFAULT; break; }
        if (!pathname || pathname[0] == '\0') {
            /* empty pathname: stat the dirfd itself */
            fd_obj_t *obj = task_get_fd(task_current(), dirfd);
            if (!obj) { regs[0] = (uint64_t)(int64_t)-EBADF; break; }
            fill_stat_from_ext4(st, obj->path);
            regs[0] = 0;
            break;
        }
        task_t *me = task_current();
        char abspath[128];
        int rpa = resolve_path_at(me, dirfd, pathname, abspath, sizeof(abspath));
        if (rpa < 0) {
            regs[0] = (uint64_t)(int64_t)rpa;
            break;
        }
        fill_stat_from_ext4(st, abspath);
        regs[0] = 0;
        break;
    }

    case LINUX_SYS_READLINKAT: {
        /* readlinkat: we don't have symlinks, return ENOENT */
        regs[0] = (uint64_t)(int64_t)-ENOENT;
        break;
    }

    case LINUX_SYS_FSYNC:
    case LINUX_SYS_FDATASYNC:
        regs[0] = 0;
        break;

    /* --- 目录操作 --- */
    case LINUX_SYS_GETCWD: {
        char    *buf   = (char *)regs[0];
        uint64_t size  = regs[1];
        if (!buf || size == 0) { regs[0] = (uint64_t)(int64_t)-EINVAL; break; }
        task_t *me = task_current();
        int n = copy_string_to_user(me->cwd, buf, (int)size);
        /* Linux syscall ABI: getcwd 返回写入长度（包含 '\0'） */
        regs[0] = (n >= 0) ? (uint64_t)(n + 1) : (uint64_t)(int64_t)-ERANGE;
        break;
    }

    case LINUX_SYS_CHDIR: {
        const char *path = (const char *)regs[0];
        if (!path) { regs[0] = (uint64_t)(int64_t)-ENOENT; break; }
        task_t *me = task_current();
        char abspath[128];
        resolve_path(me->cwd, path, abspath, sizeof(abspath));
        /* Verify it exists as a directory */
        ext4_dir d;
        if (ext4_dir_open(&d, abspath) != EOK) {
            regs[0] = (uint64_t)(int64_t)-ENOENT;
            break;
        }
        ext4_dir_close(&d);
        /* Update cwd */
        int k = 0;
        while (abspath[k] && k < (int)TASK_CWD_LEN - 1) {
            me->cwd[k] = abspath[k];
            k++;
        }
        me->cwd[k] = '\0';
        regs[0] = 0;
        break;
    }

    /* --- ioctl --- */
    case LINUX_SYS_IOCTL: {
        int   ioctl_fd  = (int)regs[0];
        uint64_t request = regs[1];
        void *argp       = (void *)regs[2];
        (void)ioctl_fd;
        if (request == TCGETS && argp) {
            struct kernel_termios *t = (struct kernel_termios *)argp;
            memset(t, 0, sizeof(*t));
            /* Canonical tty defaults, enough for busybox ash startup */
            t->c_iflag = 0x00000500U; /* ICRNL | IXON */
            t->c_oflag = 0x00000005U; /* OPOST | ONLCR */
            t->c_cflag = 0x000000BFU; /* B38400 | CS8 | CREAD */
            t->c_lflag = 0x00008A3BU; /* ISIG | ICANON | ECHO* | IEXTEN */
            t->c_cc[4] = 4;   /* VEOF  = ^D */
            t->c_cc[5] = 0;   /* VTIME */
            t->c_cc[6] = 1;   /* VMIN  */
            regs[0] = 0;
        } else if (request == TIOCGWINSZ && argp) {
            struct kernel_winsize *ws = (struct kernel_winsize *)argp;
            ws->ws_row    = 24;
            ws->ws_col    = 80;
            ws->ws_xpixel = 0;
            ws->ws_ypixel = 0;
            regs[0] = 0;
        } else if ((request == TCSETS || request == TCSETSW || request == TCSETSF) && argp) {
            regs[0] = 0;
        } else if (request == TIOCGPGRP && argp) {
            *(int *)argp = (int)task_current()->id;
            regs[0] = 0;
        } else if (request == TIOCSPGRP || request == TIOCSWINSZ) {
            regs[0] = 0;
        } else {
            regs[0] = (uint64_t)(int64_t)-ENOTTY;
        }
        break;
    }

    case X86_SYS_POLL:
    case LINUX_SYS_PPOLL:
    case LINUX_SYS_PSELECT6: {
        /*
         * poll(fds, nfds, timeout_ms)
         * ppoll(fds, nfds, timeout, sigmask, sigsetsize)
         * pselect6(...) 目前统一走最小 readiness 语义。
         */
        struct kernel_pollfd *pfds = (struct kernel_pollfd *)regs[0];
        uint64_t nfds = regs[1];
        /* If timeout pointer is NULL or timeout is non-zero, we do a
         * minimal: mark stdin (fd=0) as POLLIN if present, return count */
        int ready = 0;
        if (pfds && nfds > 0) {
            for (uint64_t pi = 0; pi < nfds; pi++) {
                pfds[pi].revents = 0;
                if (pfds[pi].fd == 0) {
                    /* Pretend stdin always has data ready (non-blocking shell) */
                    pfds[pi].revents = pfds[pi].events & 0x01; /* POLLIN=1 */
                    if (pfds[pi].revents) ready++;
                } else if (pfds[pi].fd >= 0) {
                    pfds[pi].revents = pfds[pi].events & 0x01;
                    if (pfds[pi].revents) ready++;
                }
            }
        }
        regs[0] = (uint64_t)ready;
        break;
    }

    /* --- 信号（stub）--- */
    case LINUX_SYS_RT_SIGACTION:
    case LINUX_SYS_RT_SIGPROCMASK:
    case LINUX_SYS_RT_SIGRETURN:
        KLOG_DEBUG("[syscall] rt_sigprocmask: pid=%u setting retval=0, frame->elr=0x%llx\n",
                   current->id, syscall_abi_ip(frame));
        regs[0] = 0;
        break;

    /* --- 系统信息 --- */
    case LINUX_SYS_UNAME: {
        struct kernel_utsname *u = (struct kernel_utsname *)regs[0];
        if (!u) { regs[0] = (uint64_t)(int64_t)-EFAULT; break; }
        copy_string_to_user("Linux",      u->sysname,    sizeof(u->sysname));
        copy_string_to_user("avatar",     u->nodename,   sizeof(u->nodename));
        copy_string_to_user("5.15.0",     u->release,    sizeof(u->release));
        copy_string_to_user("#1 SMP",     u->version,    sizeof(u->version));
#if ARCH_RISCV64
        copy_string_to_user("riscv64",    u->machine,    sizeof(u->machine));
#elif ARCH_AARCH64
        copy_string_to_user("aarch64",    u->machine,    sizeof(u->machine));
#else
        copy_string_to_user("x86_64",     u->machine,    sizeof(u->machine));
#endif
        copy_string_to_user("",           u->domainname, sizeof(u->domainname));
        regs[0] = 0;
        break;
    }

    case LINUX_SYS_CLOCK_GETTIME: {
        struct kernel_timespec *ts = (struct kernel_timespec *)regs[1];
        if (ts) { ts->tv_sec = 0; ts->tv_nsec = 0; }
        regs[0] = 0;
        break;
    }

    case LINUX_SYS_NANOSLEEP: {
        /* Just yield once */
        task_yield();
        regs[0] = 0;
        break;
    }

    case LINUX_SYS_GETRLIMIT:
    case LINUX_SYS_SETRLIMIT:
    case LINUX_SYS_PRLIMIT64: {
        /* Return unlimited for most resources */
        if (syscall_num == LINUX_SYS_GETRLIMIT || syscall_num == LINUX_SYS_PRLIMIT64) {
            /*
             * getrlimit(resource, rlim):                 rlim 在 arg1
             * prlimit64(pid, resource, new, old):        old  在 arg3
             */
            struct kernel_rlimit *rl = (struct kernel_rlimit *)regs[
                (syscall_num == LINUX_SYS_PRLIMIT64) ? 3 : 1];
            if (rl) {
                rl->rlim_cur = (uint64_t)-1;
                rl->rlim_max = (uint64_t)-1;
            }
        }
        regs[0] = 0;
        break;
    }

    case LINUX_SYS_GETRUSAGE:
        if (regs[1]) memset((void *)regs[1], 0, 144);
        regs[0] = 0;
        break;

    case LINUX_SYS_UMASK:
        regs[0] = 0022;
        break;

    case LINUX_SYS_PRCTL:
    case LINUX_SYS_SET_ROBUST_LIST:
    case LINUX_SYS_MPROTECT:
        regs[0] = 0;
        break;

    case LINUX_SYS_MUNMAP:
        /* 不能再“假成功”，否则用户态分配器会在错误前提下继续并破坏堆元数据 */
        regs[0] = (uint64_t)(int64_t)-ENOSYS;
        break;

    case LINUX_SYS_SOCKET:
        regs[0] = (uint64_t)(int64_t)-ENOSYS;
        break;

    case LINUX_SYS_GETRANDOM: {
        char *buf = (char *)regs[0];
        uint64_t len = regs[1];
        if (buf) {
            /* Minimal pseudo-random fill */
            for (uint64_t i = 0; i < len; i++)
                buf[i] = (char)(i ^ 0xA5);
        }
        regs[0] = len;
        break;
    }

    case LINUX_SYS_BRK:
        regs[0] = (uint64_t)sys_brk((void *)regs[0]);
        break;

    case LINUX_SYS_MMAP: {
        regs[0] = sys_mmap(regs[0], regs[1], (int)regs[2], (int)regs[3],
                           (int)regs[4], regs[5]);
        break;
    }

    default:
        KLOG_ERROR("[syscall] Unknown syscall: %llu\n", syscall_num);
        regs[0] = (uint64_t)(int64_t)-ENOSYS;
        break;
    }

    if (g_syscall_entry_count <= 16) {
        KLOG_DEBUG("[syscall] return mapped=%llu ret=0x%llx\n", syscall_num, regs[0]);
    }

    /* 调试：在设置返回值前检查 g_pmm */
    if (g_syscall_entry_count <= 3) {
        // KLOG_ERROR("[syscall] Before set_ret: g_pmm=%p regs[0]=0x%llx\n", g_pmm, regs[0]);
    }
    
    syscall_abi_set_ret(frame, regs[0]);
    
    /* 调试：在设置返回值后检查 g_pmm */
    if (g_syscall_entry_count <= 3) {
        // KLOG_ERROR("[syscall] After set_ret: g_pmm=%p frame->x[10]=0x%llx\n", 
        //            g_pmm, frame->x[10]);
    }
}


/* ── 具体系统调用实现 ──────────────────────────────────────────── */

int64_t sys_write(const char *str, uint64_t len)
{
    if (str == NULL) {
        return -1;
    }

    /* 简单实现：直接输出到 UART */
    /* 注意：这里没有验证用户指针，后续需要改进 */
    for (uint64_t i = 0; i < len; i++) {
        klog_putchar(str[i]);
    }

    return (int64_t)len;
}

void sys_exit(int status)
{
    task_t *current = task_current();

    KLOG_INFO("[syscall] process '%s' (id=%u) exiting with status %d\n",
              current->name, current->id, status);

    /* 调用 task_exit 退出当前进程 */
    task_exit();

    /* 不应该到达这里 */
    while (1)
        ;
}

int64_t sys_yield(void)
{
    task_yield();
    return 0;
}

int64_t sys_getpid(void)
{
    task_t *current = task_current();
    return (int64_t)current->id;
}

int64_t sys_sleep(uint64_t ms)
{
    /* 简单实现：使用忙等待
     * TODO: 改进为基于定时器的睡眠，让出 CPU
     */
    if (ms == 0) {
        return 0;
    }

    /* 暂时使用简单的忙等待循环
     * 假设 CPU 运行在约 1-2 GHz，每次循环约几个纳秒
     */
    volatile uint64_t count = ms * 100000;
    while (count--) {
        __asm__ volatile("nop");
    }

    return 0;
}

int64_t sys_execve(const char *pathname, char **argv, char **envp)
{
    if (pathname == NULL) {
        return -1;
    }

    KLOG_INFO("[syscall] execve called\n");

    /*
     * execve 应该关闭当前进程的所有非标准文件描述符（fd > 2）。
     * 注意：这必须在加载新程序之前执行，因为加载后当前进程就不再运行了。
     */
    task_t *current = task_current();
    KLOG_DEBUG("[execve] pid=%u closing all fds > 2\n", current->id);
    for (int fd = 3; fd < (int)TASK_MAX_FD; fd++) {
        int idx = (int)current->fd_table[fd];
        if (idx < 0 || idx >= FD_POOL_SIZE) {
            current->fd_table[fd] = (int8_t)-1;
            continue;
        }

        fd_obj_t *obj = &g_fd_pool[idx];
        if (obj->type == FDT_FREE) {
            current->fd_table[fd] = (int8_t)-1;
            continue;
        }

        KLOG_TRACE("[execve] closing fd=%d pool_idx=%d type=%d\n",
                  fd, idx, obj->type);
        if (obj->type == FDT_FILE)
            ext4_fclose(&obj->file);
        else if (obj->type == FDT_DIR)
            ext4_dir_close(&obj->dir);
        fd_pool_free(idx);
        current->fd_table[fd] = (int8_t)-1;
    }

    /* 优先尝试 ELF 加载器
     * TODO: 暂时传递 NULL argv/envp，使用默认值
     * 需要实现安全的用户空间参数复制机制
     */
    int rc = elf_loader_load_from_file(pathname, NULL, NULL);

    /* 如果成功，不应该到达这里 */
    return rc;
}

void *sys_brk(void *addr)
{
    task_t  *current = task_current();

    if (!current->is_user_process) {
        return (void *)(uint64_t)(int64_t)-12; /* ENOMEM */
    }

    uint64_t current_brk = current->heap_end;
    uint64_t req_brk = (uint64_t)addr;

    if (g_syscall_entry_count <= 16) {
        KLOG_DEBUG("[brk] req=0x%llx current=0x%llx\n", req_brk, current_brk);
    }

    /* brk(0)：查询当前堆末尾 */
    if ((uint64_t)addr == 0) {
        if (g_syscall_entry_count <= 16) {
            KLOG_DEBUG("[brk] query -> 0x%llx\n", current_brk);
        }
        return (void *)current_brk;
    }

    uint64_t new_brk = (uint64_t)addr;

    /* 缩小或不变：直接更新 */
    if (new_brk <= current_brk) {
        current->heap_end = new_brk;
        if (g_syscall_entry_count <= 16) {
            KLOG_DEBUG("[brk] shrink/no-grow -> 0x%llx\n", new_brk);
        }
        return (void *)new_brk;
    }

#if ARCH_AARCH64 || ARCH_RISCV64
    /* 扩展堆：映射新页 */
    extern pmm_t pmm;  /* 直接使用结构体，绕过 g_pmm 指针 */
    uint64_t old_page_end = ALIGN_UP(current_brk, PAGE_SIZE);
    uint64_t new_page_end = ALIGN_UP(new_brk,     PAGE_SIZE);
    void    *pgd          = phys_to_virt((uint64_t)current->pgd);

    for (uint64_t va = old_page_end; va < new_page_end; va += PAGE_SIZE) {
        uint64_t pa = pmm_alloc_pages(&pmm, 1);
        if (pa == 0) {
            KLOG_ERROR("[brk] Out of memory at va=0x%llx\n", va);
            return (void *)current_brk; /* 返回旧地址表示失败 */
        }
        memset(phys_to_virt(pa), 0, PAGE_SIZE);
        if (mm_vm_map_pages(pgd, va, pa, 1, 0) != 0) {
            /* Page already mapped (e.g. BSS last page overlap) — skip */
            pmm_free_pages(&pmm, pa, 1);
            continue;
        }
    }
#elif ARCH_X86_64
    /* x86_64：扩展堆，使用 g_pmm */
    {
        uint64_t old_page_end = ALIGN_UP(current_brk, PAGE_SIZE);
        uint64_t new_page_end = ALIGN_UP(new_brk,     PAGE_SIZE);
        void    *pgd          = phys_to_virt((uint64_t)current->pgd);

        for (uint64_t va = old_page_end; va < new_page_end; va += PAGE_SIZE) {
            uint64_t pa = pmm_alloc_pages(g_pmm, 1);
            if (pa == 0) {
                KLOG_ERROR("[brk] Out of memory at va=0x%llx\n", va);
                return (void *)current_brk;
            }
            memset(phys_to_virt(pa), 0, PAGE_SIZE);
            if (mm_vm_map_pages(pgd, va, pa, 1, 0) != 0) {
                pmm_free_pages(g_pmm, pa, 1);
                continue; /* 已映射：跳过 */
            }
        }
    }
#endif

    current->heap_end = new_brk;
    if (g_syscall_entry_count <= 16) {
        KLOG_DEBUG("[brk] grow success -> 0x%llx\n", new_brk);
    }
    return (void *)new_brk;
}

void *sys_sbrk(int64_t increment)
{
    /* 简单实现：维护一个简单的堆指针
     * TODO: 实现真正的堆管理器
     */
    static uint64_t heap_end = 0x50000000;
    void *old_brk = (void *)heap_end;

    if (increment == 0) {
        return old_brk;
    }

    heap_end += increment;

    KLOG_DEBUG("[syscall] sbrk(%lld) = 0x%llx\n", increment, old_brk);
    return old_brk;
}

int64_t sys_read(char *buf, uint64_t len)
{
    if (buf == NULL || len == 0) {
        return -1;
    }

    /* 简单实现：阻塞读取单个字符
     * TODO: 实现真正的输入缓冲
     */
    /* 暂时返回 0 表示没有数据 */
    return 0;
}

int64_t sys_open(const char *pathname, int flags, int mode)
{
    (void)flags;
    (void)mode;
    /* 暂未实现文件系统 */
    KLOG_WARN("[syscall] open('%s') not implemented\n", pathname);
    return -1;
}

int64_t sys_close(int fd)
{
    /* 暂未实现文件系统 */
    KLOG_WARN("[syscall] close(%d) not implemented\n", fd);
    return -1;
}

uint64_t sys_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t offset)
{
    (void)prot; (void)offset;

    task_t *current = task_current();
    if (!current->is_user_process) {
        return MMAP_FAILED;
    }

    /* 仅支持匿名私有映射（musl malloc 使用） */
    if (!(flags & MAP_ANONYMOUS)) {
        KLOG_WARN("[mmap] non-anonymous mmap not supported (fd=%d)\n", fd);
        return MMAP_FAILED;
    }

    if (len == 0) {
        return (uint64_t)(int64_t)-EINVAL;
    }

    uint64_t page_off = 0;
    uint64_t map_addr;
    uint64_t size;

    /*
     * Linux 语义要点：
     * 1) MAP_FIXED 时 addr 必须页对齐；
     * 2) 非 MAP_FIXED 的 hint 允许非对齐，但映射长度需要覆盖 page_off + len。
     *
     * 当前内核为避免 hint 带来碎片与覆盖风险：
     * - MAP_FIXED: 按用户指定地址映射；
     * - 非 MAP_FIXED: 忽略 hint，统一从 mmap_next 线性分配。
     */
    if ((flags & MAP_FIXED) != 0) {
        if ((addr & (PAGE_SIZE - 1)) != 0) {
            return (uint64_t)(int64_t)-EINVAL;
        }
        map_addr = addr;
        page_off = 0;
    } else {
        map_addr = ALIGN_UP(current->mmap_next, PAGE_SIZE);
        page_off = 0;
    }

    /* Linux 语义：映射长度按页对齐，不额外扩大。 */
    size = ALIGN_UP(len + page_off, PAGE_SIZE);

    KLOG_DEBUG("[mmap] req: addr=0x%llx len=0x%llx flags=0x%x fd=%d off=0x%llx -> base=0x%llx size=0x%llx\n",
               addr, len, flags, fd, offset, map_addr, size);

#if ARCH_AARCH64 || ARCH_RISCV64
    extern pmm_t pmm;  /* 直接使用结构体 */
    void *pgd = phys_to_virt((uint64_t)current->pgd);

    for (uint64_t va = map_addr; va < map_addr + size; va += PAGE_SIZE) {
        if ((flags & MAP_FIXED) != 0) {
            uint64_t old_pa = mm_vm_get_paddr(pgd, va);
            if (old_pa != 0) {
                /* MAP_FIXED 允许覆盖已有映射。最小实现：复用并清零。 */
                memset(phys_to_virt(old_pa), 0, PAGE_SIZE);
                continue;
            }
        }

        uint64_t pa = pmm_alloc_pages(&pmm, 1);
        if (pa == 0) {
            KLOG_ERROR("[mmap] Out of memory at va=0x%llx\n", va);
            return MMAP_FAILED;
        }
        memset(phys_to_virt(pa), 0, PAGE_SIZE);
        if (mm_vm_map_pages(pgd, va, pa, 1, 0) != 0) {
            KLOG_WARN("[mmap] map failed: va=0x%llx flags=0x%x\n", va, flags);
            pmm_free_pages(&pmm, pa, 1);
            return MMAP_FAILED;
        }
    }
#elif ARCH_X86_64
    {
        void *pgd = phys_to_virt((uint64_t)current->pgd);

        for (uint64_t va = map_addr; va < map_addr + size; va += PAGE_SIZE) {
            if ((flags & MAP_FIXED) != 0) {
                uint64_t old_pa = mm_vm_get_paddr(pgd, va);
                if (old_pa != 0) {
                    /* MAP_FIXED 允许覆盖已有映射。最小实现：复用并清零。 */
                    memset(phys_to_virt(old_pa), 0, PAGE_SIZE);
                    continue;
                }
            }

            uint64_t pa = pmm_alloc_pages(g_pmm, 1);
            if (pa == 0) {
                KLOG_ERROR("[mmap] Out of memory at va=0x%llx\n", va);
                return MMAP_FAILED;
            }
            memset(phys_to_virt(pa), 0, PAGE_SIZE);
            if (mm_vm_map_pages(pgd, va, pa, 1, 0) != 0) {
                KLOG_WARN("[mmap] map failed: va=0x%llx flags=0x%x\n", va, flags);
                pmm_free_pages(g_pmm, pa, 1);
                return MMAP_FAILED;
            }
        }
    }
#endif

    if ((flags & MAP_FIXED) == 0) {
        current->mmap_next = map_addr + size;
    }

    KLOG_DEBUG("[mmap] 0x%llx - 0x%llx (len=0x%llx)\n", map_addr, map_addr + size, len);
    return map_addr + page_off;
}

int64_t sys_gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    /* 简单实现：返回固定时间
     * TODO: 实现真正的时钟驱动
     */
    if (tv == NULL) {
        return -1;
    }

    /* 暂时返回一个固定的时间值 */
    tv->tv_sec = 1000;
    tv->tv_usec = 0;

    return 0;
}
