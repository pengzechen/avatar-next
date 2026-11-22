/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/task/aarch64/sched.c — AArch64 调度路径的架构部分

   AArch64 用 TTBR0/TTBR1 硬件分割用户/内核地址空间，页表切换由
   arch_task_switch(switch.S) 负责，C 层没有额外的 MMU/TLS 准备工作。
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
