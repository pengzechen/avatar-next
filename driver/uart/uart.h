/*
 * driver/uart/uart.h  —  统一 UART 接口
 *
 * 根据 driver_cfg.h 中的驱动选择，通过宏别名统一 API：
 *   uart_init()      初始化 UART
 *   uart_putc(c)     发送一个字符
 *   uart_puts(s)     发送字符串
 *   uart_getc()      阻塞读取一个字符
 *
 * 三架构映射：
 *   DRIVER_UART_PL011  (aarch64)  → pl011_*()
 *   DRIVER_UART_DW     (riscv64)  → dw_uart_*()
 *   DRIVER_UART_X86    (x86_64)   → uart_x86_*()
 */

#ifndef DRIVER_UART_H
#define DRIVER_UART_H

#include "driver_cfg.h"

/* ============================================================
 * PL011（aarch64）
 * ============================================================ */
#if DRIVER_UART_PL011

    #include "uart_pl011.h"

    #define uart_init()     pl011_init()
    #define uart_putc(c)    pl011_putchar(c)
    #define uart_puts(s)    pl011_putstr(s)
    #define uart_getc()     pl011_getchar(c)

/* ============================================================
 * DesignWare 16550（riscv64）
 * ============================================================ */
#elif DRIVER_UART_DW

    #include "uart_dw.h"

    #define uart_init()     dw_uart_early_init()
    #define uart_putc(c)    dw_uart_putchar(c)
    #define uart_puts(s)    dw_uart_putstr(s)
    #define uart_getc()     dw_uart_getchar()

/* ============================================================
 * x86 COM1 16550 Port I/O（x86_64）
 * ============================================================ */
#elif DRIVER_UART_X86

    #include "uart_x86.h"

    #define uart_init()     uart_x86_early_init()
    #define uart_putc(c)    uart_x86_putchar(c)
    #define uart_puts(s)    uart_x86_putstr(s)
    #define uart_getc()     uart_x86_getchar()

#endif  /* DRIVER_UART_* */

#endif  /* DRIVER_UART_H */
