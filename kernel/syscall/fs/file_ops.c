/*
 * fs/file_ops.c - openat / close / dup3 / fcntl / pipe2
 *
 * 由 kernel/syscall/syscall.c 拆出。
 */
#include "syscall/syscall_internal.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/fs/path.h"
#include "task/task.h"
#include "pseudofs.h"
#include "klog.h"
#include "string.h"
#include "syscall/fs/pipe.h"
#include "syscall/net/ksocket.h"
#include "syscall/fs/pty.h"
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

    int pool = fd_pool_alloc();
    if (pool < 0) { regs[0] = (uint64_t)(int64_t)-EMFILE; return; }

    fd_obj_t *obj = &g_fd_pool[pool];

    /* ── pseudofs 优先 ─────────────────────────────────────── */
    int pnid = pseudo_open(abspath);
    if (pnid >= 0) {
        obj->type           = FDT_PSEUDO;
        obj->flags          = flags;
        obj->pseudo.node_id = pnid;
        obj->pseudo.off     = 0;
        int k = 0;
        while (abspath[k] && k < 127) { obj->path[k] = abspath[k]; k++; }
        obj->path[k] = '\0';
        int fd = task_alloc_fd(current, pool);
        if (fd < 0) { fd_pool_free(pool); regs[0] = (uint64_t)(int64_t)-EMFILE; return; }
        regs[0] = (uint64_t)fd;
        return;
    }

    int rc;
    if (!(flags & 0200000)) {
        rc = ext4_fopen2(&obj->file, abspath, flags);
        if (rc == EOK) {
            obj->type  = FDT_FILE;
            obj->flags = flags;
            int k = 0;
            while (abspath[k] && k < 127) { obj->path[k] = abspath[k]; k++; }
            obj->path[k] = '\0';
            int fd = task_alloc_fd(current, pool);
            if (fd < 0) { ext4_fclose(&obj->file); fd_pool_free(pool); regs[0] = (uint64_t)(int64_t)-EMFILE; return; }
            regs[0] = (uint64_t)fd;
            return;
        }
    }

    rc = ext4_dir_open(&obj->dir, abspath);
    if (rc == EOK) {
        obj->type  = FDT_DIR;
        obj->flags = flags;
        int k = 0;
        while (abspath[k] && k < 127) { obj->path[k] = abspath[k]; k++; }
        obj->path[k] = '\0';
        int fd = task_alloc_fd(current, pool);
        if (fd < 0) { ext4_dir_close(&obj->dir); fd_pool_free(pool); regs[0] = (uint64_t)(int64_t)-EMFILE; return; }
        regs[0] = (uint64_t)fd;
        return;
    }

    fd_pool_free(pool);
    if (flags & 0200000) {
        ext4_file tmp;
        if (ext4_fopen2(&tmp, abspath, 0) == EOK) {
            ext4_fclose(&tmp);
            regs[0] = (uint64_t)(int64_t)-ENOTDIR;
            return;
        }
    }
    regs[0] = (uint64_t)(int64_t)-ENOENT;
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
    if (obj->type == FDT_FILE)
        ext4_fclose(&obj->file);
    else if (obj->type == FDT_DIR)
        ext4_dir_close(&obj->dir);
    else if (obj->type == FDT_PIPE) {
        if (obj->pipe.is_write_end)
            pipe_close_write(idx);
        else
            pipe_close_read(idx);
    }
    else if (obj->type == FDT_SOCKET) {
        ksock_close(obj->sock.sock_idx);
    }
    else if (obj->type == FDT_PTY) {
        if (obj->pty.is_master)
            pty_close_master(obj->pty.pty_idx);
        else
            pty_close_slave(obj->pty.pty_idx);
    }
#if DRIVER_ION
    else if (obj->type == FDT_ION)
        ion_free((ion_handle_t)obj->ion.handle);
#endif
    fd_pool_free(idx);
    current->fd_table[fd] = -1;
    regs[0] = 0;
}

void dup3_handler(uint64_t regs[6], task_t *current)
{
    int oldfd = (int)regs[0];
    int newfd = (int)regs[1];
    int flags = (int)regs[2];
    if (oldfd == newfd) { regs[0] = (uint64_t)(int64_t)-EINVAL; return; }
    if (oldfd < 0 || oldfd >= (int)TASK_MAX_FD) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }
    if (newfd < 0 || newfd >= (int)TASK_MAX_FD) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }

    fd_obj_t *src = (oldfd > 2) ? task_get_fd(current, oldfd) : NULL;
    if (oldfd > 2 && !src) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }

    if (current->fd_table[newfd] != -1) {
        int idx = current->fd_table[newfd];
        fd_obj_t *o = &g_fd_pool[idx];
        if (o->type == FDT_FILE) ext4_fclose(&o->file);
        else if (o->type == FDT_DIR) ext4_dir_close(&o->dir);
        else if (o->type == FDT_SOCKET) ksock_close(o->sock.sock_idx);
        else if (o->type == FDT_PIPE) {
            if (o->pipe.is_write_end) pipe_close_write(idx);
            else pipe_close_read(idx);
        }
        else if (o->type == FDT_PTY) {
            if (o->pty.is_master) pty_close_master(o->pty.pty_idx);
            else pty_close_slave(o->pty.pty_idx);
        }
    #if DRIVER_ION
        else if (o->type == FDT_ION) ion_free((ion_handle_t)o->ion.handle);
    #endif
        fd_pool_free(idx);
        current->fd_table[newfd] = -1;
    }

    if (!src) {
        int uart_idx = fd_pool_alloc();
        if (uart_idx < 0) { regs[0] = (uint64_t)(int64_t)-EMFILE; return; }
        g_fd_pool[uart_idx].type  = FDT_PSEUDO;
        g_fd_pool[uart_idx].flags = 0;
        current->fd_table[newfd] = uart_idx;
    } else {
        int new_idx = fd_pool_alloc();
        if (new_idx < 0) { regs[0] = (uint64_t)(int64_t)-EMFILE; return; }
#if DRIVER_ION
        if (src->type == FDT_ION && ion_ref((ion_handle_t)src->ion.handle) != 0) {
            fd_pool_free(new_idx);
            regs[0] = (uint64_t)(int64_t)-EBADF;
            return;
        }
#endif
        g_fd_pool[new_idx] = *src;
        if (src->type == FDT_SOCKET)
            ksock_ref(src->sock.sock_idx);
        else if (src->type == FDT_PIPE) {
            if (src->pipe.is_write_end)
                pipe_ref_write(new_idx);
            else
                pipe_ref_read(new_idx);
        }
        else if (src->type == FDT_PTY) {
            if (src->pty.is_master)
                pty_ref_master(src->pty.pty_idx);
            else
                pty_ref_slave(src->pty.pty_idx);
        }
        current->fd_table[newfd] = new_idx;
    }
    if (flags & 0x80000)
        current->fd_cloexec[newfd / 8] |= (uint8_t)(1u << (newfd % 8));
    else
        current->fd_cloexec[newfd / 8] &= (uint8_t)~(1u << (newfd % 8));
    regs[0] = newfd;
}

void fcntl_handler(uint64_t regs[6], task_t *current)
{
    int fd  = (int)regs[0];
    int cmd = (int)regs[1];

    if (fd < 0 || fd >= (int)TASK_MAX_FD) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }

    /* fd 0-2 (UART) 没有 pool entry 但合法 */
    fd_obj_t *obj = NULL;
    if (fd >= 3) {
        obj = task_get_fd(current, fd);
        if (!obj) {
            regs[0] = (uint64_t)(int64_t)-EBADF;
            return;
        }
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
        if (obj)
            obj->flags = (obj->flags & ~SETFL_MASK) | (val & SETFL_MASK);
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
        if (fd <= 2) {
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
        if (obj->type == FDT_SOCKET)
            ksock_ref(obj->sock.sock_idx);
        else if (obj->type == FDT_PIPE) {
            if (obj->pipe.is_write_end)
                pipe_ref_write(new_idx);
            else
                pipe_ref_read(new_idx);
        }
        else if (obj->type == FDT_PTY && !obj->pty.is_master)
            pty_ref_slave(obj->pty.pty_idx);
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
