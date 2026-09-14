/*
 * kernel/fs/vfs/vfs.c - small VFS dispatch layer
 */

#include "vfs.h"
#include "pseudofs.h"
#include "syscall/fs/path.h"
#include "syscall/fs/pipe.h"
#include "syscall/fs/pty.h"
#include "syscall/io/epoll.h"
#include "syscall/net/ksocket.h"
#include "kmalloc.h"
#include "string.h"
#include <ext4_errno.h>

#define VFS_ENOENT   2
#define VFS_EIO      5
#define VFS_EBADF    9
#define VFS_EINVAL  22
#define VFS_ENOTDIR 20
#define VFS_EMFILE  24
#define VFS_ENOSYS  38
#define VFS_ESPIPE  29

#define O_ACCMODE_AVATAR 0000003
#define O_WRONLY_AVATAR  0000001
#define O_RDWR_AVATAR    0000002
#define O_CREAT_AVATAR   0000100
#define O_TRUNC_AVATAR   0001000
#define O_APPEND_AVATAR  0002000
#define O_DIRECTORY_AVATAR 0200000
#define SEEK_SET_AVATAR 0
#define SEEK_CUR_AVATAR 1
#define SEEK_END_AVATAR 2

typedef struct vfs_mount vfs_mount_t;

typedef struct {
    int (*open)(const vfs_mount_t *mnt, const char *path, int flags,
                int mode, vfs_file_t **out);
    int (*stat)(const vfs_mount_t *mnt, const char *path,
                struct kernel_stat *st);
    int (*readlink)(const vfs_mount_t *mnt, const char *path,
                    char *buf, size_t bufsz);
} vfs_mount_ops_t;

struct vfs_mount {
    const char *path;
    const vfs_mount_ops_t *ops;
};

static int pseudo_mount_open(const vfs_mount_t *mnt, const char *path,
                             int flags, int mode, vfs_file_t **out);
static int pseudo_mount_stat(const vfs_mount_t *mnt, const char *path,
                             struct kernel_stat *st);
static int pseudo_mount_readlink(const vfs_mount_t *mnt, const char *path,
                                 char *buf, size_t bufsz);
static int ext4_mount_open(const vfs_mount_t *mnt, const char *path,
                           int flags, int mode, vfs_file_t **out);
static int ext4_mount_stat(const vfs_mount_t *mnt, const char *path,
                           struct kernel_stat *st);
static int ext4_mount_readlink(const vfs_mount_t *mnt, const char *path,
                               char *buf, size_t bufsz);

static const vfs_mount_ops_t pseudo_mount_ops = {
    .open = pseudo_mount_open,
    .stat = pseudo_mount_stat,
    .readlink = pseudo_mount_readlink,
};

static const vfs_mount_ops_t ext4_mount_ops = {
    .open = ext4_mount_open,
    .stat = ext4_mount_stat,
    .readlink = ext4_mount_readlink,
};

static const vfs_mount_t g_mounts[] = {
    { "/dev",  &pseudo_mount_ops },
    { "/proc", &pseudo_mount_ops },
    { "/sys",  &pseudo_mount_ops },
    { "/",     &ext4_mount_ops   },
};

static bool mount_path_matches(const char *mnt, const char *path)
{
    size_t len = strlen(mnt);

    if (len == 1 && mnt[0] == '/')
        return path && path[0] == '/';
    if (!path || strncmp(path, mnt, len) != 0)
        return false;
    return path[len] == '\0' || path[len] == '/';
}

static const vfs_mount_t *vfs_find_mount(const char *path)
{
    const vfs_mount_t *best = NULL;
    size_t best_len = 0;

    for (uint32_t i = 0; i < sizeof(g_mounts) / sizeof(g_mounts[0]); i++) {
        const char *mnt_path = g_mounts[i].path;
        size_t len = strlen(mnt_path);
        if (len >= best_len && mount_path_matches(mnt_path, path)) {
            best = &g_mounts[i];
            best_len = len;
        }
    }
    return best;
}

static void vfs_copy_path(vfs_file_t *file, const char *path)
{
    int i = 0;
    while (path && path[i] && i < (int)sizeof(file->path) - 1) {
        file->path[i] = path[i];
        i++;
    }
    file->path[i] = '\0';
}

static vfs_file_t *vfs_alloc_file(void)
{
    vfs_file_t *file = (vfs_file_t *)kmalloc(sizeof(*file));
    if (!file)
        return NULL;
    memset(file, 0, sizeof(*file));
    file->refcnt = 1;
    return file;
}

static int ext4_open_flags_from_linux(int flags)
{
    int ext4_flags = 0;

    switch (flags & O_ACCMODE_AVATAR) {
    case O_WRONLY_AVATAR:
        ext4_flags |= O_WRONLY;
        break;
    case O_RDWR_AVATAR:
        ext4_flags |= O_RDWR;
        break;
    default:
        ext4_flags |= O_RDONLY;
        break;
    }

    if (flags & O_CREAT_AVATAR)
        ext4_flags |= O_CREAT;
    if (flags & O_TRUNC_AVATAR)
        ext4_flags |= O_TRUNC;
    if (flags & O_APPEND_AVATAR)
        ext4_flags |= O_APPEND;

    return ext4_flags;
}

static int pseudo_file_read(vfs_file_t *file, void *buf, size_t len)
{
    return pseudo_read(file->u.pseudo.node_id, &file->offset, buf, len);
}

static int pseudo_file_write(vfs_file_t *file, const void *buf, size_t len)
{
    return pseudo_write(file->u.pseudo.node_id, buf, len);
}

static int pseudo_file_ioctl(vfs_file_t *file, uint64_t req, void *argp)
{
    return pseudo_ioctl(file->u.pseudo.node_id, req, argp);
}

static int pseudo_file_getdents(vfs_file_t *file, void *buf, size_t bufsz)
{
    return pseudo_getdents(file->u.pseudo.node_id, &file->offset, buf, bufsz);
}

static int pseudo_file_seek(vfs_file_t *file, int64_t offset, int whence,
                            uint64_t *new_off)
{
    if (!new_off)
        return -VFS_EINVAL;
    if (whence == SEEK_SET_AVATAR) {
        if (offset < 0)
            return -VFS_EINVAL;
        file->offset = (uint64_t)offset;
    } else if (whence == SEEK_CUR_AVATAR) {
        int64_t cur = (int64_t)file->offset;
        if (offset < 0 && cur < -offset)
            return -VFS_EINVAL;
        file->offset = (uint64_t)(cur + offset);
    } else if (whence == SEEK_END_AVATAR) {
        if (offset < 0)
            return -VFS_EINVAL;
        file->offset = (uint64_t)offset;
    } else {
        return -VFS_EINVAL;
    }
    *new_off = file->offset;
    return 0;
}

static int pseudo_file_stat(vfs_file_t *file, struct kernel_stat *st)
{
    pseudo_fill_stat(file->u.pseudo.node_id, st);
    return 0;
}

static int ext4_file_read(vfs_file_t *file, void *buf, size_t len)
{
    size_t rcnt = 0;
    int rc = ext4_fread(&file->u.ext4_file, buf, len, &rcnt);
    return rc == EOK ? (int)rcnt : -VFS_EIO;
}

static int ext4_file_write(vfs_file_t *file, const void *buf, size_t len)
{
    size_t wcnt = 0;
    int rc = ext4_fwrite(&file->u.ext4_file, buf, len, &wcnt);
    return rc == EOK ? (int)wcnt : -VFS_EIO;
}

static int ext4_file_seek(vfs_file_t *file, int64_t offset, int whence,
                          uint64_t *new_off)
{
    if (!new_off)
        return -VFS_EINVAL;
    if (whence == SEEK_END_AVATAR) {
        int64_t fsize = (int64_t)ext4_fsize(&file->u.ext4_file);
        int64_t target = fsize + offset;
        if (target < 0)
            return -VFS_EINVAL;
        offset = target;
        whence = SEEK_SET_AVATAR;
    }
    int rc = ext4_fseek(&file->u.ext4_file, offset, (uint32_t)whence);
    if (rc == EOK) {
        *new_off = (uint64_t)ext4_ftell(&file->u.ext4_file);
        file->offset = *new_off;
        return 0;
    }
    if (whence == SEEK_SET_AVATAR && offset >= 0) {
        file->u.ext4_file.fpos = (uint64_t)offset;
        file->offset = (uint64_t)offset;
        *new_off = file->offset;
        return 0;
    }
    return -VFS_EINVAL;
}

static int ext4_file_stat(vfs_file_t *file, struct kernel_stat *st)
{
    return fill_stat_from_ext4(st, file->path);
}

static int ext4_file_truncate(vfs_file_t *file, uint64_t length)
{
    int rc = ext4_ftruncate(&file->u.ext4_file, length);
    return rc == EOK ? 0 : -rc;
}

static int ext4_regular_close(vfs_file_t *file)
{
    return ext4_fclose(&file->u.ext4_file) == EOK ? 0 : -VFS_EIO;
}

static int ext4_dir_getdents(vfs_file_t *file, void *buf, size_t bufsz)
{
    uint64_t written = 0;
    char *dst = (char *)buf;

    while (written + 32 < bufsz) {
        const ext4_direntry *de = ext4_dir_entry_next(&file->u.ext4_dir);
        if (!de)
            break;

        uint8_t namelen = de->name_length;
        uint16_t reclen = (uint16_t)(19 + namelen + 1);
        reclen = (reclen + 7) & ~7;
        if (written + reclen > bufsz)
            break;

        struct kernel_dirent64 *kd = (struct kernel_dirent64 *)(dst + written);
        kd->d_ino = de->inode;
        kd->d_off = (int64_t)(written + reclen);
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
        file->offset++;
    }
    return (int)written;
}

static int ext4_dir_stat_file(vfs_file_t *file, struct kernel_stat *st)
{
    return fill_stat_from_ext4(st, file->path);
}

static int ext4_dir_file_close(vfs_file_t *file)
{
    return ext4_dir_close(&file->u.ext4_dir) == EOK ? 0 : -VFS_EIO;
}

static int pipe_file_read(vfs_file_t *file, void *buf, size_t len)
{
    if (file->u.pipe.is_write_end)
        return -VFS_EBADF;
    return pipe_read_endpoint(file->u.pipe.pipe_idx, buf, len);
}

static int pipe_file_write(vfs_file_t *file, const void *buf, size_t len)
{
    if (!file->u.pipe.is_write_end)
        return -VFS_EBADF;
    return pipe_write_endpoint(file->u.pipe.pipe_idx, buf, len);
}

static int pipe_file_close(vfs_file_t *file)
{
    pipe_close_endpoint(file->u.pipe.pipe_idx, file->u.pipe.is_write_end);
    return 0;
}

static uint32_t pipe_file_poll(vfs_file_t *file, int pool_idx)
{
    (void)pool_idx;
    uint32_t revents = 0;
    if (file->u.pipe.is_write_end) {
        if (pipe_poll_writable_endpoint(file->u.pipe.pipe_idx))
            revents |= EPOLLOUT;
    } else {
        if (pipe_poll_readable_endpoint(file->u.pipe.pipe_idx))
            revents |= EPOLLIN;
    }
    return revents;
}

static int pty_file_read(vfs_file_t *file, void *buf, size_t len)
{
    bool nonblock = (file->flags & 04000) != 0;
    if (file->u.pty.is_master)
        return pty_master_read(file->u.pty.pty_idx, buf, len, nonblock);
    return pty_slave_read(file->u.pty.pty_idx, buf, len, nonblock);
}

static int pty_file_write(vfs_file_t *file, const void *buf, size_t len)
{
    bool nonblock = (file->flags & 04000) != 0;
    if (file->u.pty.is_master)
        return pty_master_write(file->u.pty.pty_idx, buf, len, nonblock);
    return pty_slave_write(file->u.pty.pty_idx, buf, len, nonblock);
}

static int pty_file_ioctl(vfs_file_t *file, uint64_t req, void *argp)
{
    return pty_ioctl(file->u.pty.pty_idx, file->u.pty.is_master,
                     (uint32_t)req, argp);
}

static int pty_file_stat(vfs_file_t *file, struct kernel_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_dev = 5;
    st->st_ino = file->u.pty.is_master ? 128U : 256U + (uint32_t)file->u.pty.pty_idx;
    st->st_mode = 0020620;
    st->st_nlink = 1;
    st->st_rdev = file->u.pty.is_master ? ((uint64_t)5 << 8) | 2 :
                                          ((uint64_t)136 << 8) | (uint32_t)file->u.pty.pty_idx;
    st->st_blksize = 4096;
    return 0;
}

static uint32_t pty_file_poll(vfs_file_t *file, int pool_idx)
{
    (void)pool_idx;
    uint32_t revents = 0;
    if (file->u.pty.is_master) {
        if (pty_poll_readable_master(file->u.pty.pty_idx))
            revents |= EPOLLIN;
    } else {
        if (pty_poll_readable_slave(file->u.pty.pty_idx))
            revents |= EPOLLIN;
    }
    if (pty_poll_writable(file->u.pty.pty_idx))
        revents |= EPOLLOUT;
    return revents;
}

static int pty_file_close(vfs_file_t *file)
{
    if (file->u.pty.is_master)
        pty_close_master(file->u.pty.pty_idx);
    else
        pty_close_slave(file->u.pty.pty_idx);
    return 0;
}

static int socket_file_read(vfs_file_t *file, void *buf, size_t len)
{
    int flags = (file->flags & 04000) ? KSOCK_MSG_DONTWAIT : 0;
    return ksock_recv(file->u.sock.sock_idx, buf, len, flags);
}

static int socket_file_write(vfs_file_t *file, const void *buf, size_t len)
{
    int flags = (file->flags & 04000) ? KSOCK_MSG_DONTWAIT : 0;
    return ksock_send(file->u.sock.sock_idx, buf, len, flags);
}

static int socket_file_close(vfs_file_t *file)
{
    return ksock_close(file->u.sock.sock_idx);
}

static uint32_t socket_file_poll(vfs_file_t *file, int pool_idx)
{
    (void)pool_idx;
    uint32_t revents = 0;
    if (ksock_poll_readable(file->u.sock.sock_idx))
        revents |= EPOLLIN;
    if (ksock_poll_writable(file->u.sock.sock_idx))
        revents |= EPOLLOUT;
    return revents;
}

static const vfs_file_ops_t pseudo_ops = {
    .read = pseudo_file_read,
    .write = pseudo_file_write,
    .ioctl = pseudo_file_ioctl,
    .getdents = pseudo_file_getdents,
    .seek = pseudo_file_seek,
    .stat = pseudo_file_stat,
};

static const vfs_file_ops_t ext4_file_ops = {
    .read = ext4_file_read,
    .write = ext4_file_write,
    .seek = ext4_file_seek,
    .stat = ext4_file_stat,
    .truncate = ext4_file_truncate,
    .close = ext4_regular_close,
};

static const vfs_file_ops_t ext4_dir_ops = {
    .getdents = ext4_dir_getdents,
    .stat = ext4_dir_stat_file,
    .close = ext4_dir_file_close,
};

static const vfs_file_ops_t pipe_ops = {
    .read = pipe_file_read,
    .write = pipe_file_write,
    .poll = pipe_file_poll,
    .close = pipe_file_close,
};

static const vfs_file_ops_t pty_ops = {
    .read = pty_file_read,
    .write = pty_file_write,
    .ioctl = pty_file_ioctl,
    .stat = pty_file_stat,
    .poll = pty_file_poll,
    .close = pty_file_close,
};

static const vfs_file_ops_t socket_ops = {
    .read = socket_file_read,
    .write = socket_file_write,
    .poll = socket_file_poll,
    .close = socket_file_close,
};

static int pseudo_mount_open(const vfs_mount_t *mnt, const char *path,
                             int flags, int mode, vfs_file_t **out)
{
    (void)mnt;
    (void)mode;
    int pnid = pseudo_open(path);
    if (pnid < 0)
        return -VFS_ENOENT;

    vfs_file_t *file = vfs_alloc_file();
    if (!file)
        return -VFS_EMFILE;
    file->kind = VFS_FILE_PSEUDO;
    file->flags = flags;
    file->ops = &pseudo_ops;
    file->u.pseudo.node_id = pnid;
    vfs_copy_path(file, path);
    *out = file;
    return 0;
}

static int ext4_mount_open(const vfs_mount_t *mnt, const char *path,
                           int flags, int mode, vfs_file_t **out)
{
    (void)mnt;
    (void)mode;
    int ext4_flags = ext4_open_flags_from_linux(flags);

    if (!(flags & O_DIRECTORY_AVATAR)) {
        vfs_file_t *file = vfs_alloc_file();
        if (!file)
            return -VFS_EMFILE;
        if (ext4_fopen2(&file->u.ext4_file, path, ext4_flags) == EOK) {
            file->kind = VFS_FILE_EXT4_FILE;
            file->flags = flags;
            file->ops = &ext4_file_ops;
            vfs_copy_path(file, path);
            *out = file;
            return 0;
        }
        kfree(file, sizeof(*file));
    }

    vfs_file_t *dir = vfs_alloc_file();
    if (!dir)
        return -VFS_EMFILE;
    if (ext4_dir_open(&dir->u.ext4_dir, path) == EOK) {
        uint32_t mode_bits = 0;
        if (ext4_mode_get(path, &mode_bits) == EOK &&
            (mode_bits & 0170000) != 0040000) {
            ext4_dir_close(&dir->u.ext4_dir);
            kfree(dir, sizeof(*dir));
            return -VFS_ENOTDIR;
        }
        dir->kind = VFS_FILE_EXT4_DIR;
        dir->flags = flags;
        dir->ops = &ext4_dir_ops;
        vfs_copy_path(dir, path);
        *out = dir;
        return 0;
    }
    kfree(dir, sizeof(*dir));

    if (flags & O_DIRECTORY_AVATAR) {
        ext4_file tmp;
        if (ext4_fopen2(&tmp, path, 0) == EOK) {
            ext4_fclose(&tmp);
            return -VFS_ENOTDIR;
        }
    }
    return -VFS_ENOENT;
}

int vfs_open(const char *path, int flags, int mode, vfs_file_t **out)
{
    if (!path || !out)
        return -VFS_EINVAL;
    *out = NULL;

    const vfs_mount_t *mnt = vfs_find_mount(path);
    if (!mnt || !mnt->ops || !mnt->ops->open)
        return -VFS_ENOENT;
    return mnt->ops->open(mnt, path, flags, mode, out);
}

int vfs_create_pipe_file(int pipe_idx, bool is_write_end, int flags,
                         vfs_file_t **out)
{
    if (!out)
        return -VFS_EINVAL;
    *out = NULL;
    vfs_file_t *file = vfs_alloc_file();
    if (!file)
        return -VFS_EMFILE;
    file->kind = VFS_FILE_PIPE;
    file->flags = flags;
    file->ops = &pipe_ops;
    file->u.pipe.pipe_idx = (int16_t)pipe_idx;
    file->u.pipe.is_write_end = is_write_end;
    *out = file;
    return 0;
}

int vfs_create_pty_file(int pty_idx, bool is_master, int flags,
                        const char *path, vfs_file_t **out)
{
    if (!out)
        return -VFS_EINVAL;
    *out = NULL;
    vfs_file_t *file = vfs_alloc_file();
    if (!file)
        return -VFS_EMFILE;
    file->kind = VFS_FILE_PTY;
    file->flags = flags;
    file->ops = &pty_ops;
    file->u.pty.pty_idx = (int16_t)pty_idx;
    file->u.pty.is_master = is_master;
    vfs_copy_path(file, path);
    *out = file;
    return 0;
}

int vfs_create_socket_file(int sock_idx, int flags, vfs_file_t **out)
{
    if (!out)
        return -VFS_EINVAL;
    *out = NULL;
    vfs_file_t *file = vfs_alloc_file();
    if (!file)
        return -VFS_EMFILE;
    file->kind = VFS_FILE_SOCKET;
    file->flags = flags;
    file->ops = &socket_ops;
    file->u.sock.sock_idx = (int16_t)sock_idx;
    *out = file;
    return 0;
}

void vfs_ref(vfs_file_t *file)
{
    if (file)
        file->refcnt++;
}

int vfs_close(vfs_file_t *file)
{
    int rc = 0;
    if (!file)
        return 0;
    if (file->refcnt > 1) {
        file->refcnt--;
        return 0;
    }
    if (file->ops && file->ops->close)
        rc = file->ops->close(file);
    kfree(file, sizeof(*file));
    return rc;
}

void vfs_discard_unopened(vfs_file_t *file)
{
    if (file)
        kfree(file, sizeof(*file));
}

int vfs_read(vfs_file_t *file, void *buf, size_t len)
{
    if (!file || !file->ops || !file->ops->read)
        return -VFS_EBADF;
    return file->ops->read(file, buf, len);
}

int vfs_write(vfs_file_t *file, const void *buf, size_t len)
{
    if (!file || !file->ops || !file->ops->write)
        return -VFS_EBADF;
    return file->ops->write(file, buf, len);
}

int vfs_ioctl(vfs_file_t *file, uint64_t req, void *argp)
{
    if (!file || !file->ops || !file->ops->ioctl)
        return -VFS_ENOSYS;
    return file->ops->ioctl(file, req, argp);
}

int vfs_getdents(vfs_file_t *file, void *buf, size_t bufsz)
{
    if (!file || !file->ops || !file->ops->getdents)
        return -VFS_ENOTDIR;
    return file->ops->getdents(file, buf, bufsz);
}

int vfs_seek(vfs_file_t *file, int64_t offset, int whence, uint64_t *new_off)
{
    if (!file || !file->ops || !file->ops->seek)
        return -VFS_ESPIPE;
    return file->ops->seek(file, offset, whence, new_off);
}

int vfs_stat_file(vfs_file_t *file, struct kernel_stat *st)
{
    if (!file || !st || !file->ops || !file->ops->stat)
        return -VFS_EBADF;
    return file->ops->stat(file, st);
}

uint32_t vfs_poll(vfs_file_t *file, int pool_idx)
{
    if (!file)
        return 0;
    if (file->ops && file->ops->poll)
        return file->ops->poll(file, pool_idx);
    return EPOLLIN | EPOLLOUT;
}

int vfs_pread(vfs_file_t *file, void *buf, size_t len, int64_t offset)
{
    if (!vfs_file_is_regular(file) || !buf)
        return -VFS_EBADF;
    if (offset < 0)
        return -VFS_EINVAL;
    int64_t saved = ext4_ftell(&file->u.ext4_file);
    if (saved < 0 || ext4_fseek(&file->u.ext4_file, offset, SEEK_SET_AVATAR) != EOK)
        return -VFS_EIO;
    size_t rcnt = 0;
    int rc = ext4_fread(&file->u.ext4_file, buf, len, &rcnt);
    int seek_rc = ext4_fseek(&file->u.ext4_file, saved, SEEK_SET_AVATAR);
    return rc == EOK && seek_rc == EOK ? (int)rcnt : -VFS_EIO;
}

int vfs_pwrite(vfs_file_t *file, const void *buf, size_t len, int64_t offset)
{
    if (!vfs_file_is_regular(file) || !buf)
        return -VFS_EBADF;
    if (offset < 0)
        return -VFS_EINVAL;
    int64_t saved = ext4_ftell(&file->u.ext4_file);
    if (saved < 0 || ext4_fseek(&file->u.ext4_file, offset, SEEK_SET_AVATAR) != EOK)
        return -VFS_EIO;
    size_t wcnt = 0;
    int rc = ext4_fwrite(&file->u.ext4_file, buf, len, &wcnt);
    int seek_rc = ext4_fseek(&file->u.ext4_file, saved, SEEK_SET_AVATAR);
    return rc == EOK && seek_rc == EOK ? (int)wcnt : -VFS_EIO;
}

int vfs_truncate(vfs_file_t *file, uint64_t length)
{
    if (!file || !file->ops || !file->ops->truncate)
        return -VFS_EBADF;
    return file->ops->truncate(file, length);
}

int vfs_socket_index(vfs_file_t *file)
{
    if (!file || file->kind != VFS_FILE_SOCKET)
        return -1;
    return file->u.sock.sock_idx;
}

bool vfs_file_matches_socket(vfs_file_t *file, int sock_idx)
{
    return file && file->kind == VFS_FILE_SOCKET && file->u.sock.sock_idx == sock_idx;
}

bool vfs_file_matches_pty(vfs_file_t *file, int pty_idx, bool is_master)
{
    return file && file->kind == VFS_FILE_PTY &&
           file->u.pty.pty_idx == pty_idx && file->u.pty.is_master == is_master;
}

int vfs_proc_fd_target(vfs_file_t *file, char *buf, size_t bufsz)
{
    if (!file || !buf)
        return -VFS_EBADF;
    const char *target = NULL;
    char tmp[32];

    if (file->kind == VFS_FILE_PTY) {
        if (file->u.pty.is_master) {
            target = "/dev/ptmx";
        } else {
            int n = file->u.pty.pty_idx;
            tmp[0] = '/'; tmp[1] = 'd'; tmp[2] = 'e'; tmp[3] = 'v';
            tmp[4] = '/'; tmp[5] = 'p'; tmp[6] = 't'; tmp[7] = 's'; tmp[8] = '/';
            if (n < 10) {
                tmp[9] = (char)('0' + n); tmp[10] = '\0';
            } else {
                tmp[9] = (char)('0' + n / 10);
                tmp[10] = (char)('0' + n % 10);
                tmp[11] = '\0';
            }
            target = tmp;
        }
    } else if (file->kind == VFS_FILE_PIPE) {
        target = "pipe:[0]";
    } else if (file->kind == VFS_FILE_SOCKET) {
        target = "socket:[0]";
    } else if (file->path[0]) {
        target = file->path;
    }

    if (!target)
        return -VFS_ENOENT;
    size_t len = strlen(target);
    size_t copy = len < bufsz ? len : bufsz;
    memcpy(buf, target, copy);
    return (int)copy;
}

int vfs_read_to_phys(vfs_file_t *file, uint64_t offset, void *buf, size_t len)
{
    if (!vfs_file_is_regular(file) || !buf)
        return -VFS_EBADF;
    if (ext4_fseek(&file->u.ext4_file, (int64_t)offset, SEEK_SET_AVATAR) != EOK)
        return -VFS_EIO;
    size_t got = 0;
    int rc = ext4_fread(&file->u.ext4_file, buf, len, &got);
    return rc == EOK ? (int)got : -VFS_EIO;
}

static int pseudo_mount_stat(const vfs_mount_t *mnt, const char *path,
                             struct kernel_stat *st)
{
    (void)mnt;
    return pseudo_stat_path(path, st);
}

static int ext4_mount_stat(const vfs_mount_t *mnt, const char *path,
                           struct kernel_stat *st)
{
    (void)mnt;
    return fill_stat_from_ext4(st, path);
}

int vfs_stat_path(const char *path, struct kernel_stat *st)
{
    if (!path || !st)
        return -VFS_EINVAL;

    const vfs_mount_t *mnt = vfs_find_mount(path);
    if (!mnt || !mnt->ops || !mnt->ops->stat)
        return -VFS_ENOENT;
    return mnt->ops->stat(mnt, path, st);
}

static int pseudo_mount_readlink(const vfs_mount_t *mnt, const char *path,
                                 char *buf, size_t bufsz)
{
    (void)mnt;
    return pseudo_readlink(path, buf, bufsz);
}

static int ext4_mount_readlink(const vfs_mount_t *mnt, const char *path,
                               char *buf, size_t bufsz)
{
    (void)mnt;
    (void)path;
    (void)buf;
    (void)bufsz;
    return -VFS_ENOENT;
}

int vfs_readlink(const char *path, char *buf, size_t bufsz)
{
    if (!path || !buf)
        return -VFS_EINVAL;

    const vfs_mount_t *mnt = vfs_find_mount(path);
    if (!mnt || !mnt->ops || !mnt->ops->readlink)
        return -VFS_ENOENT;
    return mnt->ops->readlink(mnt, path, buf, bufsz);
}
