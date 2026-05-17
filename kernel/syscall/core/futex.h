/*
 * futex.h - 内核 futex 实现接口
 *
 * 仅供 kernel/syscall/*.c 使用。
 */
#ifndef KERNEL_SYSCALL_FUTEX_H
#define KERNEL_SYSCALL_FUTEX_H

#include "types.h"

/* 唤醒等待在 uaddr 上的最多 count 个任务，返回唤醒数 */
int futex_do_wake(uintptr_t uaddr, int count);

/* 若 *uaddr == val，阻塞当前任务；返回 0 / -EAGAIN / -ENOMEM */
int sys_futex_wait(uint32_t *uaddr, uint32_t val);

#endif /* KERNEL_SYSCALL_FUTEX_H */
