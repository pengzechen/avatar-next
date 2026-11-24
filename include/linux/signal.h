/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef KERNEL_LINUX_SIGNAL_H
#define KERNEL_LINUX_SIGNAL_H

/*
   include/linux/signal.h — Linux 信号 ABI：常量 / signal_struct / 投递

   这里放“信号本身”的常量/结构、进程级 signal_struct 处置 API，以及
   向任务/进程组投递信号的 API。每线程的 pending/blocked 掩码与任务生命
   周期绑定，字段仍在 task_t 里；syscall 入口在 kernel/linux/core/signal.c，
   signal_struct 静态池在 kernel/linux/core/signal_struct.c。
*/

#include "types.h"

/* ── 信号常量 ────────────────────────────────────────────── */
#define NSIG    64
#define SIG_DFL 0ULL /* 默认动作(终止) */
#define SIG_IGN 1ULL /* 忽略 */

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGWINCH 28

#define SA_RESTORER 0x04000000ULL

/* ── 信号动作结构 ────────────────────────────────────────── */
typedef struct {
    uint64_t sa_handler;  /* SIG_DFL / SIG_IGN / 用户 handler 地址 */
    uint64_t sa_flags;    /* SA_RESTORER 等标志 */
    uint64_t sa_restorer; /* rt_sigreturn 蹦床地址 */
    uint64_t sa_mask;     /* handler 执行期间额外屏蔽的信号 */
} sig_action_t;

/*
   signal_struct - 进程级信号处置（Linux 中一个线程组共享一份）

   task_t::sig == NULL 表示“全部信号都是 SIG_DFL”，这样大多数没有安装
   handler 的任务不占这 2 KiB。第一次 rt_sigaction 或创建线程组时从
   g_signal_pool[TASK_MAX] 静态池分配。

   refcount == 0 表示静态池里的空闲槽位（struct 本身常驻 BSS）。
*/
typedef struct signal_struct {
    volatile int refcount;
    sig_action_t actions[NSIG];
} signal_struct_t;

struct task;

/* ── 信号处置 API ──────────────────────────────────────────
   sig==NULL 时 task_sigaction_get 返回全 SIG_DFL，set 会在需要时
   从静态池懒分配。task_signal_share/copy 分别对应 CLONE_THREAD
   共享与 fork 复制；release 在 TCB 回收时调用。
*/
bool task_sigaction_get(struct task *t, int sig, sig_action_t *out);
int task_sigaction_set(struct task *t, int sig, const sig_action_t *act);
signal_struct_t *task_signal_share(struct task *parent);
bool task_signal_copy(struct task *parent, struct task *child);
void task_signal_release(struct task *t);
void task_signal_exec_reset(struct task *t);

/* 向单个任务投递信号（可在中断上下文调用）。 */
void task_send_signal(struct task *t, int sig);

/* 是否有会真正打断阻塞的 pending 信号（忽略 SIG_IGN / 默认忽略类）。 */
bool task_signal_pending_effective(struct task *t);

/* 向进程组内所有用户态任务投递信号；返回收到信号的进程数。 */
int task_send_signal_to_pgid(uint32_t pgid, int sig);

#endif /* KERNEL_LINUX_SIGNAL_H */
