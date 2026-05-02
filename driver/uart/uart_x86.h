/*
 * driver/uart/uart_x86.h — x86 COM1 (16550 Port I/O) UART 驱动
 *
 * x86 COM1 与 DesignWare 16550 是同一硬件系列，但访问方式不同：
 *   - COM1 使用 Port I/O (in/out 指令)，基地址 0x3F8
 *   - DW   使用 MMIO，寄存器按 4 字节对齐
 *
 * 本文件提供纯轮询（polling）实现，足够早期启动使用。
 */

#ifndef DRIVER_UART_X86_H
#define DRIVER_UART_X86_H

#include "types.h"
#include "driver_cfg.h"
#include "uart_16550.h"

/* COM1 I/O 基地址 */
#define UART_X86_BASE   0x3F8U

/* 绝对端口号 */
#define UART_X86_RBR    (UART_X86_BASE + UART16550_PIO_RBR)
#define UART_X86_THR    (UART_X86_BASE + UART16550_PIO_THR)
#define UART_X86_IER    (UART_X86_BASE + UART16550_PIO_IER)
#define UART_X86_IIR    (UART_X86_BASE + UART16550_PIO_IIR)
#define UART_X86_FCR    (UART_X86_BASE + UART16550_PIO_FCR)
#define UART_X86_LCR    (UART_X86_BASE + UART16550_PIO_LCR)
#define UART_X86_LSR    (UART_X86_BASE + UART16550_PIO_LSR)
#define UART_X86_DLL    (UART_X86_BASE + UART16550_PIO_DLL)
#define UART_X86_DLM    (UART_X86_BASE + UART16550_PIO_DLM)

/* 函数声明 */
void uart_x86_early_init(void);
void uart_x86_putchar(char c);
void uart_x86_putstr(const char *s);
char uart_x86_getchar(void);
bool uart_x86_getchar_nb(char *c);

#endif /* DRIVER_UART_X86_H */
