/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef __TIMER_X86_64_IMPL_H__
#define __TIMER_X86_64_IMPL_H__

/*
   x86_64 定时器实现 — 使用 LAPIC Timer (Periodic 模式)

   频率校准：lapic_timer_init 内部用 PIT Channel 2 校准 LAPIC bus 频率,
   对外只需指定目标中断频率(TIMER_FREQUENCY_HZ, 来自 driver_cfg.h)。

   函数签名与 AArch64/RISC-V impl 一致(无 static inline),
   由 timer.c #include 本文件来获得定义。
*/

#include "driver/time.h"
#include "driver/irq.h"
#include "klog.h"
#include "exception.h" /* IDT_LAPIC_TIMER_VEC */
#include "irq/lapic.h" /* lapic_timer_init / lapic_timer_stop */

/* ── x86_64 特定全局变量 ────────────────────────────────────── */

volatile uint64_t g_x86_uptime_seconds = 0;
static uint32_t g_x86_tick_counter = 0;

/* 前向声明(定义在本文件末尾) */
void timer_handler(void *frame);

/* 公共 IRQ 接口的处理器签名是 (frame, ctx)；定时器不使用 ctx */
static void timer_irq_entry(void *frame, void *ctx)
{
    (void)ctx;
    timer_handler(frame);
}

/* ── 架构特定操作实现 ────────────────────────────────────────── */

void timer_arch_init(void)
{
    /* x86_64 上 LAPIC 是计时源, 实际频率由校准决定；
       g_timer_frequency 设一个占位值(校准后 lapic.c 内部使用)
    */
    g_timer_frequency = 0; /* 将由 lapic_timer_init 校准 */

    /* 时钟配错是经典 bug 来源 —— 值得一行；原来是三行 */
    KLOG_INFO("Timer: x86_64 LAPIC tick=%dHz (%dms)\n", TIMER_FREQUENCY_HZ, TIMER_TICK_MS);

    timer_reset_stats();

    /* 控制器负责把 handler 挂到 LAPIC timer 向量 */
    if (irq_register_timer(timer_irq_entry, NULL) != 0)
        KLOG_ERROR("Timer: failed to register LAPIC timer IRQ\n");
}

void timer_arch_enable(void)
{
    /* 校准 LAPIC 频率并启动 Periodic 模式 */
    lapic_timer_init(IDT_LAPIC_TIMER_VEC);
    KLOG_TIMER("LAPIC timer started @ %d Hz\n", TIMER_FREQUENCY_HZ);
}

void timer_arch_disable(void)
{
    lapic_timer_stop();
    KLOG_TIMER("LAPIC timer stopped\n");
}

void timer_arch_set_next_interrupt(uint64_t ticks_from_now)
{
    /* LAPIC Periodic 模式硬件自动重载, 无需软件干预 */
    (void)ticks_from_now;
}

/* 扫描 UART, 向前台进程组发 SIGINT(来自 syscall.c) */
extern void signal_check_uart(void);

/* ── 定时器中断处理函数 ──────────────────────────────────────── */

void timer_handler(void *frame)
{
    (void)frame;

    /* 每个 tick 排空 UART 输入, 确保 Ctrl+C 能及时被检测 */
    signal_check_uart();

    g_system_ticks++;
    g_x86_tick_counter++;

    extern void cpu_bump_local_ticks(void);
    cpu_bump_local_ticks();

    g_timer_stats.total_interrupts++;
    g_timer_stats.last_interrupt_time = timer_get_uptime_ms();

    if (g_x86_tick_counter >= (uint32_t)TIMER_FREQUENCY_HZ) {
        g_x86_uptime_seconds++;
        g_x86_tick_counter = 0;
        g_timer_stats.total_seconds = g_x86_uptime_seconds;
    }

    /* EOI 由 LAPIC dispatch 在 handler 返回后统一发出 */

    /* 调用 tick 回调(调度器 sched_tick) */
    if (g_tick_cb) {
        g_tick_cb();
    }
}

#endif /* __TIMER_X86_64_IMPL_H__ */
