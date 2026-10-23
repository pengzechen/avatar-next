/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef KERNEL_TASK_COMPLETION_H
#define KERNEL_TASK_COMPLETION_H

/*
   kernel/task/completion.h — 一次性的「等待某个事件」原语

   与 mutex 的区别：mutex 是「持有者释放后继续」, completion 是「事件发生
   后所有等待者继续」。它的第一个用户是多 vCPU 的 AP 启动闸门：

       AP 任务：   completion_wait(&vcpu->start);   // 睡到 CPU_ON
       CPU_ON：    ...填 vcpu 入口状态...
                   completion_complete_all(&vcpu->start);

   为什么不用 task_block() 裸写：SMP 下「检查条件 → 挂等待队列 → 睡」必须
   和唤醒方在同一把锁下完成, 否则唤醒会落在两步之间丢掉(lost wakeup)。
   completion 把条件(done), 等待队列和锁封在一起, 调用方没有写错的余地。

   实现依赖 sched_unblock() 的 on_cpu/wake_pending 规则：等待者即使还没
   真正切出 CPU, 被唤醒也不会提前入队。
*/

#include "list.h"
#include "spinlock.h"
#include "types.h"

typedef struct completion {
    spinlock_noirq_t lock; /* 保护 done + waiters */
    list_t waiters;        /* 等待者(task_t.wait_node) */
    int done;              /* 事件是否已经发生 */
} completion_t;

/* 静态初始化(需要随后 completion_init 或直接用本宏) */
#define COMPLETION_INIT { .lock = SPINLOCK_NOIRQ_INIT, .waiters = { .first = 0, .last = 0, .count = 0 }, .done = 0 }

void completion_init(completion_t *c);

/*
   completion_wait — 阻塞直到 done 被置位

   只能在任务上下文调用(内部会 sched_schedule)。若 done 已经是 1,
   立即返回, 不睡眠。
*/
void completion_wait(completion_t *c);

/*
   completion_complete_all — 置 done 并唤醒所有等待者

   可在任务/中断上下文调用。先置 done 再唤醒 —— 等待者被唤醒后会重新
   检查 done, 这个顺序保证它不会再次睡回去。

   重复调用是幂等的(已经 done 时仍会清空等待队列)。
*/
void completion_complete_all(completion_t *c);

/* 查询 done(不睡眠) */
int completion_done(completion_t *c);

/*
   completion_reset — 重新武装(仅当没有等待者时调用)

   典型用法：同一个 completion 对象复用于下一次 AP 启动。若尚有等待者,
   本函数拒绝清除 done 并返回 -1。
*/
int completion_reset(completion_t *c);

#endif /* KERNEL_TASK_COMPLETION_H */
