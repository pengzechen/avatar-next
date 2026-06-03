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

    int rc = ext4_fopen2(&obj->file, abspath, flags);
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
    if (oldfd == newfd) { regs[0] = newfd; return; }
    if (oldfd < 0 || oldfd >= (int)TASK_MAX_FD) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }
    if (newfd < 0 || newfd >= (int)TASK_MAX_FD) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }

    fd_obj_t *src = (oldfd > 2) ? task_get_fd(current, oldfd) : NULL;
    if (oldfd > 2 && !src) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }

    if (current->fd_table[newfd] != -1) {
        int idx = current->fd_table[newfd];
        fd_obj_t *o = &g_fd_pool[idx];
        if (o->type == FDT_FILE) ext4_fclose(&o->file);
        else if (o->type == FDT_DIR) ext4_dir_close(&o->dir);
    #if DRIVER_ION
        else if (o->type == FDT_ION) ion_free((ion_handle_t)o->ion.handle);
    #endif
        fd_pool_free(idx);
        current->fd_table[newfd] = -1;
    }

    if (!src) {
        current->fd_table[newfd] = -1;
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
        current->fd_table[newfd] = new_idx;
    }
    regs[0] = newfd;
}

void fcntl_handler(uint64_t regs[6])
{
    regs[0] = 0;
}

void pipe2_handler(uint64_t regs[6])
{
    regs[0] = (uint64_t)(int64_t)-ENOSYS;
}
