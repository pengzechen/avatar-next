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

#endif /* AARCH64_SYSCALL_ABI_H */
