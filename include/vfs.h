#ifndef VFS_H
#define VFS_H

/*
 * include/vfs.h - small kernel VFS layer
 *
 * First-stage VFS abstraction: fd objects point at struct vfs_file, and
 * operations dispatch through file_ops. Path routing uses a small static
 * mount table: /dev, /proc and /sys route to pseudofs, while / routes to ext4.
 */

#include "types.h"
#include "kernel_stat.h"
#include <ext4.h>

typedef enum {
    VFS_FILE_NONE = 0,
    VFS_FILE_EXT4_FILE,
    VFS_FILE_EXT4_DIR,
    VFS_FILE_PSEUDO,
    VFS_FILE_PIPE,
    VFS_FILE_PTY,
    VFS_FILE_SOCKET,
} vfs_file_kind_t;

struct vfs_file;

typedef struct {
    int (*read)(struct vfs_file *file, void *buf, size_t len);
    int (*write)(struct vfs_file *file, const void *buf, size_t len);
    int (*ioctl)(struct vfs_file *file, uint64_t req, void *argp);
    int (*getdents)(struct vfs_file *file, void *buf, size_t bufsz);
    int (*seek)(struct vfs_file *file, int64_t offset, int whence, uint64_t *new_off);
    int (*stat)(struct vfs_file *file, struct kernel_stat *st);
    uint32_t (*poll)(struct vfs_file *file, int pool_idx);
    int (*truncate)(struct vfs_file *file, uint64_t length);
    int (*close)(struct vfs_file *file);
} vfs_file_ops_t;

typedef struct vfs_file {
    vfs_file_kind_t kind;
    int flags;
    uint32_t refcnt;
    uint64_t offset;
    char path[128];
    const vfs_file_ops_t *ops;
    union {
        ext4_file ext4_file;
        ext4_dir ext4_dir;
        struct {
            int32_t node_id;
        } pseudo;
        struct {
            int16_t pipe_idx;
            bool is_write_end;
        } pipe;
        struct {
            int16_t pty_idx;
            bool is_master;
        } pty;
        struct {
            int16_t sock_idx;
        } sock;
    } u;
} vfs_file_t;

int vfs_open(const char *path, int flags, int mode, vfs_file_t **out);
int vfs_create_pipe_file(int pipe_idx, bool is_write_end, int flags, vfs_file_t **out);
int vfs_create_pty_file(int pty_idx, bool is_master, int flags,
                        const char *path, vfs_file_t **out);
int vfs_create_socket_file(int sock_idx, int flags, vfs_file_t **out);
int vfs_close(vfs_file_t *file);
void vfs_discard_unopened(vfs_file_t *file);
void vfs_ref(vfs_file_t *file);

int vfs_read(vfs_file_t *file, void *buf, size_t len);
int vfs_write(vfs_file_t *file, const void *buf, size_t len);
int vfs_ioctl(vfs_file_t *file, uint64_t req, void *argp);
int vfs_getdents(vfs_file_t *file, void *buf, size_t bufsz);
int vfs_seek(vfs_file_t *file, int64_t offset, int whence, uint64_t *new_off);
int vfs_stat_file(vfs_file_t *file, struct kernel_stat *st);
uint32_t vfs_poll(vfs_file_t *file, int pool_idx);
int vfs_pread(vfs_file_t *file, void *buf, size_t len, int64_t offset);
int vfs_pwrite(vfs_file_t *file, const void *buf, size_t len, int64_t offset);
int vfs_truncate(vfs_file_t *file, uint64_t length);
int vfs_socket_index(vfs_file_t *file);
bool vfs_file_matches_socket(vfs_file_t *file, int sock_idx);
bool vfs_file_matches_pty(vfs_file_t *file, int pty_idx, bool is_master);
int vfs_proc_fd_target(vfs_file_t *file, char *buf, size_t bufsz);
int vfs_read_to_phys(vfs_file_t *file, uint64_t offset, void *buf, size_t len);
int vfs_stat_path(const char *path, struct kernel_stat *st);
int vfs_readlink(const char *path, char *buf, size_t bufsz);

static inline bool vfs_file_is_regular(const vfs_file_t *file)
{
    return file && file->kind == VFS_FILE_EXT4_FILE;
}

static inline bool vfs_file_is_dir(const vfs_file_t *file)
{
    return file && (file->kind == VFS_FILE_EXT4_DIR || file->kind == VFS_FILE_PSEUDO);
}

#endif /* VFS_H */
