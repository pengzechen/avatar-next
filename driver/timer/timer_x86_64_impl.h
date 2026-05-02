#ifndef __TIMER_X86_64_IMPL_H__
#define __TIMER_X86_64_IMPL_H__

/*
 * x86_64 定时器实现 — 使用 LAPIC Timer (Periodic 模式)
 *
 * 频率校准：lapic_timer_init 内部用 PIT Channel 2 校准 LAPIC bus 频率，
 * 对外只需指定目标中断频率（TIMER_FREQUENCY_HZ，来自 driver_cfg.h）。
 *
 * 函数签名与 AArch64/RISC-V impl 一致（无 static inline），
 * 由 timer.c #include 本文件来获得定义。
 */

#include "timer.h"
#include "klog.h"
#include "exception.h"     /* irq_install, IDT_LAPIC_TIMER_VEC */
#include "irq/lapic.h"     /* lapic_timer_init, lapic_eoi      */

/* ── x86_64 特定全局变量 ────────────────────────────────────── */

volatile uint64_t g_x86_uptime_seconds = 0;
static   uint32_t g_x86_tick_counter   = 0;

/* 前向声明（定义在本文件末尾）*/
void timer_handler(void *frame);

/* ── 架构特定操作实现 ────────────────────────────────────────── */

void
timer_arch_init(void)
{
    /* x86_64 上 LAPIC 是计时源，实际频率由校准决定；
     * g_timer_frequency 设一个占位值（校准后 lapic.c 内部使用）*/
    g_timer_frequency = 0;   /* 将由 lapic_timer_init 校准 */

    KLOG_INFO("Timer initialization (x86_64 LAPIC):\n");
    KLOG_INFO("  Target frequency: %d Hz\n", TIMER_FREQUENCY_HZ);
    KLOG_INFO("  Tick interval: %d ms\n", TIMER_TICK_MS);

    timer_reset_stats();

    /* 注册 LAPIC Timer 中断处理函数到 IDT 向量 0x20 */
    irq_install(IDT_LAPIC_TIMER_VEC, (irq_handler_t)timer_handler);
}

void
timer_arch_enable(void)
{
    /* 校准 LAPIC 频率并启动 Periodic 模式 */
    lapic_timer_init(IDT_LAPIC_TIMER_VEC);
    KLOG_INFO("LAPIC timer started @ %d Hz\n", TIMER_FREQUENCY_HZ);
}

void
timer_arch_disable(void)
{
    lapic_timer_stop();
    KLOG_INFO("LAPIC timer stopped\n");
}

void
timer_arch_set_next_interrupt(uint64_t ticks_from_now)
{
    /* LAPIC Periodic 模式硬件自动重载，无需软件干预 */
    (void)ticks_from_now;
}

/* ── 定时器中断处理函数 ──────────────────────────────────────── */

void
timer_handler(void *frame)
{
    (void)frame;

    g_system_ticks++;
    g_x86_tick_counter++;

    g_timer_stats.total_interrupts++;
    g_timer_stats.last_interrupt_time = timer_get_uptime_ms();

    if (g_x86_tick_counter >= (uint32_t)TIMER_FREQUENCY_HZ) {
        g_x86_uptime_seconds++;
        g_x86_tick_counter = 0;
        g_timer_stats.total_seconds = g_x86_uptime_seconds;

        // KLOG_INFO("System running - Uptime: %llus\n", g_x86_uptime_seconds);
    }

    /* LAPIC 必须手动发 EOI */
    lapic_eoi();

    // 调用 tick 回调（调度器 sched_tick）
    if (g_tick_cb) {
        g_tick_cb();
    }
}

#endif /* __TIMER_X86_64_IMPL_H__ */

