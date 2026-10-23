/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/driver/time.h — 时间基准公共接口

   内核只依赖本头：tick/uptime/延时/硬件计数器读取都从这里取。
   具体实现由 driver/timer/timer.c 按 ARCH 选择，驱动 probe 负责初始化。
*/

#ifndef DRIVER_TIME_H
#define DRIVER_TIME_H

#include "types.h"
#include "platform_cfg.h"
#include "arch.h"

/* ── 平台配置（由 g_platform_devices[] 的 time_cfg_t 提供）────────────── */

typedef struct {
    uint32_t freq_hz;    /* 目标 tick 频率 */
    uint32_t tick_ms;    /* tick 周期 */
    uint32_t cntp;       /* AArch64 CNTP PPI 号（其它架构为 0） */
    uint64_t counter_hz; /* 硬件计数器频率 */
} time_cfg_t;

/* ── 统计信息 ────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t total_interrupts;    /* 总中断次数 */
    uint64_t total_schedules;     /* 总调度次数(AArch64) */
    uint64_t total_seconds;       /* 总运行秒数(RISC-V) */
    uint64_t last_interrupt_time; /* 上次中断时间(毫秒) */
} timer_stats_t;

/* ── 全局状态 ────────────────────────────────────────────────────────── */

extern volatile uint64_t g_system_ticks;    /* 系统 tick 计数 */
extern volatile uint64_t g_timer_frequency; /* 定时器频率 */
extern timer_stats_t g_timer_stats;         /* 统计信息 */

/* 定时器配置(由 timer_init() 从平台配置填充) */
extern unsigned g_timer_cfg_freq_hz;     /* 中断目标频率 (Hz) */
extern unsigned g_timer_cfg_tick_ms;     /* 中断目标周期 (ms) */
extern uintptr_t g_timer_cfg_counter_hz; /* 硬件计数器频率 (Hz) */
extern unsigned g_timer_cfg_cntp;        /* CNTP IRQ 号 */

/* 定时器兼容宏(供架构实现头文件使用) */
#define TIMER_FREQUENCY_HZ g_timer_cfg_freq_hz
#define TIMER_TICK_MS      g_timer_cfg_tick_ms
#define TIMER_FREQ_HZ      g_timer_cfg_counter_hz
#define CNTP_TIMER         g_timer_cfg_cntp

/* ── 架构特定操作（driver/timer 内部使用）────────────────────────────── */

void timer_arch_init(void);
void timer_arch_enable(void);
void timer_arch_disable(void);
void timer_arch_set_next_interrupt(uint64_t ticks_from_now);

/* ── 通用接口 ────────────────────────────────────────────────────────── */

void timer_init(void);           /* 初始化定时器模块 */
void timer_enable(void);         /* 启用定时器中断 */
void timer_init_secondary(void); /* 次级核最小初始化(SMP) */
void timer_disable(void);        /* 禁用定时器中断 */
void timer_set_next_interrupt(uint64_t ticks_from_now);
void timer_schedule_next_tick(void);

/*
   定时器中断处理函数(架构特定)
   AArch64: timer_handler(uint64_t *stack_pointer)
   RISC-V:  timer_handler(void *frame)
   x86_64:  timer_handler(void *frame)
*/
#if ARCH_AARCH64
void timer_handler(uint64_t *stack_pointer);
#elif ARCH_RISCV64
void timer_handler(void *frame);
#elif ARCH_X86_64
void timer_handler(void *frame);
#endif

/* ── 时间相关函数 ────────────────────────────────────────────────────── */

uint64_t timer_get_system_ticks(void);
uint64_t timer_get_uptime_ms(void);
uint64_t timer_get_frequency(void);

void timer_delay_ms(uint32_t ms); /* 毫秒级忙等待 */
void timer_delay_us(uint32_t us); /* 微秒级忙等待 */

/*
   timer_read_counter - 读取当前平台单调硬件计数器

   RISC-V: time CSR；AArch64: CNTPCT_EL0；x86_64: TSC。
   该接口只做裸计数读取, 不换算单位。
*/
uint64_t timer_read_counter(void);

/* timer_counter_frequency - 当前计时源频率(Hz) */
uint64_t timer_counter_frequency(void);

/* timer_counter_to_ns - 将计时源 tick 换算为纳秒 */
uint64_t timer_counter_to_ns(uint64_t ticks);

/*
   timer_spin - 短暂忙等待, 用于设备寄存器轮询退避

   这是裸 nop 循环的统一入口。需要真实时间语义时使用
   timer_delay_us/timer_delay_ms 或 timer_poll_until*。
*/
void timer_spin(uint32_t iterations);

typedef bool (*timer_poll_predicate_t)(void *ctx);

/*
   timer_poll_until - 按最大轮询次数等待条件成立
   返回 0 表示条件达成, -1 表示超时。
*/
int timer_poll_until(timer_poll_predicate_t pred, void *ctx, uint32_t max_polls, uint32_t relax_iters);

/*
   timer_poll_until_us - 按真实时间上限等待条件成立
   @timeout_us: 超时时间, 单位微秒
   返回 0 表示条件达成, -1 表示超时。
*/
int timer_poll_until_us(timer_poll_predicate_t pred, void *ctx, uint64_t timeout_us, uint32_t relax_iters);

/* ── 统计 ────────────────────────────────────────────────────────────── */

void timer_get_stats(timer_stats_t *stats);
void timer_reset_stats(void);
void timer_dump_info(void);

/* ── Tick 回调（供调度器注册）────────────────────────────────────────── */

typedef void (*timer_tick_cb_t)(void);

/*
   timer_set_tick_cb - 注册 tick 回调函数
   @cb: 回调函数指针(NULL 表示取消注册)

   回调在每次 timer ISR 末尾调用；调用时中断已被 CPU 屏蔽，回调应尽快返回。
   典型用途：sched_tick()
*/
void timer_set_tick_cb(timer_tick_cb_t cb);

/*
   timer_get_ns - 获取当前单调时间(纳秒)

   使用硬件计数器(RISC-V rdtime / AArch64 cntpct_el0 / x86_64 TSC)。
   可在中断、syscall 内核路径等任意上下文中安全调用。
*/
static inline uint64_t timer_get_ns(void)
{
    return timer_counter_to_ns(timer_read_counter());
}

#endif /* DRIVER_TIME_H */
