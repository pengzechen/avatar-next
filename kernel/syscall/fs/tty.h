/*
 * fs/tty.h - 终端状态 + UART 输入环形缓冲区
 *
 * 包含全局 termios 状态、UART RX ring buffer 与若干 tty 辅助接口。
 * 大部分接口由 fs/file_io.c / fs/ioctl.c / fs/pseudofs/pseudofs.c
 * 与定时器 ISR 共享。
 */
#ifndef KERNEL_SYSCALL_FS_TTY_H
#define KERNEL_SYSCALL_FS_TTY_H

#include "types.h"
#include "syscall/syscall_internal.h"   /* struct kernel_termios */

/* 全局终端状态（TCGETS/TCSETS 读写）*/
extern struct kernel_termios g_termios;

/* UART 环形缓冲区接口 */
void uart_ringbuf_push(char c);
int  uart_ringbuf_pop (char *out);
/* int uart_ringbuf_empty(void); — 已在 syscall_internal.h 声明 */

/* 排空 UART FIFO，普通字符入 ring buffer，Ctrl+C 发 SIGINT */
/* void signal_check_uart(void); — 已在 syscall_internal.h 声明 */

#endif /* KERNEL_SYSCALL_FS_TTY_H */
