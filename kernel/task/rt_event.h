/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef KERNEL_TASK_RT_EVENT_H
#define KERNEL_TASK_RT_EVENT_H

/*
   kernel/task/rt_event.h — RT 事件位（wait/set）

   与 Linux eventfd 无关：这是内核任务之间的事件位，不涉及用户态 fd。
   用途：
     - RT worker 在“无事件”时真正阻塞，不占 CPU；
     - 驱动 ISR / 控制面用 rt_event_set() 唤醒等待者。

   语义：
     - 32 个事件位；wait 可以等任意一位（any）或全部位（all）；
     - timeout_ns == 0 表示无限等待；否则超时返回 0；
     - clear=true 时，满足等待后原子清掉请求的 mask；
     - set 可在 ISR 中调用；内部只做置位 + task_unblock，无分配、无睡眠；
     - 被唤醒任务按自身 RT 优先级入队，由调度器决定运行顺序。

   注意：timeout 依赖 tick / hrtimer 唤醒 `sleep_expire_ns`。在没有 oneshot
   也没有 tick 回调的极早期环境里，只能使用无限等待。
*/

#include "types.h"
#include "list.h"
#include "spinlock.h"

typedef struct task task_t;

typedef struct rt_event {
    spinlock_t lock; /* 保护 bits 与 waiters */
    list_t waiters;
    uint32_t bits;
} rt_event_t;

void rt_event_init(rt_event_t *e);

/* 当前事件位（内部加锁）。 */
uint32_t rt_event_get(rt_event_t *e);

/* 置位并唤醒所有满足 mask 的等待者；ISR 安全。 */
void rt_event_set(rt_event_t *e, uint32_t bits);

/* 清位；不会唤醒等待者。 */
void rt_event_clear(rt_event_t *e, uint32_t bits);

/*
   rt_event_wait - 等待事件位
   @mask:     关心的事件位（不能为 0）
   @wait_all: true = 全部位都置位才算满足；false = 任意一位
   @timeout_ns: 0 = 无限等待；否则超时返回 0
   @clear:    满足后清掉 mask 对应的位

   返回满足时的 (bits & mask)；超时返回 0。必须在任务上下文调用。
*/
uint32_t rt_event_wait(rt_event_t *e, uint32_t mask, bool wait_all, uint64_t timeout_ns, bool clear);

#endif /* KERNEL_TASK_RT_EVENT_H */
