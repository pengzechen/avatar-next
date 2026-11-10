/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   fs/file_ops.c - openat / close / dup3 / fcntl / pipe2

   由 kernel/syscall/syscall.c 拆出。
*/
#include "syscall/syscall_internal.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/fs/path.h"
#include "task/task.h"
#include "klog.h"
#include "string.h"
#include "syscall/fs/pipe.h"
#include "syscall/io/epoll.h"
#include "syscall/net/ksocket.h"
#include "syscall/fs/pty.h"
#include "arch.h"
#include "vfs.h"
#include "fs/vfs/vfs_internal.h"
#include <ext4.h>
#include <ext4_errno.h>

static bool trace_heavy_task(task_t *current)
{
    return current && current->is_user_process && current->mm->heap_end >= 0x3000000ULL;
}

/*
   open 标志的 ABI 翻译。

   aarch64 的 fcntl 位布局与 asm-generic(x86_64/riscv64) 不同, 而内核内部
   常量沿用 asm-generic 的数值:
                     aarch64      内核内部(asm-generic)
     O_DIRECTORY     040000       0200000
     O_NOFOLLOW      0100000      0400000
     O_DIRECT       0200000      040000
   不翻译的后果是"错义"而不是报错: aarch64 上 open(file, O_DIRECTORY)
   被当成普通 open 成功返回, 调用方直到 fdopendir 才发现 ENOTDIR
   (LTP 的 rmobj 正是这样, tmpdir 清理变 TWARN, 测试进程以 4 退出);
   反过来 O_DIRECT 会被误认成 O_DIRECTORY。
   O_NOFOLLOW 内核本来就不实现, 保留原样即可。
*/
#define AARCH64_O_DIRECTORY 040000
#define AARCH64_O_DIRECT    0200000

static int open_flags_from_user(int flags)
{
#if ARCH_AARCH64
    int out = flags & ~(AARCH64_O_DIRECTORY | AARCH64_O_DIRECT);
    if (flags & AARCH64_O_DIRECTORY)
        out |= O_DIRECTORY_AVATAR;
    return out;
#else
    return flags;
#endif
}

static int open_flags_to_user(int flags)
{
#if ARCH_AARCH64
    int out = flags & ~O_DIRECTORY_AVATAR;
    if (flags & O_DIRECTORY_AVATAR)
        out |= AARCH64_O_DIRECTORY;
    return out;
#else
    return flags;
#endif
}

void openat_handler(uint64_t regs[6], task_t *current)
{
    int dirfd = (int)regs[0];
    const char *pathname = (const char *)regs[1];
    int flags = open_flags_from_user((int)regs[2]);
    int mode = (int)regs[3];
    if (!pathname) {
        regs[0] = (uint64_t)(int64_t)-ENOENT;
        return;
    }
    /*
       O_CREAT 的 mode 要按进程 umask 去掉权限位（Linux 语义）。
       O_CREAT 在三架构都是 0100。以前直接把 mode 透传，导致
       umask()/umask01 与 fstat02 都不对。
    */
    if ((flags & 0100) && current)
        mode &= ~(int)current->umask;

    char abspath[128];
    int rpa = resolve_path_at(current, dirfd, pathname, abspath, sizeof(abspath));
    if (rpa < 0) {
        if (trace_heavy_task(current)) {
            KLOG_SYSCALL("[vfsop] pid=%u openat path=%s flags=0x%x rc=%d\n", current->id, pathname, flags, rpa);
        }
        regs[0] = (uint64_t)(int64_t)rpa;
        return;
    }
    follow_symlinks(abspath, sizeof(abspath));
    if (trace_heavy_task(current)) {
        KLOG_SYSCALL("[vfsop] pid=%u openat path=%s resolved=%s flags=0x%x\n", current->id, pathname, abspath, flags);
    }

    /* ── PTY: /dev/ptmx 和 /dev/pts/N(不需要 fd pool 预分配)── */
    if (strcmp(abspath, "/dev/tty") == 0) {
        if (current->ctty_pty_idx < 0) {
            regs[0] = (uint64_t)(int64_t)-ENXIO;
            return;
        }
        int fd = pty_open_slave(current, current->ctty_pty_idx);
        regs[0] = fd >= 0 ? (uint64_t)fd : (uint64_t)(int64_t)fd;
        return;
    }
    if (strcmp(abspath, "/dev/ptmx") == 0) {
        int fd = pty_alloc_master(current);
        regs[0] = fd >= 0 ? (uint64_t)fd : (uint64_t)(int64_t)fd;
        return;
    }
    {
        int pts_idx = pty_match_pts_path(abspath);
        if (pts_idx >= 0) {
            int fd = pty_open_slave(current, pts_idx);
            if (fd < 0)
                KLOG_WARN("[pty] open_slave(%d) failed: %d path=%s\n", pts_idx, fd, abspath);
            regs[0] = fd >= 0 ? (uint64_t)fd : (uint64_t)(int64_t)fd;
            return;
        }
    }

    vfs_file_t *vf = NULL;
    int rc = vfs_open(abspath, flags, mode, &vf);
    if (rc < 0) {
        regs[0] = (uint64_t)(int64_t)rc;
        return;
    }

    int pool = fd_pool_alloc();
    if (pool < 0) {
        vfs_close(vf);
        regs[0] = (uint64_t)(int64_t)-EMFILE;
        return;
    }

    fd_obj_attach_vfs(pool, vf);

    int fd = task_alloc_fd(current, pool);
    if (fd < 0) {
        vfs_close(vf);
        fd_pool_free(pool);
        regs[0] = (uint64_t)(int64_t)-EMFILE;
        return;
    }
    /*
       open(O_CLOEXEC)：三架构的 O_CLOEXEC 都是 0x80000。
       以前这里完全没记这位, 而 execve 现在按位图关闭 CLOEXEC fd —— 不记
       就等于 O_CLOEXEC 被忽略, fd 会泄漏给 exec 后的新程序。
    */
    if (flags & 0x80000)
        current->files->fd_cloexec[fd / 8] |= (uint8_t)(1u << (fd % 8));
    if (trace_heavy_task(current)) {
        KLOG_SYSCALL("[vfsop] pid=%u openat fd=%d pool=%d path=%s kind=%d\n", current->id, fd, pool, vf->path,
                     vf->kind);
    }
    regs[0] = (uint64_t)fd;
}

void close_handler(uint64_t regs[6], task_t *current)
{
    int fd = (int)regs[0];
    if (fd < 0 || fd >= (int)TASK_MAX_FD) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }
    /*
       一次原子摘取同时解决三件事：fd 0/1/2 默认 UART(fd_table 是 -1,
       close 直接成功), 把表项置回 -1, 以及保证同一个 fd 只会有一个
       线程拿到 idx(否则两个线程都关同一个槽, 第二次归还时槽可能已经被
       别的 open 拿走)。详见 task_take_fd 的注释。
    */
    int idx = task_take_fd(current, fd);
    if (idx < 0 || idx >= FD_POOL_SIZE) {
        /*
           fd 0/1/2 的 fd_table 初值就是 -1（隐式控制台），close 应当成功；
           fd >= 3 从未打开过/已经关过一次 → Linux 返回 EBADF。
           LTP close02 就是测重复 close。
        */
        regs[0] = (fd <= 2) ? 0 : (uint64_t)(int64_t)-EBADF;
        return;
    }
    fd_obj_t *obj = &g_fd_pool[idx];
    KLOG_SYSCALL("[fd] close: pid=%u fd=%d pool_idx=%d type=%d\n", current->id, fd, idx, obj->type);
    fd_close_notify(idx);
    fd_obj_close(idx);
    fd_pool_free(idx);
    regs[0] = 0;
}

static int dup_to_fd(task_t *current, int oldfd, int newfd, int flags)
{
    if (oldfd < 0 || oldfd >= (int)TASK_MAX_FD)
        return -EBADF;
    if (newfd < 0 || newfd >= (int)TASK_MAX_FD)
        return -EBADF;

    fd_obj_t *src = task_get_fd(current, oldfd);
    if (oldfd > 2 && !src)
        return -EBADF;

    int old_idx = task_take_fd(current, newfd);
    if (old_idx >= 0 && old_idx < FD_POOL_SIZE) {
        fd_close_notify(old_idx);
        fd_obj_close(old_idx);
        fd_pool_free(old_idx);
    }

    if (!src) {
        int uart_idx = fd_pool_alloc();
        if (uart_idx < 0)
            return -EMFILE;
        g_fd_pool[uart_idx].type = FDT_ALLOCATED;
        g_fd_pool[uart_idx].flags = 0;
        g_fd_pool[uart_idx].vfs_file = NULL;
        g_fd_pool[uart_idx].path[0] = '\0';
        current->files->fd_table[newfd] = uart_idx;
    } else {
        int new_idx = fd_pool_alloc();
        if (new_idx < 0)
            return -EMFILE;
        g_fd_pool[new_idx] = *src;
        g_fd_pool[new_idx].wq.waiter_count = 0;
        /* vfs_file 走 vfs_ref；ION/dma-buf 走各自 fd_ops->dup */
        if (fd_obj_ref(new_idx) != 0) {
            fd_pool_free(new_idx);
            return -EBADF;
        }
        current->files->fd_table[newfd] = new_idx;
    }
    if (flags & 0x80000)
        current->files->fd_cloexec[newfd / 8] |= (uint8_t)(1u << (newfd % 8));
    else
        current->files->fd_cloexec[newfd / 8] &= (uint8_t)~(1u << (newfd % 8));
    return newfd;
}

void dup_handler(uint64_t regs[6], task_t *current)
{
    int oldfd = (int)regs[0];

    if (oldfd < 0 || oldfd >= (int)TASK_MAX_FD) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }
    if (oldfd > 2 && !task_get_fd(current, oldfd)) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }

    for (int newfd = 3; newfd < (int)TASK_MAX_FD; newfd++) {
        if (current->files->fd_table[newfd] == -1) {
            int ret = dup_to_fd(current, oldfd, newfd, 0);
            regs[0] = ret >= 0 ? (uint64_t)ret : (uint64_t)(int64_t)ret;
            return;
        }
    }

    regs[0] = (uint64_t)(int64_t)-EMFILE;
}

void dup3_handler(uint64_t regs[6], task_t *current)
{
    int oldfd = (int)regs[0];
    int newfd = (int)regs[1];
    int flags = (int)regs[2];

    /* dup3 只认 O_CLOEXEC；其它位（包括 -1）一律 EINVAL。LTP dup3_02 */
    if (flags & ~0x80000) {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }
    if (oldfd == newfd) {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }

    int ret = dup_to_fd(current, oldfd, newfd, flags);
    regs[0] = ret >= 0 ? (uint64_t)ret : (uint64_t)(int64_t)ret;
}

void fcntl_handler(uint64_t regs[6], task_t *current)
{
    int fd = (int)regs[0];
    int cmd = (int)regs[1];

    if (fd < 0 || fd >= (int)TASK_MAX_FD) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }

    /* fd 0-2 默认是 UART, 但也可能已被 dup2() 重定向到 PTY/pipe/socket。 */
    fd_obj_t *obj = task_get_fd(current, fd);
    if (!obj && fd >= 3) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }

#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC      1
#define O_NONBLOCK      04000
#define O_APPEND        02000
#define O_ASYNC         020000
/* F_SETFL 只能改这些位 */
#define SETFL_MASK (O_NONBLOCK | O_APPEND | O_ASYNC)

    switch (cmd) {
    case F_GETFD: {
        int bit = (current->files->fd_cloexec[fd / 8] >> (fd % 8)) & 1;
        regs[0] = (uint64_t)bit;
        return;
    }
    case F_SETFD: {
        int val = (int)regs[2];
        if (val & FD_CLOEXEC)
            current->files->fd_cloexec[fd / 8] |= (uint8_t)(1u << (fd % 8));
        else
            current->files->fd_cloexec[fd / 8] &= (uint8_t)~(1u << (fd % 8));
        regs[0] = 0;
        return;
    }
    case F_GETFL: {
        regs[0] = obj ? (uint64_t)(uint32_t)open_flags_to_user(obj->flags) : 0;
        return;
    }
    case F_SETFL: {
        int val = (int)regs[2];
        if (obj) {
            obj->flags = (obj->flags & ~SETFL_MASK) | (val & SETFL_MASK);
            if (obj->vfs_file)
                obj->vfs_file->flags = obj->flags;
        }
        regs[0] = 0;
        return;
    }
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        /* F_DUPFD / F_DUPFD_CLOEXEC: duplicate fd, allocate >= arg */
        int minfd = (int)regs[2];
        if (minfd < 0)
            minfd = 0;
        if (!obj && fd > 2) {
            regs[0] = (uint64_t)(int64_t)-EBADF;
            return;
        }
        if (!obj) {
            /* dup UART fd: create a real pool object so dup2(saved, 1) works. */
            for (int nf = minfd < 3 ? 3 : minfd; nf < (int)TASK_MAX_FD; nf++) {
                if (current->files->fd_table[nf] == -1) {
                    int uart_idx = fd_pool_alloc();
                    if (uart_idx < 0) {
                        regs[0] = (uint64_t)(int64_t)-EMFILE;
                        return;
                    }
                    g_fd_pool[uart_idx].type = FDT_ALLOCATED;
                    g_fd_pool[uart_idx].flags = 0;
                    g_fd_pool[uart_idx].vfs_file = NULL;
                    g_fd_pool[uart_idx].path[0] = '\0';
                    current->files->fd_table[nf] = (int16_t)uart_idx;
                    if (cmd == F_DUPFD_CLOEXEC)
                        current->files->fd_cloexec[nf / 8] |= (uint8_t)(1u << (nf % 8));
                    else
                        current->files->fd_cloexec[nf / 8] &= (uint8_t)~(1u << (nf % 8));
                    regs[0] = (uint64_t)nf;
                    return;
                }
            }
            regs[0] = (uint64_t)(int64_t)-EMFILE;
            return;
        }
        int new_idx = fd_pool_alloc();
        if (new_idx < 0) {
            regs[0] = (uint64_t)(int64_t)-EMFILE;
            return;
        }
        g_fd_pool[new_idx] = *obj;
        g_fd_pool[new_idx].wq.waiter_count = 0;
        fd_obj_ref(new_idx);
        for (int nf = minfd < 3 ? 3 : minfd; nf < (int)TASK_MAX_FD; nf++) {
            if (current->files->fd_table[nf] == -1) {
                current->files->fd_table[nf] = (int16_t)new_idx;
                if (cmd == F_DUPFD_CLOEXEC)
                    current->files->fd_cloexec[nf / 8] |= (uint8_t)(1u << (nf % 8));
                else
                    current->files->fd_cloexec[nf / 8] &= (uint8_t)~(1u << (nf % 8));
                regs[0] = (uint64_t)nf;
                return;
            }
        }
        fd_pool_free(new_idx);
        regs[0] = (uint64_t)(int64_t)-EMFILE;
        return;
    }
    default:
        KLOG_SYSCALL("[fcntl] unsupported cmd=%d fd=%d\n", cmd, fd);
        regs[0] = 0;
        return;
    }

#undef F_DUPFD
#undef F_DUPFD_CLOEXEC
#undef F_GETFD
#undef F_SETFD
#undef F_GETFL
#undef F_SETFL
#undef FD_CLOEXEC
#undef O_NONBLOCK
#undef O_APPEND
#undef O_ASYNC
#undef SETFL_MASK
}

void pipe2_handler(uint64_t regs[6], task_t *current)
{
    int *user_fds = (int *)regs[0];
    int flags = (int)regs[1];

    if (!user_fds) {
        regs[0] = (uint64_t)(int64_t)-EFAULT;
        return;
    }
    /*
       pipe2 认 O_CLOEXEC(0x80000)、O_NONBLOCK(0x800) 和 O_DIRECT。
       O_DIRECT 是 Linux 3.4 起 pipe2 支持的包模式标志(LTP pipe2_01 覆盖)。
       注意 O_DIRECT 的数值是**架构相关**的：
         x86_64           0x4000
         aarch64/riscv64  0x10000
       本内核不区分包边界，但 F_GETFL 必须能把它读回来。
    */
#define PIPE2_GNORE_FLAGS (0x800u | 0x4000u | 0x10000u)
    if (flags & ~(0x80000 | PIPE2_GNORE_FLAGS)) {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }

    int fds[2];
    int rc = pipe_alloc(current, fds);
    if (rc < 0) {
        regs[0] = (uint64_t)(int64_t)-EMFILE;
        return;
    }

    /*
       把 flags 落到 fd 对象上：
         - O_NONBLOCK 必须进 obj->flags / vfs_file->flags，pipe_read/write
           才会走非阻塞路径，fcntl(F_GETFL) 也要能看到它(LTP pipe2_01)
         - O_CLOEXEC 记进 fd 位图，execve 时会关闭(LTP pipe2_02)
    */
    for (int i = 0; i < 2; i++) {
        fd_obj_t *obj = task_get_fd(current, fds[i]);
        if (obj) {
            obj->flags = (obj->flags & ~PIPE2_GNORE_FLAGS) | (flags & PIPE2_GNORE_FLAGS);
            if (obj->vfs_file)
                obj->vfs_file->flags = obj->flags;
        }
        if (flags & 0x80000)
            current->files->fd_cloexec[fds[i] / 8] |= (uint8_t)(1u << (fds[i] % 8));
    }

    /* 用户指针必须经 copy_to_user_bytes（CLAUDE.md 用户指针规则） */
    if (copy_to_user_bytes(fds, user_fds, sizeof(fds)) < 0) {
        for (int i = 0; i < 2; i++) {
            int idx = task_take_fd(current, fds[i]);
            if (idx >= 0) {
                fd_obj_close(idx);
                fd_pool_free(idx);
            }
        }
        regs[0] = (uint64_t)(int64_t)-EFAULT;
        return;
    }
#undef PIPE2_GNORE_FLAGS
    regs[0] = 0;
}
