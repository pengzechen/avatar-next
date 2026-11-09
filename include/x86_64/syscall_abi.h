/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef X86_64_SYSCALL_ABI_H
#define X86_64_SYSCALL_ABI_H

#include "types.h"
#include "x86_64/exception.h"

static inline uint64_t syscall_abi_nr(const trap_frame_t *frame)
{
    return frame->rax; /* syscall number in rax */
}

static inline uint64_t syscall_abi_arg(const trap_frame_t *frame, int idx)
{
    switch (idx) {
    case 0:
        return frame->rdi;
    case 1:
        return frame->rsi;
    case 2:
        return frame->rdx;
    case 3:
        return frame->r10;
    case 4:
        return frame->r8;
    case 5:
        return frame->r9;
    default:
        return 0;
    }
}

static inline void syscall_abi_set_ret(trap_frame_t *frame, uint64_t ret)
{
    frame->rax = ret;
}

static inline uint64_t syscall_abi_ip(const trap_frame_t *frame)
{
    return frame->rip;
}

static inline uint64_t syscall_abi_user_sp(const trap_frame_t *frame)
{
    return frame->rsp;
}

/*
   syscall_abi_reset_frame_for_exec - execve 成功后把 trap frame 重置成
   新程序的初始用户现场

   syscall_wrapper.S 的返回路径用 trap frame 里的 RCX/R11/RSP 做 sysretq，
   所以入口/栈/标志位都从这里取。FP 状态没有 per-task 保存，不在本函数
   范围内(和现有首次进入路径一致)。CS/SS 保持入口压入的 0x2B/0x23。
*/
static inline void syscall_abi_reset_frame_for_exec(trap_frame_t *frame, uint64_t entry, uint64_t sp)
{
    frame->r15 = 0;
    frame->r14 = 0;
    frame->r13 = 0;
    frame->r12 = 0;
    frame->r11 = 0;
    frame->r10 = 0;
    frame->r9 = 0;
    frame->r8 = 0;
    frame->rbp = 0;
    frame->rdi = 0;
    frame->rsi = 0;
    frame->rdx = 0;
    frame->rcx = 0;
    frame->rbx = 0;
    frame->rax = 0;
    frame->rip = entry;
    frame->rflags = 0x202; /* IF=1, Reserved=1 */
    frame->rsp = sp;
}

#endif /* X86_64_SYSCALL_ABI_H */
