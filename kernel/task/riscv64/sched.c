/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/task/riscv64/sched.c — RISC-V 调度路径的架构部分

   RISC-V 的 satp 切换由 arch_task_switch(switch.S) 完成，C 层负责
   选出"要装入的 satp 根表"以及"要不要把当前 satp 存回 prev->mm->pgd"。
*/

#include "task/sched.h"
#include "task/switch.h"
#include "task/task.h"
#include "mm/mm_vm.h"

uint64_t **arch_sched_prev_pgd_slot(task_t *prev)
{
    /*
       RISC-V 用户进程：pgd 是创建时固定的物理地址, 不需要由 arch_task_switch
       动态保存。若传 &prev->mm->pgd, 当用户进程在 arch_switch_to_user 切换 satp
       之前被抢占(satp 仍为内核页表)时, prev->mm->pgd 会被覆写为内核页表地址,
       导致下次调度回来时以错误的 satp 进入 U-mode → VA 0x10000 不在内核页表 → Inst PF。
       因此对用户进程传 NULL(跳过 satp 保存), 保持 pgd 不变。
    */
    return prev->is_user_process ? NULL : &prev->mm->pgd;
}

uint64_t *arch_sched_next_pgd(task_t *next)
{
    if (next->is_user_process) {
        /* 首次进入用户进程：延迟 satp 切换, 由 arch_switch_to_user 负责 */
        return next->user_started ? next->mm->pgd : NULL;
    }
    if (next->mm->pgd == NULL) {
        /* 切换到内核任务：恢复内核根页表 */
        return (uint64_t *)mm_vm_kernel_root_phys();
    }
    return next->mm->pgd;
}
