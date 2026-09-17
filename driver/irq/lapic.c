/*
 * driver/irq/lapic.c — x86_64 Local APIC 驱动实现
 *
 * 使用 PIT Channel 2 作为参考时钟来校准 LAPIC timer 频率：
 *   1. 以 PIT 频率（1.193182 MHz）计时 10ms
 *   2. 读取这 10ms 内 LAPIC 计数器的变化量
 *   3. 推算 LAPIC bus 频率，设置 Periodic 模式的初始计数
 *
 * 物理内存映射：LAPIC 基址 0xFEE00000 已在 boot.S 的 1GB huge page
 * 中被映射到虚拟地址 0xffff800000000000 + 0xFEE00000。
 */

#include "lapic.h"
#include "x86_64/io.h"
#include "klog.h"
#include "../timer/timer.h"  /* TIMER_FREQUENCY_HZ = g_timer_cfg_freq_hz */

/* ── LAPIC 虚拟基址 ─────────────────────────────────────────────
 * boot.S 建立了 identity + high-half 两份映射（1GB huge page），
 * 物理 0xFEE00000 对应虚拟 PHYS_OFFSET + 0xFEE00000。
 */
#define PHYS_OFFSET  0xffff800000000000UL
#define LAPIC_VIRT   (PHYS_OFFSET + LAPIC_BASE_PHYS)

/* ── 寄存器访问 ──────────────────────────────────────────────── */

static inline uint32_t lapic_read(uint32_t reg)
{
    volatile uint32_t *p = (volatile uint32_t *)(LAPIC_VIRT + reg);
    return *p;
}

static inline void lapic_write(uint32_t reg, uint32_t val)
{
    volatile uint32_t *p = (volatile uint32_t *)(LAPIC_VIRT + reg);
    *p = val;
}

/* ── PIT Channel 2 辅助（用于校准）─────────────────────────────
 *
 * 只通过 PIT 自身的端口（0x43 命令 / 0x42 数据）读计数器，不碰 port 0x61。
 * port 0x61（gate / OUT 状态）由南桥 PIIX/ICH 提供，QEMU 的
 * -machine microvm 没有南桥，该端口未实现、读回恒为 0xff，任何靠它判断
 * "PIT 计满没有"的等待都会立刻返回（详见 pit_poll_tsc_freq 注释）。
 */

static bool pit_poll_tsc_freq(uint64_t *out_hz);
static bool pit_wait_10ms(void);

/* TSC 频率（Hz），由 lapic_timer_init 在 PIT 校准时同步测量 */
volatile uint64_t g_tsc_freq_hz = 0;

static inline uint64_t _lapic_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* PIT 输入时钟 1.193182 MHz；11932 count ≈ 10ms */
#define PIT_INPUT_HZ     1193182ULL
#define PIT_CAL_TICKS    11932u

/*
 * 低于此值的 TSC 频率一律视为标定失败，不再采用。
 * 正是缺少这道校验，3 MHz 这种明显错误的值才会一路传到
 * CLOCK_MONOTONIC，让整个系统时钟快约 1000 倍。
 */
#define TSC_MIN_PLAUSIBLE_HZ  50000000ULL   /* 50 MHz */

/*
 * pit_start_oneshot - 让 channel 2 以 mode 0 从 0xFFFF 开始倒计数
 *
 * mode 0 (Interrupt on Terminal Count)，命令字 0xB0：
 *   bits[7:6] = 10  (channel 2)
 *   bits[5:4] = 11  (lo/hi byte)
 *   bits[3:1] = 000 (mode 0)
 *   bit[0]   = 0    (binary)
 * 满量程 0xFFFF / 1.193182MHz ≈ 54.9ms。
 */
static void pit_start_oneshot(void)
{
    outb(PIT_CAL_PORT_CMD, 0xB0u);
    outb(PIT_CAL_PORT_CH2, 0xFFu);
    outb(PIT_CAL_PORT_CH2, 0xFFu);
}

/*
 * pit_latch_read - latch channel 2 并读回当前计数值
 *
 * 0x80 = latch channel 2；随后从数据口读两次得到 lo/hi。
 * 全程只用 PIT 自己的端口，不需要 port 0x61。
 */
static uint32_t pit_latch_read(void)
{
    outb(PIT_CAL_PORT_CMD, 0x80u);
    uint32_t lo = inb(PIT_CAL_PORT_CH2);
    uint32_t hi = inb(PIT_CAL_PORT_CH2);
    return (hi << 8) | lo;
}

/*
 * pit_poll_tsc_freq - 用 PIT channel 2 的计数器反推 TSC 频率
 *
 * 做法：把 channel 2 装成满量程 one-shot，自旋固定的一批 TSC tick，
 * 再 latch 读回这段时间里 PIT 走了多少 count，于是
 *     TSC 频率 = 自旋的 TSC tick 数 / (PIT count / 1.193182MHz)
 *
 * 为什么不用 port 0x61 等 10ms 的老办法：port 0x61（PIT gate / OUT 状态）
 * 由南桥 PIIX/ICH 提供，QEMU 的 -machine microvm 没有南桥，该端口未实现、
 * 读回恒为 0xff —— OUT 位一直是 1，等待循环立刻退出，把"10ms"缩成几次
 * port I/O 的时间（实测 ~12us）。TSC 频率因此被算成 3 MHz 而不是
 * ~2.4 GHz，CLOCK_MONOTONIC 于是快了约 1000 倍（epoll_wait04 等
 * 计时类测例全挂）。这里只读 PIT 计数器本身，与 port 0x61 无关。
 *
 * 失败返回 false（PIT 不存在 / 不计数 / 结果不合理），调用者退化为
 * 粗粒度时钟而不是采用一个错的值。
 */
#define PIT_WAIT_TSC_TICKS  2000000ULL   /* 约为毫秒量级，具体取决于 TSC 频率 */

static bool pit_poll_tsc_freq(uint64_t *out_hz)
{
    pit_start_oneshot();

    uint64_t t0 = _lapic_rdtsc();
    uint64_t t1 = t0 + PIT_WAIT_TSC_TICKS;
    while ((int64_t)(_lapic_rdtsc() - t1) < 0)
        ;

    uint32_t now = pit_latch_read();

    /* 计数器纹丝不动 → PIT 没在跑（gate 未拉高或设备不存在） */
    if (now >= 0xFFFEu)
        return false;

    uint32_t elapsed = 0xFFFFu - now;
    if (elapsed < 200u)
        return false;              /* 走得太少，量化误差会很大 */

    uint64_t hz = PIT_WAIT_TSC_TICKS * PIT_INPUT_HZ / (uint64_t)elapsed;
    if (hz < TSC_MIN_PLAUSIBLE_HZ || hz > 20000000000ULL) {
        KLOG_WARN("TSC freq: PIT measurement implausible "
                  "(elapsed_pit=%u -> %llu MHz)\n",
                  elapsed, (unsigned long long)(hz / 1000000ULL));
        return false;
    }

    KLOG_INFO("TSC freq: PIT counter %u counts over %llu tsc ticks\n",
              elapsed, (unsigned long long)PIT_WAIT_TSC_TICKS);
    *out_hz = hz;
    return true;
}

/*
 * pit_wait_10ms - 用 PIT 计数器轮询等待约 10ms（不依赖 port 0x61）
 *
 * 只在 TSC 频率未知时用于量 LAPIC 的 10ms 窗口。轮询周期受 port I/O
 * 开销限制（每次 latch+读两次，微秒量级），精度足够做 10ms 量级的窗口。
 */
static bool pit_wait_10ms(void)
{
    pit_start_oneshot();

    uint32_t start = pit_latch_read();
    if (start >= 0xFFFEu)
        return false;

    for (uint32_t guard = 0; guard < 200000u; guard++) {
        uint32_t now = pit_latch_read();
        if ((uint32_t)(start - now) >= PIT_CAL_TICKS)
            return true;
    }
    return false;
}

/* ── TSC 频率探测 ─────────────────────────────────────────────
 *
 * 先试 CPUID，再退回 PIT 实测：
 *   - CPUID.15H 给出 TSC/晶振比值与晶振频率，结果精确
 *   - CPUID.16H 只给标称基频（MHz），精度差但可用
 *   - 两者都拿不到时用 pit_poll_tsc_freq() 实测
 *
 * 实测：QEMU 8.2 的 -cpu host 在 x86_64 客户机里把 15H/16H 全部置零
 * （宿主本来是 15H=[2,126,38400000]、16H.base=2400），所以本项目的
 * QEMU 场景实际都会走 PIT 那条路；CPUID 这条留着给裸机 / 其它 VMM。
 *
 * 低于 TSC_MIN_PLAUSIBLE_HZ 的结果一律视为标定失败，不再采用。
 */
static inline void
_lapic_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf));
}

static bool
tsc_freq_from_cpuid(uint64_t *out_hz)
{
    uint32_t a, b, c, d, max_leaf;

    _lapic_cpuid(0, &max_leaf, &b, &c, &d);

    if (max_leaf >= 0x15u) {
        _lapic_cpuid(0x15u, &a, &b, &c, &d);
        if (a && b && c) {
            uint64_t hz = (uint64_t)c * (uint64_t)b / (uint64_t)a;
            if (hz >= TSC_MIN_PLAUSIBLE_HZ) {
                *out_hz = hz;
                return true;
            }
        }
    }

    if (max_leaf >= 0x16u) {
        _lapic_cpuid(0x16u, &a, &b, &c, &d);
        if ((a & 0xFFFFu) != 0u) {
            uint64_t hz = (uint64_t)(a & 0xFFFFu) * 1000000ULL;
            if (hz >= TSC_MIN_PLAUSIBLE_HZ) {
                *out_hz = hz;
                return true;
            }
        }
    }

    /* 报失败时连同原始 CPUID 值一起打，便于判断是"叶不存在"还是"值不可用" */
    uint32_t a15 = 0, b15 = 0, c15 = 0, d15 = 0, a16 = 0;
    if (max_leaf >= 0x15u) {
        _lapic_cpuid(0x15u, &a15, &b15, &c15, &d15);
    }
    if (max_leaf >= 0x16u) {
        _lapic_cpuid(0x16u, &a16, &b, &c, &d);
    }
    KLOG_DEBUG("TSC freq: CPUID inconclusive (max_leaf=0x%x "
               "15H=[%u,%u,%u] 16H.base=%u MHz), trying PIT\n",
               max_leaf, a15, b15, c15, a16 & 0xFFFFu);

    return false;
}

/* ── lapic_init ─────────────────────────────────────────────── */

void lapic_init(void)
{
    /* 设置 Task Priority 为 0（接收所有中断） */
    lapic_write(LAPIC_REG_TPR, 0);

    /* 设置 Spurious Vector Register：使能 APIC + 伪中断向量 0xFF */
    lapic_write(LAPIC_REG_SVR, LAPIC_SVR_ENABLE | 0xFFu);

    KLOG_INFO("LAPIC init: ID=0x%x, ver=0x%x\n",
              lapic_read(LAPIC_REG_ID) >> 24,
              lapic_read(LAPIC_REG_VER) & 0xFF);
}

uint32_t lapic_id(void)
{
    return lapic_read(LAPIC_REG_ID) >> 24;
}

static void lapic_wait_icr_idle(void)
{
    while (lapic_read(LAPIC_REG_ICR_LOW) & (1u << 12))
        ;
}

void lapic_send_init(uint32_t apic_id)
{
    lapic_wait_icr_idle();
    lapic_write(LAPIC_REG_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, 0x00004500u); /* INIT, level assert, physical */
    lapic_wait_icr_idle();
}

void lapic_send_sipi(uint32_t apic_id, uint8_t vector)
{
    lapic_wait_icr_idle();
    lapic_write(LAPIC_REG_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, 0x00004600u | vector); /* STARTUP IPI */
    lapic_wait_icr_idle();
}

/* ── lapic_timer_init ───────────────────────────────────────── */

void lapic_timer_init(uint8_t vector)
{
    /* 分频 = 16，减少误差 */
    lapic_write(LAPIC_REG_TIMER_DCR, LAPIC_TIMER_DIV_16);

    /* 先屏蔽 timer，避免校准期间产生中断 */
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_MASKED | vector);

    /*
     * 确定 TSC 频率：先 CPUID，失败才退回 PIT 计时反推。
     * tsc_freq_hz == 0 表示两者都不可用，此时 timer_counter_to_ns() 会
     * 退化成按 g_system_ticks 算的粗粒度时钟（量级正确，只是分辨率差），
     * 比用一个错的值好得多。
     */
    const char *tsc_src = "none";
    uint64_t tsc_freq_hz = 0;

    if (tsc_freq_from_cpuid(&tsc_freq_hz)) {
        tsc_src = "cpuid";
    } else if (pit_poll_tsc_freq(&tsc_freq_hz)) {
        tsc_src = "pit";
    }

    g_tsc_freq_hz = tsc_freq_hz;

    if (tsc_freq_hz)
        KLOG_INFO("TSC frequency: %llu MHz (source=%s)\n",
                  tsc_freq_hz / 1000000ULL, tsc_src);
    else
        KLOG_WARN("TSC frequency unknown; falling back to tick-granularity "
                  "monotonic clock (CLOCK_MONOTONIC resolution = 1 tick)\n");

    /* ── 校准：测量 10ms 内 LAPIC 计数器减少了多少 ── */
    lapic_write(LAPIC_REG_TIMER_ICR, 0xFFFFFFFFu);  /* 设置最大初始值开始倒计数 */

    uint64_t tsc_before = _lapic_rdtsc();
    if (tsc_freq_hz) {
        /* 已知 TSC 频率：直接用它精确等 10ms，不再依赖 PIT */
        uint64_t target = tsc_before + tsc_freq_hz / 100ULL;
        while ((int64_t)(_lapic_rdtsc() - target) < 0)
            ;
    } else {
        /* 没有可用的 TSC 频率：只能用 PIT 计数器尽力等一个 10ms 量级 */
        if (!pit_wait_10ms())
            KLOG_WARN("LAPIC calibration window unavailable (no TSC freq, "
                      "no usable PIT); tick rate may be off\n");
    }
    uint64_t tsc_after = _lapic_rdtsc();

    uint32_t ticks_in_10ms = 0xFFFFFFFFu - lapic_read(LAPIC_REG_TIMER_CCR);

    KLOG_INFO("LAPIC timer: %u ticks in 10ms (div=16, tsc_delta=%llu)\n",
              ticks_in_10ms, (unsigned long long)(tsc_after - tsc_before));

    /* 计算每次 tick 中断所需的计数值
     * TIMER_FREQUENCY_HZ = 中断频率 (如 100 Hz → 10ms/tick)
     * ticks_per_interrupt = ticks_in_10ms * 10ms / (1000ms / TIMER_FREQUENCY_HZ)
     * 即: ticks_per_interrupt = ticks_in_10ms * TIMER_FREQUENCY_HZ / 100
     */
    uint32_t ticks_per_interrupt = ticks_in_10ms * TIMER_FREQUENCY_HZ / 100u;
    if (ticks_per_interrupt == 0)
        ticks_per_interrupt = ticks_in_10ms;   /* 保底 */

    KLOG_INFO("LAPIC timer: %u ticks/interrupt @ %d Hz\n",
              ticks_per_interrupt, TIMER_FREQUENCY_HZ);

    /* 配置 Periodic 模式 + 向量 */
    lapic_write(LAPIC_REG_TIMER_ICR, ticks_per_interrupt);
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_PERIODIC | vector);
}

/* ── lapic_eoi ──────────────────────────────────────────────── */

void lapic_eoi(void)
{
    lapic_write(LAPIC_REG_EOI, 0);
}

/* ── lapic_timer_stop ────────────────────────────────────────── */

void lapic_timer_stop(void)
{
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_MASKED);
}
