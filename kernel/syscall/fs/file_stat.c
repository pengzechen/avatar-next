/*
 * fs/file_stat.c - fstat / newfstatat / readlinkat / faccessat / getdents64
 *
 * 由 kernel/syscall/syscall.c 拆出。
 */
#include "syscall/syscall_internal.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/fs/path.h"
#include "task/task.h"
#include "pseudofs.h"
#include "kernel_stat.h"
#include "string.h"
#include <ext4.h>
#include <ext4_errno.h>

static void fill_pty_stat(struct kernel_stat *st, int pty_idx, bool is_master)
{
    memset(st, 0, sizeof(*st));
    st->st_dev = 5;
    st->st_ino = (uint64_t)(is_master ? 128U : 256U + (uint32_t)pty_idx);
    st->st_mode = 0020620;  /* S_IFCHR | 0620 */
    st->st_nlink = 1;
    st->st_rdev = is_master ? ((uint64_t)5 << 8) | 2 : ((uint64_t)136 << 8) | (uint32_t)pty_idx;
    st->st_blksize = 4096;
}

void fstat_handler(uint64_t regs[6], task_t *current)
{
    int fd = (int)regs[0];
    struct kernel_stat *st = (struct kernel_stat *)regs[1];
    if (!st) { regs[0] = (uint64_t)(int64_t)-EFAULT; return; }

    if (fd == 0 || fd == 1 || fd == 2) {
        memset(st, 0, sizeof(*st));
        st->st_mode = 0020666;  /* character device */
        st->st_rdev = (5 << 8) | (fd == 0 ? 0 : 1);
        regs[0] = 0;
        return;
    }
    fd_obj_t *obj = task_get_fd(current, fd);
    if (!obj) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }
    if (obj->type == FDT_PSEUDO) {
        pseudo_fill_stat(obj->pseudo.node_id, st);
    } else if (obj->type == FDT_PTY) {
        fill_pty_stat(st, obj->pty.pty_idx, obj->pty.is_master);
    } else {
        int rc = fill_stat_from_ext4(st, obj->path);
        if (rc < 0) { regs[0] = (uint64_t)(int64_t)rc; return; }
    }
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
        if (!obj) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }
        if (obj->type == FDT_PSEUDO) {
            pseudo_fill_stat(obj->pseudo.node_id, st);
        } else if (obj->type == FDT_PTY) {
            fill_pty_stat(st, obj->pty.pty_idx, obj->pty.is_master);
        } else {
            int rc = fill_stat_from_ext4(st, obj->path);
            if (rc < 0) { regs[0] = (uint64_t)(int64_t)rc; return; }
        }
        regs[0] = 0;
        return;
    }
    char abspath[128];
    int rpa = resolve_path_at(current, dirfd, pathname, abspath, sizeof(abspath));
    if (rpa < 0) {
        regs[0] = (uint64_t)(int64_t)rpa;
        return;
    }
    if (pseudo_stat_path(abspath, st) == 0) { regs[0] = 0; return; }
    int pts_idx = pty_match_pts_path(abspath);
    if (pts_idx >= 0) {
        fill_pty_stat(st, pts_idx, false);
        regs[0] = 0;
        return;
    }
    if (strcmp(abspath, "/dev/ptmx") == 0) {
        fill_pty_stat(st, 0, true);
        regs[0] = 0;
        return;
    }
    /* AT_SYMLINK_NOFOLLOW = 0x100 */
    if (!(regs[3] & 0x100))
        follow_symlinks(abspath, sizeof(abspath));
    int rc = fill_stat_from_ext4(st, abspath);
    if (rc < 0) { regs[0] = (uint64_t)(int64_t)rc; return; }
    regs[0] = 0;
}

static int proc_self_fd_readlink(task_t *current, int fdnum,
                                 char *buf, size_t bufsz)
{
    fd_obj_t *obj = task_get_fd(current, fdnum);
    if (!obj) return -ENOENT;

    const char *target = NULL;
    char tmp[32];

    switch (obj->type) {
    case FDT_PTY:
        if (obj->pty.is_master) {
            target = "/dev/ptmx";
        } else {
            int n = obj->pty.pty_idx;
            tmp[0] = '/'; tmp[1] = 'd'; tmp[2] = 'e'; tmp[3] = 'v';
            tmp[4] = '/'; tmp[5] = 'p'; tmp[6] = 't'; tmp[7] = 's';
            tmp[8] = '/';
            if (n < 10) {
                tmp[9] = (char)('0' + n); tmp[10] = '\0';
            } else {
                tmp[9] = (char)('0' + n / 10);
                tmp[10] = (char)('0' + n % 10);
                tmp[11] = '\0';
            }
            target = tmp;
        }
        break;
    case FDT_PIPE:
        target = "pipe:[0]";
        break;
    case FDT_SOCKET:
        target = "socket:[0]";
        break;
    case FDT_FILE:
    case FDT_DIR:
        if (obj->path[0])
            target = obj->path;
        break;
    default:
        break;
    }

    if (!target) return -ENOENT;
    size_t len = strlen(target);
    size_t copy = len < bufsz ? len : bufsz;
    memcpy(buf, target, copy);
    return (int)copy;
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

    int rc = pseudo_readlink(abspath, lbuf, (size_t)lbufsz);
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
    if (pseudo_stat_path(abspath, &_tmpst) == 0) { regs[0] = 0; return; }

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

    if (obj->type == FDT_PSEUDO) {
        int rc = pseudo_getdents(obj->pseudo.node_id, &obj->pseudo.off,
                                 buf, (size_t)count);
        regs[0] = rc >= 0 ? (uint64_t)rc : (uint64_t)(int64_t)rc;
        return;
    }

    if (obj->type != FDT_DIR) {
        regs[0] = (uint64_t)(int64_t)-ENOTDIR;
        return;
    }

    uint64_t written = 0;
    while (written + 32 < count) {
        const ext4_direntry *de = ext4_dir_entry_next(&obj->dir);
        if (!de) break;

        uint8_t namelen = de->name_length;
        uint16_t reclen = (uint16_t)(19 + namelen + 1);
        reclen = (reclen + 7) & ~7;
        if (written + reclen > count) break;

        struct kernel_dirent64 *kd = (struct kernel_dirent64 *)(buf + written);
        kd->d_ino    = de->inode;
        kd->d_off    = (int64_t)(written + reclen);
        kd->d_reclen = reclen;
        switch (de->inode_type) {
            case EXT4_DE_REG_FILE: kd->d_type = 8; break;
            case EXT4_DE_DIR:      kd->d_type = 4; break;
            case EXT4_DE_SYMLINK:  kd->d_type = 10; break;
            default:               kd->d_type = 0; break;
        }
        for (uint8_t k = 0; k < namelen; k++)
            kd->d_name[k] = (char)de->name[k];
        kd->d_name[namelen] = '\0';
        written += reclen;
    }
    regs[0] = written;
}
