/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/syscall/fs/process_fs.c — 进程级 fd 表 / cwd（files_struct、fs_struct）

   从 task_t 拆出来的两块进程文件系统状态，原因见
   docs/task_t_refactor.md。静态池 + 引用计数：
     - 任务创建时各取一份（fd 表初始全 -1，cwd 初始 "/"）；
     - 当前 clone 语义仍是“fork/线程各复制一份”，所以 refcount 通常为 1，
       但模型已经为 CLONE_FILES / CLONE_FS 共享留好接口；
     - TCB 回收在锁外 release，最后一个引用归还池槽位。

   池大小 = TASK_MAX + AVATAR_MAX_CPUS：除了 g_task_pool 的 64 个槽，
   每个 CPU 的 idle 任务也需要一份。
*/

#include "process_fs.h"
#include "task/task.h"
#include "task/cpu.h"
#include "spinlock.h"
#include "string.h"

#define PROC_FS_POOL_MAX (TASK_MAX + AVATAR_MAX_CPUS)

static files_struct_t g_files_pool[PROC_FS_POOL_MAX];
static fs_struct_t g_fs_pool[PROC_FS_POOL_MAX];
static spinlock_t g_proc_fs_lock = SPINLOCK_INIT;

files_struct_t *files_alloc(void)
{
    uint64_t flags;
    spin_lock_irqsave(&g_proc_fs_lock, &flags);

    for (uint32_t i = 0; i < PROC_FS_POOL_MAX; i++) {
        files_struct_t *f = &g_files_pool[i];
        if (f->refcount != 0)
            continue;
        for (uint32_t fd = 0; fd < FILES_FD_MAX; fd++)
            f->fd_table[fd] = -1;
        memset(f->fd_cloexec, 0, sizeof(f->fd_cloexec));
        f->refcount = 1;
        spin_unlock_irqrestore(&g_proc_fs_lock, flags);
        return f;
    }

    spin_unlock_irqrestore(&g_proc_fs_lock, flags);
    return NULL;
}

void files_put(files_struct_t *files)
{
    if (!files)
        return;
    uint64_t flags;
    spin_lock_irqsave(&g_proc_fs_lock, &flags);
    if (files->refcount > 0)
        files->refcount--;
    spin_unlock_irqrestore(&g_proc_fs_lock, flags);
}

fs_struct_t *fs_alloc(void)
{
    uint64_t flags;
    spin_lock_irqsave(&g_proc_fs_lock, &flags);

    for (uint32_t i = 0; i < PROC_FS_POOL_MAX; i++) {
        fs_struct_t *fs = &g_fs_pool[i];
        if (fs->refcount != 0)
            continue;
        fs->cwd[0] = '/';
        fs->cwd[1] = '\0';
        fs->refcount = 1;
        spin_unlock_irqrestore(&g_proc_fs_lock, flags);
        return fs;
    }

    spin_unlock_irqrestore(&g_proc_fs_lock, flags);
    return NULL;
}

void fs_put(fs_struct_t *fs)
{
    if (!fs)
        return;
    uint64_t flags;
    spin_lock_irqsave(&g_proc_fs_lock, &flags);
    if (fs->refcount > 0)
        fs->refcount--;
    spin_unlock_irqrestore(&g_proc_fs_lock, flags);
}
