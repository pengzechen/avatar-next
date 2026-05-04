#include "syscall/syscall.h"
#include "loader/bin_loader.h"
#include "loader/elf_loader.h"
#include "klog.h"
#include "task/task.h"
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

struct kernel_pollfd {
    int   fd;
    short events;
    short revents;
};

#define MAP_ANONYMOUS  0x20
#define MAP_PRIVATE    0x02
#define MMAP_FAILED    ((uint64_t)(int64_t)-1)

/* POSIX errno values */
#define ENOSYS   38
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
 * struct stat / dirent definitions (Linux AArch64 ABI)
 * ───────────────────────────────────────────────────────────────── */
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

/* ─────────────────────────────────────────────────────────────────
 * Main syscall dispatcher
 * ───────────────────────────────────────────────────────────────── */
void syscall_handler(trap_frame_t *frame)
{
    static uint32_t s_syscall_log_count = 0;
    uint64_t regs[9] = {0};
    for (int i = 0; i < 6; i++) {
        regs[i] = syscall_abi_arg(frame, i);
    }
    uint64_t syscall_num = syscall_abi_nr(frame);

    task_t *current = task_current();
    uint64_t ip = syscall_abi_ip(frame);

    /* 验证 frame 完整性 */
    if (ip < 0x1000) {
        KLOG_ERROR("[syscall] CORRUPTION: pid=%u nr=%llu frame->elr=0x%llx < 0x1000!\n",
                   current->id, syscall_num, ip);
        KLOG_ERROR("[syscall] frame=%p, regs=%p, sp=%p\n",
                   frame, regs, __builtin_frame_address(0));
    }

    KLOG_DEBUG("[syscall] pid=%u nr=%llu args=[0x%llx, 0x%llx, 0x%llx]\n",
               current->id, syscall_num, regs[0], regs[1], regs[2]);
    if (s_syscall_log_count < 32) {
        KLOG_INFO("[syscall] pid=%u nr=%llu args=[0x%llx,0x%llx,0x%llx]\n",
                  current->id, syscall_num, regs[0], regs[1], regs[2]);
        s_syscall_log_count++;
    }

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

        /* mmap 区域（当前实现从 0x30000000 线性向上分配） */
        if (clone_copy_ok && parent->mmap_next > 0x30000000ULL)
            CLONE_COPY_RANGE(0x30000000ULL, parent->mmap_next);

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

    case LINUX_SYS_KILL:
    case LINUX_SYS_TGKILL:
        regs[0] = 0; /* stub */
        break;

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
            /* stdin: 阻塞读 UART（跨架构统一接口） */
            uint64_t n = 0;
            while (n < count) {
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
        regs[0] = (n >= 0) ? (uint64_t)buf : (uint64_t)(int64_t)-ERANGE;
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
        if (request == TIOCGWINSZ && argp) {
            struct kernel_winsize *ws = (struct kernel_winsize *)argp;
            ws->ws_row    = 24;
            ws->ws_col    = 80;
            ws->ws_xpixel = 0;
            ws->ws_ypixel = 0;
            regs[0] = 0;
        } else if (request == TIOCGPGRP && argp) {
            *(int *)argp = (int)task_current()->id;
            regs[0] = 0;
        } else if (request == TIOCSPGRP || request == TIOCSWINSZ) {
            regs[0] = 0;
        } else {
            /* TCGETS/TCSETS/other: success with zeroed output */
            regs[0] = 0;
        }
        break;
    }

    case LINUX_SYS_PPOLL:
    case LINUX_SYS_PSELECT6: {
        /* ppoll(fds, nfds, timeout, sigmask, sigsetsize) */
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
            struct kernel_rlimit *rl = (struct kernel_rlimit *)regs[
                (syscall_num == LINUX_SYS_PRLIMIT64) ? 2 : 1];
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
    case LINUX_SYS_MUNMAP:
        regs[0] = 0;
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

    syscall_abi_set_ret(frame, regs[0]);

    /* 系统调用处理完成后验证 frame 完整性 */
    ip = syscall_abi_ip(frame);
    if (ip < 0x1000) {
        KLOG_ERROR("[syscall] POST-SYSCALL CORRUPTION: pid=%u nr=%llu frame->elr=0x%llx < 0x1000!\n",
                   current->id, syscall_num, ip);
        KLOG_ERROR("[syscall]   frame=%p, regs=%p, retval=0x%llx\n",
                   frame, regs, regs[0]);
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

    KLOG_INFO("[syscall] execve('%s')\n", pathname);

    /*
     * execve 应该关闭当前进程的所有非标准文件描述符（fd > 2）。
     * 注意：这必须在加载新程序之前执行，因为加载后当前进程就不再运行了。
     */
    task_t *current = task_current();
    KLOG_DEBUG("[execve] pid=%u closing all fds > 2\n", current->id);
    for (int fd = 3; fd < (int)TASK_MAX_FD; fd++) {
        if (current->fd_table[fd] != -1) {
            int idx = current->fd_table[fd];
            fd_obj_t *obj = &g_fd_pool[idx];
            KLOG_TRACE("[execve] closing fd=%d pool_idx=%d type=%d\n",
                      fd, idx, obj->type);
            if (obj->type == FDT_FILE)
                ext4_fclose(&obj->file);
            else if (obj->type == FDT_DIR)
                ext4_dir_close(&obj->dir);
            fd_pool_free(idx);
            current->fd_table[fd] = -1;
        }
    }

    /* 优先尝试 ELF 加载器 */
    int rc = elf_loader_load_from_file(pathname, argv, envp);

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

    /* brk(0)：查询当前堆末尾 */
    if ((uint64_t)addr == 0) {
        return (void *)current_brk;
    }

    uint64_t new_brk = (uint64_t)addr;

    /* 缩小或不变：直接更新 */
    if (new_brk <= current_brk) {
        current->heap_end = new_brk;
        return (void *)new_brk;
    }

#if ARCH_AARCH64
    /* 扩展堆：映射新页 */
    uint64_t old_page_end = ALIGN_UP(current_brk, PAGE_SIZE);
    uint64_t new_page_end = ALIGN_UP(new_brk,     PAGE_SIZE);
    void    *pgd          = phys_to_virt((uint64_t)current->pgd);

    for (uint64_t va = old_page_end; va < new_page_end; va += PAGE_SIZE) {
        uint64_t pa = pmm_alloc_pages(g_pmm, 1);
        if (pa == 0) {
            KLOG_ERROR("[brk] Out of memory at va=0x%llx\n", va);
            return (void *)current_brk; /* 返回旧地址表示失败 */
        }
        memset(phys_to_virt(pa), 0, PAGE_SIZE);
        if (mm_vm_map_pages(pgd, va, pa, 1, 0) != 0) {
            /* Page already mapped (e.g. BSS last page overlap) — skip */
            pmm_free_pages(g_pmm, pa, 1);
            continue;
        }
    }
#endif

    current->heap_end = new_brk;
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

    uint64_t size     = ALIGN_UP(len, PAGE_SIZE);
    uint64_t map_addr = (addr != 0) ? ALIGN_DOWN(addr, PAGE_SIZE) : current->mmap_next;

#if ARCH_AARCH64
    void *pgd = phys_to_virt((uint64_t)current->pgd);

    for (uint64_t va = map_addr; va < map_addr + size; va += PAGE_SIZE) {
        uint64_t pa = pmm_alloc_pages(g_pmm, 1);
        if (pa == 0) {
            KLOG_ERROR("[mmap] Out of memory at va=0x%llx\n", va);
            return MMAP_FAILED;
        }
        memset(phys_to_virt(pa), 0, PAGE_SIZE);
        if (mm_vm_map_pages(pgd, va, pa, 1, 0) != 0) {
            pmm_free_pages(g_pmm, pa, 1);
            return MMAP_FAILED;
        }
    }
#endif

    if (addr == 0) {
        current->mmap_next = map_addr + size;
    }

    KLOG_DEBUG("[mmap] 0x%llx - 0x%llx (len=0x%llx)\n", map_addr, map_addr + size, len);
    return map_addr;
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
