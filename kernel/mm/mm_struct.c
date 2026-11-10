/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/mm/mm_struct.c — 进程地址空间对象（mm_struct）

   从 task_t 拆出 pgd/heap/mmap/interp/exe_path，原因见
   docs/task_t_refactor.md：
     - CLONE_VM 线程共享同一份地址空间，fork 复制一份新的；
     - “谁拥有 pgd、最后一个用户何时销毁”由 refcount/owns_pgd 决定，
       不再依赖 task_t 的 is_thread / shares_pgd 两个手工标志；
     - exec 换映像时只替换 mm，旧 mm 留给仍在共享它的线程。

   静态池大小 = TASK_MAX + AVATAR_MAX_CPUS（每个 CPU 的 idle 任务
   也有一份空 mm）。mm_put() 在锁外销毁页表（几百页，不能占着池锁）。
*/

#include "mm_struct.h"
#include "task/task.h"
#include "task/cpu.h"
#include "spinlock.h"
#include "string.h"
#include "vm_user.h"

#define MM_POOL_MAX (TASK_MAX + AVATAR_MAX_CPUS)

static mm_struct_t g_mm_pool[MM_POOL_MAX];
static spinlock_t g_mm_lock = SPINLOCK_INIT;

mm_struct_t *mm_alloc(void)
{
    uint64_t flags;
    spin_lock_irqsave(&g_mm_lock, &flags);

    for (uint32_t i = 0; i < MM_POOL_MAX; i++) {
        mm_struct_t *mm = &g_mm_pool[i];
        if (mm->refcount != 0)
            continue;
        memset(mm, 0, sizeof(*mm));
        mm->refcount = 1;
        spin_unlock_irqrestore(&g_mm_lock, flags);
        return mm;
    }

    spin_unlock_irqrestore(&g_mm_lock, flags);
    return NULL;
}

mm_struct_t *mm_share(mm_struct_t *mm)
{
    if (!mm)
        return NULL;
    uint64_t flags;
    spin_lock_irqsave(&g_mm_lock, &flags);
    mm->refcount++;
    spin_unlock_irqrestore(&g_mm_lock, flags);
    return mm;
}

void mm_put(mm_struct_t *mm)
{
    if (!mm)
        return;

    uint64_t pgd_phys = 0;
    uint64_t flags;

    spin_lock_irqsave(&g_mm_lock, &flags);
    if (mm->refcount > 1) {
        mm->refcount--;
        spin_unlock_irqrestore(&g_mm_lock, flags);
        return;
    }

    /*
       最后一个引用：先摘走 pgd 再把槽位标成空闲。销毁页表在锁外做，
       但要先保存 pgd 本地副本 —— 解锁后槽位可能立刻被别人复用。
       内核任务的 mm 也可能保存过内核 pgd（owns_pgd=false），不能销毁。
    */
    mm->refcount = 0;
    if (mm->owns_pgd && mm->pgd) {
        pgd_phys = (uint64_t)mm->pgd;
        mm->pgd = NULL;
        mm->owns_pgd = false;
    }
    spin_unlock_irqrestore(&g_mm_lock, flags);

    if (pgd_phys)
        vm_destroy_user_process(pgd_phys);
}
