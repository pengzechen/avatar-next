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
    case 0: return frame->rdi;
    case 1: return frame->rsi;
    case 2: return frame->rdx;
    case 3: return frame->r10;
    case 4: return frame->r8;
    case 5: return frame->r9;
    default: return 0;
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

#endif /* X86_64_SYSCALL_ABI_H */
