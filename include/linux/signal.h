/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/linux/signal.h — 内核侧信号投递 API（Linux 进程语义）

   实现在 kernel/linux/core/signal.c。task 层只保留 TCB / 调度原语，
   不在这里暴露 pending/blocked/进程组策略。
*/

#ifndef KERNEL_SYSCALL_SIGNAL_H
#define KERNEL_SYSCALL_SIGNAL_H

#include "types.h"

struct task;

/* 向单个任务投递信号（可在中断上下文调用）。 */
void task_send_signal(struct task *t, int sig);

/* 是否有会真正打断阻塞的 pending 信号（忽略 SIG_IGN / 默认忽略类）。 */
bool task_signal_pending_effective(struct task *t);

/* 向进程组内所有用户态任务投递信号；返回收到信号的进程数。 */
int task_send_signal_to_pgid(uint32_t pgid, int sig);

#endif /* KERNEL_SYSCALL_SIGNAL_H */
