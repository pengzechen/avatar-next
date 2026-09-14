/*
 * fs/file_ops.c - openat / close / dup3 / fcntl / pipe2
 *
 * 由 kernel/syscall/syscall.c 拆出。
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
#include "vfs.h"
#if DRIVER_ION
#include "ion/ion.h"
#endif
#include <ext4.h>
#include <ext4_errno.h>

void openat_handler(uint64_t regs[6], task_t *current)
{
    int dirfd = (int)regs[0];
    const char *pathname = (const char *)regs[1];
    int         flags    = (int)regs[2];
    if (!pathname) { regs[0] = (uint64_t)(int64_t)-ENOENT; return; }

    char abspath[128];
    int rpa = resolve_path_at(current, dirfd, pathname, abspath, sizeof(abspath));
    if (rpa < 0) {
        regs[0] = (uint64_t)(int64_t)rpa;
        return;
    }
    follow_symlinks(abspath, sizeof(abspath));

    /* ── PTY: /dev/ptmx 和 /dev/pts/N（不需要 fd pool 预分配）── */
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
                KLOG_WARN("[pty] open_slave(%d) failed: %d path=%s\n",
                          pts_idx, fd, abspath);
            regs[0] = fd >= 0 ? (uint64_t)fd : (uint64_t)(int64_t)fd;
            return;
        }
    }

    vfs_file_t *vf = NULL;
    int rc = vfs_open(abspath, flags, 0, &vf);
    if (rc < 0) { regs[0] = (uint64_t)(int64_t)rc; return; }

    int pool = fd_pool_alloc();
    if (pool < 0) {
        vfs_close(vf);
        regs[0] = (uint64_t)(int64_t)-EMFILE;
        return;
    }

    fd_obj_t *obj = &g_fd_pool[pool];
    fd_obj_attach_vfs(pool, vf);

    int fd = task_alloc_fd(current, pool);
    if (fd < 0) {
        vfs_close(vf);
        fd_pool_free(pool);
        regs[0] = (uint64_t)(int64_t)-EMFILE;
        return;
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
    /* fd 0/1/2 默认 UART（fd_table == -1），close 直接成功 */
    if (current->fd_table[fd] == -1) {
        regs[0] = 0;
        return;
    }
    int idx = current->fd_table[fd];
    fd_obj_t *obj = &g_fd_pool[idx];
    KLOG_DEBUG("[fd] close: pid=%u fd=%d pool_idx=%d type=%d\n",
              current->id, fd, idx, obj->type);
    fd_close_notify(idx);
    fd_obj_close(idx);
    fd_pool_free(idx);
    current->fd_table[fd] = -1;
    regs[0] = 0;
}

static int dup_to_fd(task_t *current, int oldfd, int newfd, int flags)
{
    if (oldfd < 0 || oldfd >= (int)TASK_MAX_FD) return -EBADF;
    if (newfd < 0 || newfd >= (int)TASK_MAX_FD) return -EBADF;

    fd_obj_t *src = (oldfd > 2) ? task_get_fd(current, oldfd) : NULL;
    if (oldfd > 2 && !src) return -EBADF;

    if (current->fd_table[newfd] != -1) {
        int idx = current->fd_table[newfd];
        fd_obj_t *o = &g_fd_pool[idx];
        fd_close_notify(idx);
        fd_obj_close(idx);
        fd_pool_free(idx);
        current->fd_table[newfd] = -1;
    }

    if (!src) {
        int uart_idx = fd_pool_alloc();
        if (uart_idx < 0) return -EMFILE;
        g_fd_pool[uart_idx].type  = FDT_ALLOCATED;
        g_fd_pool[uart_idx].flags = 0;
        g_fd_pool[uart_idx].vfs_file = NULL;
        current->fd_table[newfd] = uart_idx;
    } else {
        int new_idx = fd_pool_alloc();
        if (new_idx < 0) return -EMFILE;
#if DRIVER_ION
        if (src->type == FDT_ION && ion_ref((ion_handle_t)src->ion.handle) != 0) {
            fd_pool_free(new_idx);
            return -EBADF;
        }
#endif
        g_fd_pool[new_idx] = *src;
        g_fd_pool[new_idx].wq.waiter_count = 0;
        fd_obj_ref(new_idx);
        current->fd_table[newfd] = new_idx;
    }
    if (flags & 0x80000)
        current->fd_cloexec[newfd / 8] |= (uint8_t)(1u << (newfd % 8));
    else
        current->fd_cloexec[newfd / 8] &= (uint8_t)~(1u << (newfd % 8));
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
        if (current->fd_table[newfd] == -1) {
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

    if (oldfd == newfd) {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }

    int ret = dup_to_fd(current, oldfd, newfd, flags);
    regs[0] = ret >= 0 ? (uint64_t)ret : (uint64_t)(int64_t)ret;
}

void fcntl_handler(uint64_t regs[6], task_t *current)
{
    int fd  = (int)regs[0];
    int cmd = (int)regs[1];

    if (fd < 0 || fd >= (int)TASK_MAX_FD) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }

    /* fd 0-2 默认是 UART，但也可能已被 dup2() 重定向到 PTY/pipe/socket。 */
    fd_obj_t *obj = task_get_fd(current, fd);
    if (!obj && fd >= 3) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }

    #define F_DUPFD     0
    #define F_GETFD     1
    #define F_SETFD     2
    #define F_GETFL     3
    #define F_SETFL     4
    #define F_DUPFD_CLOEXEC 1030
    #define FD_CLOEXEC  1
    #define O_NONBLOCK  04000
    #define O_APPEND    02000
    #define O_ASYNC     020000
    /* F_SETFL 只能改这些位 */
    #define SETFL_MASK  (O_NONBLOCK | O_APPEND | O_ASYNC)

    switch (cmd) {
    case F_GETFD: {
        int bit = (current->fd_cloexec[fd / 8] >> (fd % 8)) & 1;
        regs[0] = (uint64_t)bit;
        return;
    }
    case F_SETFD: {
        int val = (int)regs[2];
        if (val & FD_CLOEXEC)
            current->fd_cloexec[fd / 8] |= (uint8_t)(1u << (fd % 8));
        else
            current->fd_cloexec[fd / 8] &= (uint8_t)~(1u << (fd % 8));
        regs[0] = 0;
        return;
    }
    case F_GETFL: {
        regs[0] = obj ? (uint64_t)(uint32_t)obj->flags : 0;
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
        if (minfd < 0) minfd = 0;
        if (!obj && fd > 2) {
            regs[0] = (uint64_t)(int64_t)-EBADF;
            return;
        }
        if (!obj) {
            /* dup UART fd: just pick a free fd >= minfd */
            for (int nf = minfd < 3 ? 3 : minfd; nf < (int)TASK_MAX_FD; nf++) {
                if (current->fd_table[nf] == -1) {
                    current->fd_table[nf] = -1;  /* stays as UART alias */
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
            if (current->fd_table[nf] == -1) {
                current->fd_table[nf] = (int16_t)new_idx;
                if (cmd == F_DUPFD_CLOEXEC)
                    current->fd_cloexec[nf / 8] |= (uint8_t)(1u << (nf % 8));
                else
                    current->fd_cloexec[nf / 8] &= (uint8_t)~(1u << (nf % 8));
                regs[0] = (uint64_t)nf;
                return;
            }
        }
        fd_pool_free(new_idx);
        regs[0] = (uint64_t)(int64_t)-EMFILE;
        return;
    }
    default:
        KLOG_DEBUG("[fcntl] unsupported cmd=%d fd=%d\n", cmd, fd);
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
    if (!user_fds) {
        regs[0] = (uint64_t)(int64_t)-EFAULT;
        return;
    }
    int fds[2];
    int rc = pipe_alloc(current, fds);
    if (rc < 0) {
        regs[0] = (uint64_t)(int64_t)-EMFILE;
        return;
    }
    user_fds[0] = fds[0];
    user_fds[1] = fds[1];
    regs[0] = 0;
}
