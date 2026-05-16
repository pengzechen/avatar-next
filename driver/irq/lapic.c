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
 * Port 0x61 bit0 = Gate，bit5 = OUT（读计数状态用）
 * Channel 2 one-shot 模式：写入 reload 值后倒计数，OUT 从高变低时停止。
 */

/* 等待约 10ms（PIT 11932 tick ≈ 10ms at 1.193182 MHz）*/
#define PIT_CAL_TICKS  11932u

static void pit_calibrate_delay(void)
{
    /*
     * 使用 Mode 0 (Interrupt on Terminal Count)：
     *   - 写命令后 OUT 立即变 LOW
     *   - 计数结束（terminal count）后 OUT 变 HIGH
     *
     * 命令字 0xB0 = 1011_0000：
     *   bits[7:6] = 10  (channel 2)
     *   bits[5:4] = 11  (lo/hi byte)
     *   bits[3:1] = 000 (mode 0)
     *   bit[0]   = 0   (binary)
     */
    outb(PIT_CAL_PORT_CMD, 0xB0u);

    outb(PIT_CAL_PORT_CH2, (uint8_t)(PIT_CAL_TICKS & 0xFFu));
    outb(PIT_CAL_PORT_CH2, (uint8_t)(PIT_CAL_TICKS >> 8));

    /* 确保 Gate=1（port 0x61 bit0）；mode 0 需要 gate 为高才计数 */
    outb(PIT_CAL_PORT_CTRL, inb(PIT_CAL_PORT_CTRL) | 0x01u);

    /*
     * 等待 OUT 变高（port 0x61 bit5 = channel 2 OUT）。
     * mode 0：写命令后 OUT=LOW；terminal count 时 OUT=HIGH。
     */
    while ((inb(PIT_CAL_PORT_CTRL) & 0x20u) == 0)
        ;
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

/* ── lapic_timer_init ───────────────────────────────────────── */

void lapic_timer_init(uint8_t vector)
{
    /* 分频 = 16，减少误差 */
    lapic_write(LAPIC_REG_TIMER_DCR, LAPIC_TIMER_DIV_16);

    /* 先屏蔽 timer，避免校准期间产生中断 */
    lapic_write(LAPIC_REG_LVT_TIMER, LAPIC_TIMER_MASKED | vector);

    /* ── 校准：测量 10ms 内 LAPIC 计数器减少了多少 ── */
    lapic_write(LAPIC_REG_TIMER_ICR, 0xFFFFFFFFu);  /* 设置最大初始值开始倒计数 */

    pit_calibrate_delay();   /* 等待约 10ms */

    uint32_t ticks_in_10ms = 0xFFFFFFFFu - lapic_read(LAPIC_REG_TIMER_CCR);

    KLOG_INFO("LAPIC timer: %u ticks in 10ms (div=16)\n", ticks_in_10ms);

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
