#ifndef __TIMER_RISCV64_IMPL_H__
#define __TIMER_RISCV64_IMPL_H__

#include "timer.h"
#include "sysreg.h"
#include "klog.h"
#include "exception.h"   /* irq_install, CAUSE_SUPERVISOR_TIMER */

/* SBI v0.2+ TIME extension and legacy timer extension IDs */
#define SBI_EID_TIME            0x54494D45UL
#define SBI_FID_SET_TIMER       0UL
#define SBI_LEGACY_SET_TIMER    0UL

// ============================================================
// RISC-V 定时器相关宏（仅保留 sysreg.h 中没有的）
// ============================================================
#define READ_TIME()         CSR_READ(time)   /* sysreg.h 中同名，内容相同 */

/* TIMER_FREQ_HZ 由 platform_cfg.h 提供（= g_timer_counter_hz，从平台配置扫描）*/

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

/*
 * ── tick 的绝对基准 ────────────────────────────────────────────────────
 *
 * 原本的做法是"从现在起一个周期"，即 next = READ_TIME() + period。那样 ISR
 * 每迟到一次，下一个 tick 就再顺延一次 —— **误差累积、不会自校正**，负载下
 * 内核时钟能慢到 1/1.5（实测：bwtest 在 100Mbps 链路上报出 140.7 Mbps，
 * 而那超过物理上限 95.5；反推真实窗口是 1.47s 而不是它以为的 1.0s）。
 *
 * 现在改成两者都从同一个绝对原点推出：
 *
 *     g_system_ticks = (READ_TIME() - s_tick_base) / s_tick_step
 *     下一个截止时间  = s_tick_base + (g_system_ticks + 1) * s_tick_step
 *
 * 两个要点：
 *   1. tick **计数**也按真实时间算，而不是自增 —— 否则计数照样少算，而
 *      timer_get_uptime_ms() 就是它乘出来的，时钟照样走慢。
 *   2. 由取整的性质，(ticks+1)*step 必然大于 now-base，所以下一个截止时间
 *      **永远在未来**，最多差一个周期 —— 不会因为追赶而连发中断。
 */
static uint64_t s_tick_base;   /* 时间原点（READ_TIME 的刻度）      */
static uint64_t s_tick_step;   /* 一个 tick 周期折合多少计数器刻度  */

void
timer_arch_enable(void)
{
    // 计算下一次中断的时间
    uint64_t ticks_per_interrupt = g_timer_frequency / TIMER_FREQUENCY_HZ;

    KLOG_INFO("Timer enabled with %llu ticks per interrupt\n", ticks_per_interrupt);

    /*
     * 设时间原点。用 "now - 已过周期数*step" 而不是直接取 now：
     * disable/enable 再次调用时（SMP 启动路径）g_system_ticks 不会倒退。
     */
    s_tick_step = ticks_per_interrupt;
    s_tick_base = READ_TIME() -
                  (s_tick_step ? g_system_ticks * s_tick_step : 0ULL);

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
    uint64_t step = s_tick_step ? s_tick_step : ticks_from_now;
    uint64_t next_time;

    if (step == 0ULL) {
        /*
         * 兜底：频率未知（s_tick_step 还没设）。退化成老行为 —— 会漂，
         * 但至少能走。正常情况下 timer_arch_enable 已经设好了。
         */
        next_time = READ_TIME() + ticks_from_now;
    } else {
        /*
         * 绝对截止时间：从时间原点 + 整数个周期算出来，**不是从现在起算**。
         * ISR 迟到只影响本次，不会累积到下一次。
         */
        next_time = s_tick_base + (g_system_ticks + 1ULL) * step;
    }

    /*
     * Prefer SBI v0.2+ TIME extension first; if firmware does not support it,
     * fall back to the legacy timer extension.
     */
    register uint64_t a0 asm("a0") = next_time;
    register uint64_t a1 asm("a1") = 0;
    register uint64_t a6 asm("a6") = SBI_FID_SET_TIMER;
    register uint64_t a7 asm("a7") = SBI_EID_TIME;

    asm volatile(
        "ecall"
        : "+r"(a0), "+r"(a1)
        : "r"(a6), "r"(a7)
        : "memory"
    );

    if ((long)a0 != 0) {
        /* Legacy SBI: EID=0, arg0=stime_value */
        register uint64_t l_a0 asm("a0") = next_time;
        register uint64_t l_a7 asm("a7") = SBI_LEGACY_SET_TIMER;

        asm volatile(
            "ecall"
            : "+r"(l_a0)
            : "r"(l_a7)
            : "a1", "a2", "a3", "a4", "a5", "a6", "memory"
        );
    }
}

// ============================================================
// 定时器中断处理函数
// ============================================================

/* 扫描 UART，向前台进程组发 SIGINT（来自 syscall.c） */
extern void signal_check_uart(void);

void
timer_handler(void *frame)
{
    trap_frame_t *tf = (trap_frame_t *)frame;

    /*
     * S-mode IRQ can arrive at arbitrary kernel instructions. Keep that path
     * short and avoid signal/TTY work there until kernel preemption and locks
     * are fully audited. U-mode timer IRQ keeps the old responsive Ctrl+C path.
     */
    if (!tf || !(tf->sstatus & SSTATUS_SPP))
        signal_check_uart();

    /*
     * 更新系统 tick 计数。
     *
     * **按真实时间算，不能自增**：ISR 迟到时自增会少算，而
     *     timer_get_uptime_ms() = g_system_ticks * TIMER_TICK_MS
     * 就是它乘出来的 —— 少算就等于内核时钟走慢（实测负载下慢到 1/1.5，
     * 会让 bwtest 在 100Mbps 链路上报出 140Mbps）。除以 s_tick_step 得到
     * "已过多少个周期"，迟到的部分天然被补回来。
     */
    if (s_tick_step != 0ULL) {
        uint64_t hz = TIMER_FREQUENCY_HZ;

        g_system_ticks  = (READ_TIME() - s_tick_base) / s_tick_step;
        g_tick_counter  = hz ? (uint32_t)(g_system_ticks % hz) : 0U;
        g_uptime_seconds = hz ? (g_system_ticks / hz) : 0ULL;
    } else {
        /* 兜底：频率未知，退化成老行为 */
        g_system_ticks++;
        g_tick_counter++;
        if (g_tick_counter >= TIMER_FREQUENCY_HZ) {
            g_uptime_seconds++;
            g_tick_counter = 0;
        }
    }

    extern void cpu_bump_local_ticks(void);
    cpu_bump_local_ticks();

    // 更新统计信息
    g_timer_stats.total_interrupts++;
    g_timer_stats.last_interrupt_time = timer_get_uptime_ms();
    g_timer_stats.total_seconds = g_uptime_seconds;

    // 调度下一个tick
    timer_schedule_next_tick();

    // 调用 tick 回调（调度器 sched_tick）
    if (g_tick_cb) {
        g_tick_cb();
    }
}

#endif /* __TIMER_RISCV64_IMPL_H__ */
