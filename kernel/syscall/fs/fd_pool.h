/*
 * fd_pool.h - 全局 fd 对象池声明
 *
 * 类型、池数组与池/任务级 fd 操作函数声明。
 * 实现位于 kernel/syscall/fd_pool.c。包含本头文件需要 lwext4
 * 头文件搜索路径（LWEXT4_CFLAGS）— fd_obj_t 内嵌 ext4_file / ext4_dir。
 */
#ifndef KERNEL_SYSCALL_FD_POOL_H
#define KERNEL_SYSCALL_FD_POOL_H

#include "types.h"
#include <ext4.h>

/* 前向声明 task_t，避免本头文件强制拉入 task/task.h */
struct task;
typedef struct task task_t;

#define FD_POOL_SIZE  64

typedef enum {
    FDT_FREE = 0,
    FDT_FILE,
    FDT_DIR,
    FDT_PSEUDO,
} fd_type_t;

typedef struct {
    fd_type_t  type;
    int        flags;
    char       path[128];
    union {
        ext4_file file;
        ext4_dir  dir;
        struct {
            int32_t  node_id;
            uint64_t off;
        } pseudo;
    };
} fd_obj_t;

extern fd_obj_t g_fd_pool[FD_POOL_SIZE];

/* ── fd 池槽位分配 / 释放 ─────────────────────────────────────── */
int  fd_pool_alloc(void);
void fd_pool_free (int idx);

/* ── 任务级 fd 表操作 ─────────────────────────────────────────── */
int        task_alloc_fd(task_t *task, int pool_idx);
fd_obj_t  *task_get_fd  (task_t *task, int fd);

#endif /* KERNEL_SYSCALL_FD_POOL_H */
