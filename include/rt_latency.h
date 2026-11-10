/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef RT_LATENCY_H
#define RT_LATENCY_H

/*
   R0 延迟测量（docs/realtime/ARCHITECTURE.md）

   测量的是“值得优化的量”，不是精确的硬件 trace：
     - wake→run：任务从 sched_unblock() 变为 READY 到真正被切入的延迟；
     - syscall：用户 syscall 的内核执行时长（不含 task_block 睡眠段）；
     - tick_gap：相邻两次 timer tick 的间隔，作为“长关中断/长临界区”
       的近似指标（正常约 10ms；明显大于 10ms 说明 timer 被拖住）。

   数据通过 /proc/rtstat 读取。统计是 per-CPU 的，wake-up 跨核时记在
   实际运行的那颗核上。
*/

#include "types.h"

struct task;

void rt_latency_wake(struct task *t);
void rt_latency_run(struct task *t, uint64_t now_ns);
void rt_latency_syscall(uint64_t duration_ns, uint32_t nr);
void rt_latency_tick(uint32_t cpu_id, uint64_t now_ns);

/* 清空统计（启动收尾时调用；之后是“运行时”基线，不含启动期长临界区）。 */
void rt_latency_reset(void);

/*
   RT_IRQ_TRACE=1 时由 arch_irq_save/restore/disable/enable 调用：
   记录“关中断区间”的起止。不是默认开启（会多一次函数调用）。
*/
void rt_latency_irq_off_begin(void);
void rt_latency_irq_off_end(void);

/* 生成 /proc/rtstat 的文本；返回写入长度（不含 NUL）。 */
int rt_latency_snprintf(char *buf, size_t cap);

#endif /* RT_LATENCY_H */
