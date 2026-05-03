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

#endif /* RISCV64_SYSCALL_ABI_H */
