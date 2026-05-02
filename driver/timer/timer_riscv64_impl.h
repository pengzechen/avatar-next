#ifndef __TIMER_RISCV64_IMPL_H__
#define __TIMER_RISCV64_IMPL_H__

#include "timer.h"
#include "sysreg.h"
#include "klog.h"
#include "exception.h"   /* irq_install, CAUSE_SUPERVISOR_TIMER */

// ============================================================
// RISC-V 定时器相关宏（仅保留 sysreg.h 中没有的）
// ============================================================
#define READ_TIME()         CSR_READ(time)   /* sysreg.h 中同名，内容相同 */

// 定时器频率配置 (QEMU RISC-V virt 机器通常是 10MHz)
#define TIMER_FREQ_HZ 10000000UL  // 10MHz

// ============================================================
// RISC-V 特定的全局变量
// ============================================================
extern volatile uint64_t g_uptime_seconds;   // 系统运行时间（秒）
extern volatile uint32_t g_tick_counter;     // tick计数器，用于计算秒

/* 前向声明在 timer.h 中已存在，无需重复 */

// ============================================================
// 架构特定操作实现
// ============================================================

void
timer_arch_init(void)
{
    // 设置定时器频率
    g_timer_frequency = TIMER_FREQ_HZ;

    KLOG_INFO("Timer initialization (RISC-V):\n");
    KLOG_INFO("  Timer frequency: %llu Hz\n", g_timer_frequency);
    KLOG_INFO("  Target frequency: %d Hz\n", TIMER_FREQUENCY_HZ);
    KLOG_INFO("  Tick interval: %d ms\n", TIMER_TICK_MS);

    // 禁用定时器
    timer_disable();

    // 清零统计信息
    timer_reset_stats();

    // 注册 Supervisor Timer 中断处理函数（scause=5）
    irq_install(CAUSE_SUPERVISOR_TIMER, (irq_handler_t)timer_handler);
}

void
timer_arch_enable(void)
{
    // 计算下一次中断的时间
    uint64_t ticks_per_interrupt = g_timer_frequency / TIMER_FREQUENCY_HZ;

    KLOG_INFO("Timer enabled with %llu ticks per interrupt\n", ticks_per_interrupt);

    // 首先设置下一次中断时间
    timer_arch_set_next_interrupt(ticks_per_interrupt);

    // 然后启用Machine模式定时器中断
    uint64_t sie = READ_SIE();
    sie |= SIE_STIE;  // 启用机器定时器中断
    CSR_WRITE(sie, sie);

    KLOG_INFO("Machine timer interrupt enabled\n");
}

void
timer_arch_disable(void)
{
    // 禁用Machine模式定时器中断
    uint64_t sie = READ_SIE();
    sie &= ~SIE_STIE;  // 禁用机器定时器中断
    CSR_WRITE(sie, sie);

    KLOG_INFO("Timer disabled\n");
}

void
timer_arch_set_next_interrupt(uint64_t ticks_from_now)
{
    uint64_t current_time = READ_TIME();
    uint64_t next_time = current_time + ticks_from_now;

    // 使用 SBI 调用设置定时器
    // SBI call: sbi_set_timer(uint64_t stime_value)
    // a7 = 0 (EID: Timer Extension)
    // a6 = 0 (FID: sbi_set_timer)
    // a0 = stime_value
    register uint64_t a0 asm("a0") = next_time;
    register uint64_t a6 asm("a6") = 0;  // FID: sbi_set_timer
    register uint64_t a7 asm("a7") = 0;  // EID: Timer Extension

    asm volatile(
        "ecall"
        : "+r"(a0)
        : "r"(a6), "r"(a7)
        : "memory"
    );
}

// ============================================================
// 定时器中断处理函数
// ============================================================
void
timer_handler(void *frame)
{
    (void)frame;  // 抑制未使用参数警告

    // 更新系统tick计数
    g_system_ticks++;
    g_tick_counter++;

    // 更新统计信息
    g_timer_stats.total_interrupts++;
    g_timer_stats.last_interrupt_time = timer_get_uptime_ms();

    // 每100个tick（1秒）更新秒数计数器
    if (g_tick_counter >= TIMER_FREQUENCY_HZ) {
        g_uptime_seconds++;
        g_tick_counter = 0;
        g_timer_stats.total_seconds = g_uptime_seconds;

        // 每秒输出一句话
        // KLOG_INFO("System running - Uptime: %llus\n", g_uptime_seconds);
    }

    // 调度下一个tick
    timer_schedule_next_tick();
}

#endif /* __TIMER_RISCV64_IMPL_H__ */
