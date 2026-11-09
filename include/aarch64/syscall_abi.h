/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef AARCH64_SYSCALL_ABI_H
#define AARCH64_SYSCALL_ABI_H

#include "types.h"
#include "aarch64/exception.h"

static inline uint64_t syscall_abi_nr(const trap_frame_t *frame)
{
    return frame->r[8]; /* x8 = syscall number */
}

static inline uint64_t syscall_abi_arg(const trap_frame_t *frame, int idx)
{
    return (idx >= 0 && idx < 6) ? frame->r[idx] : 0;
}

static inline void syscall_abi_set_ret(trap_frame_t *frame, uint64_t ret)
{
    frame->r[0] = ret; /* x0 = return value */
}

static inline uint64_t syscall_abi_ip(const trap_frame_t *frame)
{
    return frame->elr;
}

static inline uint64_t syscall_abi_user_sp(const trap_frame_t *frame)
{
    return frame->usp;
}

/*
   syscall_abi_reset_frame_for_exec - execve 成功后把 trap frame 重置成
   新程序的初始用户现场

   从 syscall 返回时会整体恢复 trap frame，所以原地 exec 只需要改这里：
   入口、栈、通用寄存器、TLS、FP/SIMD 状态全部换成"刚 exec 的新进程"。
   SPSR 用 EL0t + IRQ 使能，与 arch_switch_to_user 首次进入保持一致。
*/
static inline void syscall_abi_reset_frame_for_exec(trap_frame_t *frame, uint64_t entry, uint64_t sp)
{
    for (int i = 0; i < NUM_REGS; i++)
        frame->r[i] = 0;
    frame->usp = sp;
    frame->elr = entry;
    frame->spsr = 0x340;  /* EL0t, IRQ 使能 */
    frame->tpidr_el0 = 0; /* exec 不继承 TLS */
    frame->fp_pad = 0;
    for (int i = 0; i < 32; i++)
        frame->q[i] = 0;
    frame->fpcr = 0;
    frame->fpsr = 0;
}

#endif /* AARCH64_SYSCALL_ABI_H */
