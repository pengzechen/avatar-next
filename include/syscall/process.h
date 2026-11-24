/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/syscall/process.h — Linux 进程属性默认值

   task 层只负责 TCB 与调度；umask / dumpable / rlimit 的 Linux 默认值
   属于 ABI 策略，放在 kernel/syscall/core/resource.c。
*/

#ifndef KERNEL_SYSCALL_PROCESS_H
#define KERNEL_SYSCALL_PROCESS_H

#include "types.h"

struct task;

/* 给新分配的 TCB 填 Linux 进程属性默认值。 */
void task_linux_defaults_init(struct task *t);

#endif /* KERNEL_SYSCALL_PROCESS_H */
