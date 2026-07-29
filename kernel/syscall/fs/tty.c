/*
 * fs/tty.c - 终端状态 + UART 输入环形缓冲区 + signal_check_uart
 *
 * 由 kernel/syscall/syscall.c 拆出。
 */
#include "syscall/fs/tty.h"
#include "syscall/syscall_internal.h"
#include "task/task.h"
#include "spinlock.h"
#include "uart/uart.h"

#define UART_RINGBUF_SIZE  64u
static volatile uint8_t  g_uart_rb[UART_RINGBUF_SIZE];
static volatile uint32_t g_uart_rb_head = 0;
static volatile uint32_t g_uart_rb_tail = 0;
static spinlock_noirq_t g_uart_rb_lock = SPINLOCK_NOIRQ_INIT;

void uart_ringbuf_push(char c) {
    spin_lock_irqsave(&g_uart_rb_lock);
    uint32_t next = (g_uart_rb_head + 1u) % UART_RINGBUF_SIZE;
    if (next != g_uart_rb_tail) {
        g_uart_rb[g_uart_rb_head] = (uint8_t)c;
        g_uart_rb_head = next;
    }
    spin_unlock_irqrestore(&g_uart_rb_lock);
}

int uart_ringbuf_pop(char *out) {
    spin_lock_irqsave(&g_uart_rb_lock);
    if (g_uart_rb_tail == g_uart_rb_head) {
        spin_unlock_irqrestore(&g_uart_rb_lock);
        return 0;
    }
    *out = (char)g_uart_rb[g_uart_rb_tail];
    g_uart_rb_tail = (g_uart_rb_tail + 1u) % UART_RINGBUF_SIZE;
    spin_unlock_irqrestore(&g_uart_rb_lock);
    return 1;
}

int uart_ringbuf_empty(void) {
    spin_lock_irqsave(&g_uart_rb_lock);
    int empty = (g_uart_rb_tail == g_uart_rb_head);
    spin_unlock_irqrestore(&g_uart_rb_lock);
    return empty;
}

/* 全局 termios 状态 */
struct kernel_termios g_termios = {
    .c_iflag = 0x00000500U, /* ICRNL | IXON */
    .c_oflag = 0x00000005U, /* OPOST | ONLCR */
    .c_cflag = 0x000000BFU, /* B38400 | CS8 | CREAD */
    .c_lflag = 0x00008A3BU, /* ISIG | ICANON | ECHO* | IEXTEN */
    .c_line  = 0,
    .c_cc    = {0,0,0,0, 4/*VEOF=^D*/, 0/*VTIME*/, 1/*VMIN*/, 0,0,0,0,0,0,0,0,0,0,0,0},
};

void signal_check_uart(void) {
    while (uart_rx_ready()) {
        char c = uart_getc();
        if (c == '\x03' && (g_termios.c_lflag & 0x0001u)) {
            uint32_t fg = g_fg_pgid;
            task_t *cur = task_current();
            if (fg == 0 && cur)
                fg = cur->pgid;
            if (fg) task_send_signal_to_pgid(fg, SIGINT);
        } else {
            uart_ringbuf_push(c);
        }
    }
}

/* tty 辅助接口：供 pseudofs/tty_read 等调用 */
int termios_is_raw(void)   { return !(g_termios.c_lflag & 0x0002u); }
int termios_do_icrnl(void) { return  (g_termios.c_iflag & 0x0100u); }
int tty_getchar_nb(char *c) { signal_check_uart(); return uart_ringbuf_pop(c); }
