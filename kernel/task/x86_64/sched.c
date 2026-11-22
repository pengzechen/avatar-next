/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/task/x86_64/sched.c — x86_64 调度路径的架构部分

   x86 的 CR3/TSS/FS 切换留在 kernel/task/sched.c 的 #if ARCH_X86_64 里；
   这里只放伪帧参数选择。
*/

#include "task/sched.h"
#include "task/switch.h"
#include "task/task.h"

uint64_t **arch_sched_prev_pgd_slot(task_t *prev)
{
    return &prev->mm->pgd;
}

uint64_t *arch_sched_next_pgd(task_t *next)
{
    return next->mm->pgd;
}
