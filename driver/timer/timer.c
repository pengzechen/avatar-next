#include "timer.h"
#include "klog.h"
#include "platform_cfg.h"

// ============================================================
// 全局变量定义（在架构实现包含之前，使 timer_handler 可见）
// ============================================================
volatile uint64_t g_system_ticks    = 0;
volatile uint64_t g_timer_frequency = 0;
timer_stats_t     g_timer_stats     = {0};

/* 定时器配置（由 timer_init() 填充） */
unsigned  g_timer_cfg_freq_hz    = 100;
unsigned  g_timer_cfg_tick_ms    = 10;
uintptr_t g_timer_cfg_counter_hz = 0;
unsigned  g_timer_cfg_cntp       = 0;

/* tick 回调（调度器通过 timer_set_tick_cb 注册）*/
static timer_tick_cb_t g_tick_cb = NULL;

void
timer_set_tick_cb(timer_tick_cb_t cb)
{
    g_tick_cb = cb;
}

// RISC-V 特定的全局变量
#if ARCH_RISCV64
volatile uint64_t g_uptime_seconds = 0;
volatile uint32_t g_tick_counter   = 0;
#endif

// ============================================================
// 包含架构特定实现
// ============================================================
#if ARCH_AARCH64
    #include "timer_aarch64_impl.h"
#elif ARCH_RISCV64
    #include "timer_riscv64_impl.h"
#elif ARCH_X86_64
    #include "timer_x86_64_impl.h"
#else
    #error "Unsupported architecture"
#endif

#if ARCH_X86_64
extern volatile uint64_t g_tsc_freq_hz;
#endif

static uint64_t timer_delay_counter_frequency(void)
{
#if ARCH_X86_64
    return g_tsc_freq_hz;
#else
    return timer_counter_frequency();
#endif
}

// ============================================================
// 通用接口实现
// ============================================================

// 初始化定时器
void
timer_init(void)
{
    /* 从 Lua 配置中读取定时器参数 */
    g_timer_cfg_freq_hz    = platform_get_uint("timer", "freq_hz");
    g_timer_cfg_tick_ms    = platform_get_uint("timer", "tick_ms");
    g_timer_cfg_counter_hz = platform_get_uintptr("timer", "counter_hz");
    g_timer_cfg_cntp       = platform_get_uint("timer", "cntp");

    // 调用架构特定的初始化
    timer_arch_init();

    KLOG_INFO("Timer initialized successfully\n");
}

// 启用定时器
void
timer_enable(void)
{
    timer_arch_enable();
}

void
timer_init_secondary(void)
{
#if ARCH_AARCH64
    /*
     * 次级核需要独立 enable 本地 banked PPI（GICv2 GICD_ISENABLER0 每 CPU 独立）。
     * 主核 timer_arch_init 已 irq_enable_irq(CNTP_TIMER)，但只对主核生效。
     */
    irq_enable_irq(CNTP_TIMER);
    uint64_t ticks_per_interrupt = g_timer_frequency / TIMER_FREQUENCY_HZ;
    WRITE_CNTP_TVAL_EL0(ticks_per_interrupt);
    WRITE_CNTP_CTL_EL0(1); /* enable, unmask */
#elif ARCH_RISCV64
    timer_arch_enable();
#elif ARCH_X86_64
    timer_arch_enable();
#endif
}

// 禁用定时器
void
timer_disable(void)
{
    timer_arch_disable();
}

// 设置下一次中断
void
timer_set_next_interrupt(uint64_t ticks_from_now)
{
    timer_arch_set_next_interrupt(ticks_from_now);
}

// 调度下一个tick
void
timer_schedule_next_tick(void)
{
    uint64_t ticks_per_interrupt = g_timer_frequency / TIMER_FREQUENCY_HZ;
    timer_set_next_interrupt(ticks_per_interrupt);
}

// ============================================================
// 时间相关函数
// ============================================================

uint64_t
timer_read_counter(void)
{
#if ARCH_AARCH64
    return READ_CNTPCT_EL0();
#elif ARCH_RISCV64
    return READ_TIME();
#elif ARCH_X86_64
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
#endif
}

uint64_t
timer_counter_frequency(void)
{
#if ARCH_X86_64
    if (g_tsc_freq_hz)
        return g_tsc_freq_hz;
    uint64_t tick_ms = g_timer_cfg_tick_ms ? g_timer_cfg_tick_ms : 10ULL;
    return 1000ULL / tick_ms;
#else
    if (g_timer_frequency)
        return g_timer_frequency;
    if (g_timer_cfg_counter_hz)
        return g_timer_cfg_counter_hz;
#if ARCH_RISCV64
    return 10000000ULL;
#elif ARCH_AARCH64
    return 62500000ULL;
#endif
#endif
}

uint64_t
timer_counter_to_ns(uint64_t ticks)
{
#if ARCH_X86_64
    if (!g_tsc_freq_hz) {
        uint64_t tick_ms = g_timer_cfg_tick_ms ? g_timer_cfg_tick_ms : 10ULL;
        return g_system_ticks * tick_ms * 1000000ULL;
    }
#endif
    uint64_t freq = timer_counter_frequency();
    if (!freq)
        return 0;
    return (ticks / freq) * 1000000000ULL
         + (ticks % freq) * 1000000000ULL / freq;
}

void
timer_spin(uint32_t iterations)
{
    for (uint32_t i = 0; i < iterations; i++)
        asm volatile("nop");
}

int
timer_poll_until(timer_poll_predicate_t pred, void *ctx,
                 uint32_t max_polls, uint32_t relax_iters)
{
    if (!pred)
        return -1;

    for (uint32_t i = 0; i < max_polls; i++) {
        if (pred(ctx))
            return 0;
        timer_spin(relax_iters);
    }

    return -1;
}

int
timer_poll_until_us(timer_poll_predicate_t pred, void *ctx,
                    uint64_t timeout_us, uint32_t relax_iters)
{
    if (!pred)
        return -1;

    uint64_t freq = timer_counter_frequency();
    if (!freq)
        return timer_poll_until(pred, ctx, (uint32_t)timeout_us, relax_iters);

    uint64_t start = timer_read_counter();
    uint64_t timeout_ticks = (freq / 1000000ULL) * timeout_us
                           + (freq % 1000000ULL) * timeout_us / 1000000ULL;

    do {
        if (pred(ctx))
            return 0;
        timer_spin(relax_iters);
    } while ((timer_read_counter() - start) < timeout_ticks);

    return pred(ctx) ? 0 : -1;
}

// 获取系统tick数
uint64_t
timer_get_system_ticks(void)
{
    return g_system_ticks;
}

// 获取系统运行时间（毫秒）
uint64_t
timer_get_uptime_ms(void)
{
    return g_system_ticks * TIMER_TICK_MS;
}

// 获取定时器频率
uint64_t
timer_get_frequency(void)
{
    return g_timer_frequency;
}

// 毫秒延时（忙等待）
void
timer_delay_ms(uint32_t ms)
{
    uint64_t freq = timer_delay_counter_frequency();
    if (!freq) {
        uint64_t start = timer_get_uptime_ms();
        while ((timer_get_uptime_ms() - start) < ms)
            timer_spin(1);
        return;
    }

    uint64_t start_time  = timer_read_counter();
    uint64_t delay_ticks = (freq * ms) / 1000;
    uint64_t target_time = start_time + delay_ticks;

    while ((int64_t)(timer_read_counter() - target_time) < 0)
        timer_spin(1);
}

// 微秒延时（忙等待）
void
timer_delay_us(uint32_t us)
{
    uint64_t freq = timer_delay_counter_frequency();
    if (!freq) {
        timer_delay_ms((us + 999U) / 1000U);
        return;
    }

    uint64_t start_time  = timer_read_counter();
    uint64_t delay_ticks = (freq / 1000000ULL) * us
                         + (freq % 1000000ULL) * us / 1000000ULL;
    uint64_t target_time = start_time + delay_ticks;

    while ((int64_t)(timer_read_counter() - target_time) < 0)
        timer_spin(1);
}

// ============================================================
// 统计相关函数
// ============================================================

// 获取统计信息
void
timer_get_stats(timer_stats_t *stats)
{
    if (stats) {
        *stats = g_timer_stats;
    }
}

// 重置统计信息
void
timer_reset_stats(void)
{
    g_timer_stats.total_interrupts    = 0;
    g_timer_stats.total_schedules     = 0;
    g_timer_stats.total_seconds       = 0;
    g_timer_stats.last_interrupt_time = 0;
}

// 打印定时器信息
void
timer_dump_info(void)
{
    KLOG_INFO("Timer Information:\n");
    KLOG_INFO("  Frequency: %llu Hz\n", g_timer_frequency);
    KLOG_INFO("  System ticks: %llu\n", g_system_ticks);
    KLOG_INFO("  Uptime: %llu ms\n", timer_get_uptime_ms());

    KLOG_INFO("Timer Statistics:\n");
    KLOG_INFO("  Total interrupts: %llu\n", g_timer_stats.total_interrupts);
    KLOG_INFO("  Total schedules: %llu\n", g_timer_stats.total_schedules);
    KLOG_INFO("  Total seconds: %llu\n", g_timer_stats.total_seconds);
}
