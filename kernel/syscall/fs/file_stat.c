/*
 * fs/file_stat.c - fstat / newfstatat / readlinkat / faccessat / getdents64
 *
 * 由 kernel/syscall/syscall.c 拆出。
 */
#include "syscall/syscall_internal.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/fs/path.h"
#include "task/task.h"
#include "kernel_stat.h"
#include "string.h"
#include "klog.h"
#include "vfs.h"
#include <ext4.h>
#include <ext4_errno.h>

static bool trace_heavy_task(task_t *current)
{
    return current && current->is_user_process && current->heap_end >= 0x3000000ULL;
}

void fstat_handler(uint64_t regs[6], task_t *current)
{
    int fd = (int)regs[0];
    struct kernel_stat *st = (struct kernel_stat *)regs[1];
    if (!st) { regs[0] = (uint64_t)(int64_t)-EFAULT; return; }

    fd_obj_t *obj = task_get_fd(current, fd);
    if (trace_heavy_task(current)) {
        KLOG_DEBUG("[vfsstat] pid=%u fstat fd=%d st=0x%llx path=%s kind=%d\n",
                   current->id, fd, (uint64_t)st,
                   (obj && fd_obj_file(obj)) ? fd_obj_file(obj)->path : "<none>",
                   (obj && fd_obj_file(obj)) ? fd_obj_file(obj)->kind : -1);
    }
    if (fd == 0 || fd == 1 || fd == 2 || (obj && !fd_obj_file(obj))) {
        memset(st, 0, sizeof(*st));
        st->st_mode = 0020666;  /* character device */
        st->st_rdev = (5 << 8) | (fd == 0 ? 0 : 1);
        regs[0] = 0;
        return;
    }
    if (!obj) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }
    int rc = vfs_stat_file(fd_obj_file(obj), st);
    if (rc < 0) { regs[0] = (uint64_t)(int64_t)rc; return; }
    regs[0] = 0;
}

void newfstatat_handler(uint64_t regs[6], task_t *current)
{
    int dirfd = (int)regs[0];
    const char         *pathname = (const char *)regs[1];
    struct kernel_stat *st       = (struct kernel_stat *)regs[2];
    if (!st) { regs[0] = (uint64_t)(int64_t)-EFAULT; return; }
    if (!pathname || pathname[0] == '\0') {
        fd_obj_t *obj = task_get_fd(current, dirfd);
        if (trace_heavy_task(current)) {
            KLOG_DEBUG("[vfsstat] pid=%u newfstatat empty dirfd=%d st=0x%llx path=%s kind=%d\n",
                       current->id, dirfd, (uint64_t)st,
                       (obj && fd_obj_file(obj)) ? fd_obj_file(obj)->path : "<none>",
                       (obj && fd_obj_file(obj)) ? fd_obj_file(obj)->kind : -1);
        }
        if (!obj) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }
        int rc = vfs_stat_file(fd_obj_file(obj), st);
        if (rc < 0) { regs[0] = (uint64_t)(int64_t)rc; return; }
        regs[0] = 0;
        return;
    }
    char abspath[128];
    int rpa = resolve_path_at(current, dirfd, pathname, abspath, sizeof(abspath));
    if (rpa < 0) {
        if (trace_heavy_task(current)) {
            KLOG_DEBUG("[vfsstat] pid=%u newfstatat path=%s st=0x%llx rc=%d\n",
                       current->id, pathname, (uint64_t)st, rpa);
        }
        regs[0] = (uint64_t)(int64_t)rpa;
        return;
    }
    /* AT_SYMLINK_NOFOLLOW = 0x100 */
    if (!(regs[3] & 0x100))
        follow_symlinks(abspath, sizeof(abspath));
    if (trace_heavy_task(current)) {
        KLOG_DEBUG("[vfsstat] pid=%u newfstatat path=%s resolved=%s st=0x%llx flags=0x%llx\n",
                   current->id, pathname, abspath, (uint64_t)st, regs[3]);
    }

    if (vfs_stat_path(abspath, st) == 0) { regs[0] = 0; return; }
    int pts_idx = pty_match_pts_path(abspath);
    if (pts_idx >= 0) {
        memset(st, 0, sizeof(*st));
        st->st_dev = 5;
        st->st_ino = 256U + (uint32_t)pts_idx;
        st->st_mode = 0020620;
        st->st_nlink = 1;
        st->st_rdev = ((uint64_t)136 << 8) | (uint32_t)pts_idx;
        st->st_blksize = 4096;
        regs[0] = 0;
        return;
    }
    if (strcmp(abspath, "/dev/ptmx") == 0) {
        memset(st, 0, sizeof(*st));
        st->st_dev = 5;
        st->st_ino = 128U;
        st->st_mode = 0020620;
        st->st_nlink = 1;
        st->st_rdev = ((uint64_t)5 << 8) | 2;
        st->st_blksize = 4096;
        regs[0] = 0;
        return;
    }
    int rc = fill_stat_from_ext4(st, abspath);
    if (rc < 0) { regs[0] = (uint64_t)(int64_t)rc; return; }
    regs[0] = 0;
}

static int proc_self_fd_readlink(task_t *current, int fdnum,
                                 char *buf, size_t bufsz)
{
    fd_obj_t *obj = task_get_fd(current, fdnum);
    if (!obj) return -ENOENT;

    return vfs_proc_fd_target(fd_obj_file(obj), buf, bufsz);
}

void readlinkat_handler(uint64_t regs[6], task_t *current)
{
    int dirfd = (int)regs[0];
    const char *pathname = (const char *)regs[1];
    char       *lbuf     = (char *)regs[2];
    uint64_t    lbufsz   = regs[3];
    if (!pathname || !lbuf) { regs[0] = (uint64_t)(int64_t)-EFAULT; return; }
    char abspath[128];
    resolve_path_at(current, dirfd, pathname, abspath, sizeof(abspath));
    if (trace_heavy_task(current)) {
        KLOG_DEBUG("[vfsstat] pid=%u readlinkat path=%s resolved=%s buf=0x%llx size=0x%llx\n",
                   current->id, pathname, abspath, (uint64_t)lbuf, lbufsz);
    }

    /* /proc/self/fd/N — resolve from task fd table */
    if (strncmp(abspath, "/proc/self/fd/", 14) == 0) {
        const char *p = abspath + 14;
        int fdnum = 0;
        while (*p >= '0' && *p <= '9')
            fdnum = fdnum * 10 + (*p++ - '0');
        if (*p == '\0') {
            int rc = proc_self_fd_readlink(current, fdnum, lbuf, (size_t)lbufsz);
            regs[0] = rc >= 0 ? (uint64_t)rc : (uint64_t)(int64_t)rc;
            return;
        }
    }

    int rc = vfs_readlink(abspath, lbuf, (size_t)lbufsz);
    regs[0] = rc >= 0 ? (uint64_t)rc : (uint64_t)(int64_t)-ENOENT;
}

void faccessat_handler(uint64_t regs[6], task_t *current)
{
    int dirfd = (int)regs[0];
    const char *pathname = (const char *)regs[1];
    (void)regs[2]; /* mode */
    (void)regs[3]; /* flags */

    if (!pathname) {
        regs[0] = (uint64_t)(int64_t)-EFAULT;
        return;
    }

    char abspath[128];
    int rpa = resolve_path_at(current, dirfd, pathname, abspath, sizeof(abspath));
    if (rpa < 0) {
        regs[0] = (uint64_t)(int64_t)rpa;
        return;
    }
    follow_symlinks(abspath, sizeof(abspath));

    struct kernel_stat _tmpst;
    if (vfs_stat_path(abspath, &_tmpst) == 0) { regs[0] = 0; return; }

    int rc = ext4_inode_exist(abspath, EXT4_DE_UNKNOWN);
    regs[0] = (rc == EOK) ? 0 : (uint64_t)(int64_t)-ENOENT;
}

void getdents64_handler(uint64_t regs[6], task_t *current)
{
    int fd   = (int)regs[0];
    char *buf = (char *)regs[1];
    uint64_t count = regs[2];
    if (!buf || count < 32) { regs[0] = (uint64_t)(int64_t)-EINVAL; return; }

    fd_obj_t *obj = task_get_fd(current, fd);
    if (!obj) { regs[0] = (uint64_t)(int64_t)-ENOTDIR; return; }

    if (fd_obj_file(obj)) {
        int rc = vfs_getdents(fd_obj_file(obj), buf, (size_t)count);
        regs[0] = rc >= 0 ? (uint64_t)rc : (uint64_t)(int64_t)rc;
        return;
    }
    regs[0] = (uint64_t)(int64_t)-ENOTDIR;
}
