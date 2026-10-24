/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/fs/vfs/vfs_internal.h - VFS 内部接口（vfs.c 与 fat_vfs.c 共用）

   公开头 include/vfs.h 只暴露 fd 级 API；mount 分发表和路径操作常量放在
   这里，避免把 FatFs 相关实现细节带进所有 VFS 使用者。
*/
#ifndef VFS_INTERNAL_H
#define VFS_INTERNAL_H

#include "types.h"
#include "kernel_stat.h"
#include "vfs.h"

/* ── 内核内部 errno（与 Linux 数值一致，syscall 层直接透传）────────── */
#define VFS_ENOENT    2
#define VFS_EIO       5
#define VFS_EBADF     9
#define VFS_ENOMEM    12
#define VFS_EACCES    13
#define VFS_EEXIST    17
#define VFS_EXDEV     18
#define VFS_ENODEV    19
#define VFS_ENOTDIR   20
#define VFS_EISDIR    21
#define VFS_EINVAL    22
#define VFS_EMFILE    24
#define VFS_ENOSPC    28
#define VFS_ESPIPE    29
#define VFS_EROFS     30
#define VFS_ENOSYS    38
#define VFS_ENOTEMPTY 39

/* ── Linux open/seek 标志（架构无关的那部分）──────────────────────── */
#define O_ACCMODE_AVATAR   0000003
#define O_WRONLY_AVATAR    0000001
#define O_RDWR_AVATAR      0000002
#define O_CREAT_AVATAR     0000100
#define O_EXCL_AVATAR      0000200
#define O_TRUNC_AVATAR     0001000
#define O_APPEND_AVATAR    0002000
#define O_DIRECTORY_AVATAR 0200000
/* O_NONBLOCK 由 fcntl(F_SETFL) 存进 fd_obj/vfs_file 的 flags(见
   syscall/fs/file_ops.c)。管道读写要读这个位, 否则非阻塞语义不成立。
*/
#define O_NONBLOCK_AVATAR 04000
#define SEEK_SET_AVATAR   0
#define SEEK_CUR_AVATAR   1
#define SEEK_END_AVATAR   2

typedef struct vfs_mount vfs_mount_t;

/*
   每个挂载点必须提供的路径级操作。FAT 和 ext4 都实现这些；
   pseudofs 只实现 open/stat/readlink，命名空间操作返回 EROFS。
*/
typedef struct {
    int (*open)(const vfs_mount_t *mnt, const char *path, int flags, int mode, vfs_file_t **out);
    int (*stat)(const vfs_mount_t *mnt, const char *path, struct kernel_stat *st);
    int (*readlink)(const vfs_mount_t *mnt, const char *path, char *buf, size_t bufsz);
    int (*unlink)(const vfs_mount_t *mnt, const char *path, bool is_dir);
    int (*mkdir)(const vfs_mount_t *mnt, const char *path, int mode);
    int (*rename)(const vfs_mount_t *mnt, const char *oldpath, const char *newpath);
} vfs_mount_ops_t;

struct vfs_mount {
    const char *path;
    const vfs_mount_ops_t *ops;
};

/* fat_vfs.c 定义，vfs_register_fat_mount() 使用 */
extern const vfs_mount_ops_t fat_mount_ops;

/* vfs.c 提供的小工具（申请 vfs_file_t / 复制路径），fat_vfs.c 也用 */
vfs_file_t *vfs_alloc_file(void);
void vfs_copy_path(vfs_file_t *file, const char *path);

#endif /* VFS_INTERNAL_H */
