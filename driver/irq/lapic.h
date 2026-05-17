#ifndef LAPIC_H
#define LAPIC_H

/*
 * driver/irq/lapic.h — x86_64 Local APIC 驱动
 *
 * LAPIC 通过 MMIO 访问（物理基址默认 0xFEE00000）。
 * 这里只实现单核所需的最小子集：
 *   - LAPIC 初始化（使能、设置 SVR）
 *   - LAPIC Timer 配置（Periodic 模式，使用 PIT 校准频率）
 *   - EOI（End-Of-Interrupt）
 */

#include "types.h"

/* ── LAPIC MMIO 寄存器偏移 ─────────────────────────────────────
 * 物理基址默认 0xFEE00000（可由 IA32_APIC_BASE MSR 修改）
 * 所有寄存器 32-bit 宽，每个占 16 字节（步进 0x10）
 */
#define LAPIC_BASE_PHYS         0xFEE00000UL

#define LAPIC_REG_ID            0x020u   /* APIC ID                      */
#define LAPIC_REG_VER           0x030u   /* APIC Version                 */
#define LAPIC_REG_TPR           0x080u   /* Task Priority                */
#define LAPIC_REG_SVR           0x0F0u   /* Spurious Interrupt Vector    */
#define LAPIC_REG_EOI           0x0B0u   /* End of Interrupt             */
#define LAPIC_REG_LVT_TIMER     0x320u   /* LVT Timer                    */
#define LAPIC_REG_TIMER_ICR     0x380u   /* Timer Initial Count          */
#define LAPIC_REG_TIMER_CCR     0x390u   /* Timer Current Count          */
#define LAPIC_REG_TIMER_DCR     0x3E0u   /* Timer Divide Configuration   */

/* SVR 位 */
#define LAPIC_SVR_ENABLE        (1u << 8)   /* APIC Software Enable     */

/* LVT Timer 位 */
#define LAPIC_TIMER_PERIODIC    (1u << 17)  /* Periodic 模式             */
#define LAPIC_TIMER_MASKED      (1u << 16)  /* 屏蔽中断                  */

/* Divide Configuration 值（用于 TIMER_DCR）*/
#define LAPIC_TIMER_DIV_1       0x0Bu   /* 不分频                       */
#define LAPIC_TIMER_DIV_2       0x00u
#define LAPIC_TIMER_DIV_4       0x01u
#define LAPIC_TIMER_DIV_8       0x02u
#define LAPIC_TIMER_DIV_16      0x03u

/* PIT (8254) 辅助定义（用于频率校准）*/
#define PIT_CAL_PORT_CMD        0x43u
#define PIT_CAL_PORT_CH2        0x42u
#define PIT_CAL_PORT_CTRL       0x61u   /* PC speaker / counter-2 gate  */
#define PIT_BASE_FREQ           1193182UL

/* ── 函数声明 ──────────────────────────────────────────────────*/

/**
 * lapic_init — 使能 LAPIC，设置 SVR
 * 需要在 exception_init 之后调用（SVR 中断向量 0xFF 需已注册）
 */
void lapic_init(void);

/**
 * lapic_timer_init — 使用 PIT 校准 LAPIC timer 频率，
 *                   设置 Periodic 模式，每 (1000/TIMER_FREQUENCY_HZ) ms 一次中断
 * @param vector  IDT 向量号（通常 IDT_LAPIC_TIMER_VEC）
 */
void lapic_timer_init(uint8_t vector);

/**
 * g_tsc_freq_hz — TSC 频率（Hz），由 lapic_timer_init 在 PIT 校准时测量
 */
extern volatile uint64_t g_tsc_freq_hz;

/**
 * lapic_eoi — 发送 EOI 信号（每次中断处理完必须调用）
 */
void lapic_eoi(void);

/**
 * lapic_timer_stop — 屏蔽 LAPIC timer 中断
 */
void lapic_timer_stop(void);

#endif /* LAPIC_H */
