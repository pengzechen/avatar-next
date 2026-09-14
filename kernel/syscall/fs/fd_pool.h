/*
 * fd_pool.h - 全局 fd 对象池声明
 *
 * 类型、池数组与池/任务级 fd 操作函数声明。
 * 实现位于 kernel/syscall/fd_pool.c。
 */
#ifndef KERNEL_SYSCALL_FD_POOL_H
#define KERNEL_SYSCALL_FD_POOL_H

#include "types.h"
#include "vfs.h"
#include "syscall/io/epoll.h"

/* 前向声明 task_t，避免本头文件强制拉入 task/task.h */
struct task;
typedef struct task task_t;

#define FD_POOL_SIZE  128

typedef enum {
    FDT_FREE = 0,
    FDT_ALLOCATED,
    FDT_VFS,
    FDT_ION,
    FDT_EPOLL,
} fd_type_t;

typedef struct {
    fd_type_t  type;
    int        flags;
    char       path[128];
    vfs_file_t *vfs_file;
    union {
        struct {
            uint32_t handle;
        } ion;
        struct {
            int16_t  ep_idx;
        } epoll;
    };
    fd_waitqueue_t wq;
} fd_obj_t;

extern fd_obj_t g_fd_pool[FD_POOL_SIZE];

/* ── fd 池槽位分配 / 释放 ─────────────────────────────────────── */
int  fd_pool_alloc(void);
void fd_pool_free (int idx);
void fd_obj_ref   (int idx);
void fd_obj_close (int idx);
void fd_obj_attach_vfs(int idx, vfs_file_t *file);

/* ── 任务级 fd 表操作 ─────────────────────────────────────────── */
int task_alloc_ion_fd(task_t *task, uint32_t handle);
int task_get_ion_handle(task_t *task, int fd, uint32_t *handle);
int        task_alloc_fd(task_t *task, int pool_idx);
fd_obj_t  *task_get_fd  (task_t *task, int fd);

static inline vfs_file_t *fd_obj_file(fd_obj_t *obj)
{
    return obj ? obj->vfs_file : NULL;
}

#endif /* KERNEL_SYSCALL_FD_POOL_H */
