/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef RISCV64_SYSCALL_ABI_H
#define RISCV64_SYSCALL_ABI_H

#include "types.h"
#include "riscv64/exception.h"

static inline uint64_t syscall_abi_nr(const trap_frame_t *frame)
{
    return frame->x[17]; /* a7 = syscall number */
}

static inline uint64_t syscall_abi_arg(const trap_frame_t *frame, int idx)
{
    return (idx >= 0 && idx < 6) ? frame->x[10 + idx] : 0; /* a0..a5 */
}

static inline void syscall_abi_set_ret(trap_frame_t *frame, uint64_t ret)
{
    frame->x[10] = ret; /* a0 = return value */
}

static inline uint64_t syscall_abi_ip(const trap_frame_t *frame)
{
    return frame->sepc;
}

static inline uint64_t syscall_abi_user_sp(const trap_frame_t *frame)
{
    return frame->x[2]; /* sp */
}

/*
   syscall_abi_reset_frame_for_exec - execve 成功后把 trap frame 重置成
   新程序的初始用户现场

   返回路径是一条普通 sret，所以这里必须自己把 sstatus 摆成"返回 U 模式、
   SPIE=1、SIE=0"，与 arch_switch_to_user 的处理保持一致；FP 状态清零，
   tp 也清零(exec 不继承 TLS)。x0 随后会被 syscall 返回值的回填覆盖
   （值为 0，新程序不依赖它）。
*/
static inline void syscall_abi_reset_frame_for_exec(trap_frame_t *frame, uint64_t entry, uint64_t sp)
{
    for (int i = 0; i < 32; i++)
        frame->x[i] = 0;
    frame->x[2] = sp;
    frame->sepc = entry;
    frame->scause = 0;
    frame->stval = 0;
    frame->sstatus = (frame->sstatus & ~0x102ULL) | (1ULL << 23) | (1ULL << 5);
    for (int i = 0; i < 32; i++)
        frame->f[i] = 0;
    frame->fcsr = 0;
}

#endif /* RISCV64_SYSCALL_ABI_H */
