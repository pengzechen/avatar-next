/*
 * driver/driver_cfg.h  —  驱动配置总入口
 *
 * 所有 driver/ 内的头文件必须 #include "driver_cfg.h"，
 * 不再直接依赖各自原始项目的 cfg.h / t_cfg.h。
 *
 * 驱动选择与架构解耦：
 *   UART  通过 Makefile UART=pl011|dw 指定，未指定时按架构默认
 *   IRQ   通过 Makefile GIC=v2|v3    指定（仅 aarch64），未指定时默认 v2
 *   Timer 固定按架构选择（依赖架构专属系统寄存器，无法跨架构复用）
 *
 * 架构默认值：
 *   aarch64  : UART=pl011, IRQ=GICv2, Timer=aarch64
 *   riscv64  : UART=dw,             Timer=rv
 *   x86_64   : UART 由 platform.c 直接通过 io.h 处理，无独立驱动
 *
 * 覆盖示例（Makefile 传入）：
 *   make ARCH=aarch64 UART=dw kernel    # aarch64 使用 DW UART
 *   make ARCH=aarch64 GIC=v3  kernel    # aarch64 使用 GICv3
 */

#ifndef DRIVER_CFG_H
#define DRIVER_CFG_H

#include "arch.h"

/* ============================================================
 * UART 驱动选择
 *   优先使用编译时外部传入的 DRIVER_UART_PL011 / DRIVER_UART_DW，
 *   否则按架构默认。
 * ============================================================ */

#if !defined(DRIVER_UART_PL011) && !defined(DRIVER_UART_DW) && !defined(DRIVER_UART_X86)
    /* 无外部覆盖：按架构选择默认驱动 */
    #if ARCH_AARCH64
        #define DRIVER_UART_PL011   1
    #elif ARCH_RISCV64
        #define DRIVER_UART_DW      1
    #elif ARCH_X86_64
        #define DRIVER_UART_X86     1
    #endif
#endif

/* 合法性检查：PL011 目前仅支持 aarch64 系统寄存器布局 */
#if defined(DRIVER_UART_PL011) && ARCH_RISCV64
    #error "PL011 UART driver is not supported on RISC-V"
#endif

/* ============================================================
 * IRQ 控制器选择（aarch64 专用）
 *   优先使用编译时传入的 DRIVER_GIC_V2 / DRIVER_GIC_V3，
 *   否则默认 GICv2。
 * ============================================================ */

#if ARCH_AARCH64
    #if !defined(DRIVER_GIC_V2) && !defined(DRIVER_GIC_V3)
        #define DRIVER_GIC_V2   1
    #endif
#endif

/* ============================================================
 * Timer 驱动选择（固定按架构，不可覆盖）
 * ============================================================ */

#if ARCH_AARCH64
    #define DRIVER_TIMER_AARCH64    1
#elif ARCH_RISCV64
    #define DRIVER_TIMER_RV         1
#elif ARCH_X86_64
    #define DRIVER_TIMER_X86        1
#endif

/* ============================================================
 * 硬件基地址（QEMU virt 机器）
 * ============================================================ */

#if ARCH_AARCH64

    #define GICD_BASE_ADDR      0x08000000UL    /* GIC Distributor      */
    #define GICC_BASE_ADDR      0x08010000UL    /* GICv2 CPU Interface  */
    #define GICH_BASE_ADDR      0x08030000UL    /* GIC Hypervisor I/F   */
    #define GICR_BASE_ADDR      0x080A0000UL    /* GICv3 Redistributor  */

    /* UART 基地址依选定驱动而异 */
    #if defined(DRIVER_UART_PL011)
        #define UART_BASE       0x09000000UL    /* PL011                */
    #elif defined(DRIVER_UART_DW)
        #define UART_BASE       0x09000000UL    /* DW 16550（同地址）   */
    #endif

#elif ARCH_RISCV64

    #define UART_BASE           0x10000000UL    /* 16550 / DW UART      */
    #define PLIC_BASE_ADDR      0x0C000000UL    /* PLIC                 */
    #define CLINT_BASE_ADDR     0x02000000UL    /* CLINT                */

#endif  /* ARCH_* */

/* ============================================================
 * UART 16550 寄存器宽度（供 dw_uart.h 使用）
 * ============================================================ */

#if defined(DRIVER_UART_DW)
    #ifndef UART_REG_SHIFT
        #define UART_REG_SHIFT  0   /* QEMU virt：寄存器按字节紧密排列 */
    #endif
#endif

/* ============================================================
 * 定时器配置
 * ============================================================ */

#if ARCH_AARCH64
    #define TIMER_TICK_MS           10
    #define TIMER_FREQUENCY_HZ      (1000 / TIMER_TICK_MS)     /* 100 Hz */
    #define CNTP_TIMER              30                          /* PPI #30 */
#elif ARCH_RISCV64
    #define TIMER_FREQ_HZ           10000000UL                  /* 10 MHz */
    #define TIMER_TICK_MS           10
    #define TIMER_FREQUENCY_HZ      (1000 / TIMER_TICK_MS)     /* 100 Hz */
#elif ARCH_X86_64
    #define TIMER_TICK_MS           10
    #define TIMER_FREQUENCY_HZ      (1000 / TIMER_TICK_MS)     /* 100 Hz */
#endif

/* ============================================================
 * logger_* 兼容宏 — 映射到 klog 系统
 *   驱动内部代码使用 logger_info/warn/debug/error，
 *   统一通过本宏转发到 KLOG_* 宏。
 * ============================================================ */
#include "klog.h"
#define logger_error(...)  KLOG_ERROR(__VA_ARGS__)
#define logger_warn(...)   KLOG_WARN(__VA_ARGS__)
#define logger_info(...)   KLOG_INFO(__VA_ARGS__)
#define logger_debug(...)  KLOG_DEBUG(__VA_ARGS__)
#define logger_gic_debug(...)  KLOG_DEBUG(__VA_ARGS__)
#define logger_trace(...)  KLOG_TRACE(__VA_ARGS__)

#endif  /* DRIVER_CFG_H */

