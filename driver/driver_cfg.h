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
#include "mm_vm.h"   /* KERNEL_VMA: 外设基地址需加偏移，通过 TTBR1 访问 */
#include "device_profile.h"

/* ============================================================
 * UART 驱动选择
 *   优先使用编译时外部传入的 DRIVER_UART_PL011 / DRIVER_UART_DW，
 *   否则按架构默认。
 * ============================================================ */

#if !defined(DRIVER_UART_PL011) && !defined(DRIVER_UART_DW) && !defined(DRIVER_UART_X86)
    /* 无外部覆盖：按设备画像选择默认驱动 */
    #if DEVICE_DEFAULT_UART_PL011
        #define DRIVER_UART_PL011   1
    #elif DEVICE_DEFAULT_UART_DW
        #define DRIVER_UART_DW      1
    #elif DEVICE_DEFAULT_UART_X86
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
        #if DEVICE_DEFAULT_GIC_V2
            #define DRIVER_GIC_V2   1
        #elif DEVICE_DEFAULT_GIC_V3
            #define DRIVER_GIC_V3   1
        #endif
    #endif
#endif

/* ============================================================
 * Timer 驱动选择（固定按架构，不可覆盖）
 * ============================================================ */

#if DEVICE_DEFAULT_TIMER_AARCH64
    #define DRIVER_TIMER_AARCH64    1
#elif DEVICE_DEFAULT_TIMER_RV
    #define DRIVER_TIMER_RV         1
#elif DEVICE_DEFAULT_TIMER_X86
    #define DRIVER_TIMER_X86        1
#endif

/* ============================================================
 * 硬件基地址（QEMU virt 机器）
 * ============================================================ */

#if DEVICE_MMIO_NEEDS_VMA
    #define DEVICE_MMIO_ADDR(raw)   ((raw) + KERNEL_VMA)
#else
    #define DEVICE_MMIO_ADDR(raw)   (raw)
#endif

#define GICD_BASE_ADDR      DEVICE_MMIO_ADDR(DEVICE_GICD_BASE_RAW)
#define GICC_BASE_ADDR      DEVICE_MMIO_ADDR(DEVICE_GICC_BASE_RAW)
#define GICH_BASE_ADDR      DEVICE_MMIO_ADDR(DEVICE_GICH_BASE_RAW)
#define GICR_BASE_ADDR      DEVICE_MMIO_ADDR(DEVICE_GICR_BASE_RAW)

#define UART_BASE           DEVICE_MMIO_ADDR(DEVICE_UART_BASE_RAW)
#define PLIC_BASE_ADDR      DEVICE_MMIO_ADDR(DEVICE_PLIC_BASE_RAW)
#define CLINT_BASE_ADDR     DEVICE_MMIO_ADDR(DEVICE_CLINT_BASE_RAW)

/* ============================================================
 * UART 16550 寄存器宽度（供 dw_uart.h 使用）
 * ============================================================ */

#if defined(DRIVER_UART_DW)
    #ifndef UART_REG_SHIFT
        #define UART_REG_SHIFT  DEVICE_UART_REG_SHIFT
    #endif
#endif

/* ============================================================
 * 定时器配置
 * ============================================================ */

#define TIMER_TICK_MS           DEVICE_TIMER_TICK_MS
#define TIMER_FREQUENCY_HZ      DEVICE_TIMER_FREQUENCY_HZ

#if DEVICE_TIMER_COUNTER_HZ != 0 && !ARCH_RISCV64 && !defined(TIMER_FREQ_HZ)
    #define TIMER_FREQ_HZ       DEVICE_TIMER_COUNTER_HZ
#endif

#if DEVICE_CNTP_TIMER != 0
    #define CNTP_TIMER          DEVICE_CNTP_TIMER
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

