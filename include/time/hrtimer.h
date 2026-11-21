/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef HRTIMER_H
#define HRTIMER_H

#include "types.h"

/*
   hrtimer — 高精度睡眠/唤醒

   V1 只在 x86_64 上启用：LAPIC Timer 的 TSC-deadline 模式。
   aarch64/riscv64 上 timer_arch_oneshot_supported() 返回 false，
   hrtimer_sleep_until() 退化为 100Hz tick 睡眠。

   设计：
     - 不再使用 LAPIC periodic，而是把“下一个 100Hz tick”和“本核最早的
       sleep_expire_ns”取最小值，写 IA32_TSC_DEADLINE；
     - 定时器 ISR 先判断 tick 是否到期（由 x86 timer_handler 跑原有
       tick 记账），再唤醒到期的 sleeper，最后重新武装；
     - sleep_expire_ns 是现有的每任务字段，用户 nanosleep 也会因此获得
       高精度唤醒。
*/

/* 每核初始化：设置下一个 tick 并武装定时器。由 x86 timer_arch_enable 调用。 */
void hrtimer_cpu_init(void);

/* 定时器 ISR 调用：返回 true 表示 100Hz tick 到期，调用方需跑 tick 记账。 */
bool hrtimer_isr(void);

/* 阻塞到绝对时间 deadline_ns；oneshot 不可用时退化为 tick 睡眠。 */
void hrtimer_sleep_until(uint64_t deadline_ns);

bool hrtimer_oneshot_supported(void);             /* 当前 CPU */
bool hrtimer_cpu_oneshot_supported(uint32_t cpu); /* 指定逻辑 CPU */

/*
   架构钩子：
     x86_64 由 driver/timer/timer_x86_64_impl.h 实现；
     其它架构在 hrtimer.c 里提供 false/no-op 版本。
*/
bool timer_arch_oneshot_supported(void);
void timer_arch_program_oneshot_ns(uint64_t deadline_ns);

#endif /* HRTIMER_H */
