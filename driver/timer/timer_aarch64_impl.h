#ifndef __TIMER_AARCH64_IMPL_H__
#define __TIMER_AARCH64_IMPL_H__

#include "timer.h"
#include "sysreg.h"
#include "mmio.h"
#include "irq/irq.h"
#include "klog.h"

// ============================================================
// 定时器寄存器访问宏
// ============================================================
#define CNTFRQ_EL0_READ()        READ_CNTFRQ_EL0()
#define CNTPCT_EL0_READ()        READ_CNTPCT_EL0()
#define CNTP_CTL_EL0_READ()      READ_CNTP_CTL_EL0()
#define CNTP_CTL_EL0_WRITE(val)  WRITE_CNTP_CTL_EL0(val)
#define CNTP_TVAL_EL0_WRITE(val) WRITE_CNTP_TVAL_EL0(val)

// 定时器控制寄存器位定义
#define CNTV_CTL_ENABLE  (1 << 0)  // 使能定时器
#define CNTV_CTL_IMASK   (1 << 1)  // 中断屏蔽
#define CNTV_CTL_ISTATUS (1 << 2)  // 中断状态


// ============================================================
// 架构特定操作实现
// ============================================================

void
timer_arch_init(void)
{
    // 读取定时器频率
    g_timer_frequency = CNTFRQ_EL0_READ();

    KLOG_INFO("Timer initialization (AArch64):\n");
    KLOG_INFO("  Timer frequency: %llu Hz\n", g_timer_frequency);
    KLOG_INFO("  Target frequency: %d Hz\n", TIMER_FREQUENCY_HZ);
    KLOG_INFO("  Tick interval: %d ms\n", TIMER_TICK_MS);

    // 禁用定时器
    timer_disable();

    // 清零统计信息
    timer_reset_stats();

    // 配置 GIC 中断
    // PPI (Private Peripheral Interrupt) 需要设置优先级
    extern void gic_set_ipriority(uint32_t, uint32_t);
    gic_set_ipriority(CNTP_TIMER, 0x00);  // 设置优先级

    // 安装定时器中断处理函数
    extern void irq_install(int, void (*)(uint64_t *));
    irq_install(CNTP_TIMER, timer_handler);
    irq_enable_irq(CNTP_TIMER);

    KLOG_INFO("Timer IRQ %d installed and enabled\n", CNTP_TIMER);
}

void
timer_arch_enable(void)
{
    // 计算下一次中断的时间
    uint64_t ticks_per_interrupt = g_timer_frequency / TIMER_FREQUENCY_HZ;

    // 设置定时器值
    CNTP_TVAL_EL0_WRITE(ticks_per_interrupt);

    // 启用定时器，不屏蔽中断
    CNTP_CTL_EL0_WRITE(CNTV_CTL_ENABLE);

    KLOG_INFO("Timer enabled with %llu ticks per interrupt\n", ticks_per_interrupt);
    KLOG_INFO("Timer CTL: 0x%x, TVAL: %llu\n", CNTP_CTL_EL0_READ(), ticks_per_interrupt);
}

void
timer_arch_disable(void)
{
    // 禁用定时器并屏蔽中断
    CNTP_CTL_EL0_WRITE(CNTV_CTL_IMASK);

    KLOG_INFO("Timer disabled\n");
}

void
timer_arch_set_next_interrupt(uint64_t ticks_from_now)
{
    CNTP_TVAL_EL0_WRITE(ticks_from_now);
}

// ============================================================
// 定时器中断处理函数
// ============================================================
void
timer_handler(uint64_t *stack_pointer)
{
    (void)stack_pointer;  // Suppress unused parameter warning

    // 更新系统tick计数
    g_system_ticks++;

    // 更新统计信息
    g_timer_stats.total_interrupts++;
    g_timer_stats.last_interrupt_time = timer_get_uptime_ms();

    // 调度下一个tick
    timer_schedule_next_tick();

    // 调用 tick 回调（调度器 sched_tick）
    if (g_tick_cb) {
        g_tick_cb();
    }
}

#endif /* __TIMER_AARCH64_IMPL_H__ */
