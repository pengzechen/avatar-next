/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/task/riscv64/user_entry.c — RISC-V 用户态入口钩子

   arch_user_entry_debug 由 kernel/task/riscv64/switch.S 在首次跳转
   U-mode 前调用；目前唯一的职责是把 user_started 置位，让调度器区分
   "已经真正进过 U-mode"和"首次进入、satp 还没切"。
*/

#include "task/task.h"

void arch_user_entry_debug(uint64_t user_entry, uint64_t user_sp, uint64_t kernel_sp, uint64_t user_pgd)
{
    (void)user_entry;
    (void)user_sp;
    (void)kernel_sp;
    (void)user_pgd;

    task_t *cur = task_current();
    if (cur && cur->is_user_process)
        cur->user_started = true;
}
