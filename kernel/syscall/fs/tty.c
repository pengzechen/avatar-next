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
    uint64_t flags;
    spin_lock_irqsave(&g_uart_rb_lock, &flags);
    uint32_t next = (g_uart_rb_head + 1u) % UART_RINGBUF_SIZE;
    if (next != g_uart_rb_tail) {
        g_uart_rb[g_uart_rb_head] = (uint8_t)c;
        g_uart_rb_head = next;
    }
    spin_unlock_irqrestore(&g_uart_rb_lock, flags);
}

int uart_ringbuf_pop(char *out) {
    uint64_t flags;
    spin_lock_irqsave(&g_uart_rb_lock, &flags);
    if (g_uart_rb_tail == g_uart_rb_head) {
        spin_unlock_irqrestore(&g_uart_rb_lock, flags);
        return 0;
    }
    *out = (char)g_uart_rb[g_uart_rb_tail];
    g_uart_rb_tail = (g_uart_rb_tail + 1u) % UART_RINGBUF_SIZE;
    spin_unlock_irqrestore(&g_uart_rb_lock, flags);
    return 1;
}

int uart_ringbuf_empty(void) {
    uint64_t flags;
    spin_lock_irqsave(&g_uart_rb_lock, &flags);
    int empty = (g_uart_rb_tail == g_uart_rb_head);
    spin_unlock_irqrestore(&g_uart_rb_lock, flags);
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

/*
 * 保护 UART 硬件（LSR/RBR）读取的自旋锁。
 *
 * signal_check_uart() 会被每个 CPU 的时钟中断处理程序调用，也会在
 * read/poll/select/epoll 等系统调用路径中被任意 CPU 调用。若不加锁，
 * SMP 下多个核会并发轮询同一个 COM1：核 A 读走 RBR 后，核 B 的
 * uart_getc() 会读到已被消费/空/半更新的寄存器，从而注入乱码字符
 * （单核时只有一个消费者，故不会复现）。此锁把整段硬件抽取串行化。
 */
static spinlock_noirq_t g_uart_hw_lock = SPINLOCK_NOIRQ_INIT;

void signal_check_uart(void) {
    char buf[UART_RINGBUF_SIZE];
    int  n = 0;

    /* 在锁内一次性把硬件 FIFO 抽干到本地缓冲，缩短临界区并避免与
     * 环形缓冲锁/信号投递产生锁嵌套。剩余字节留待下次调用处理。 */
    uint64_t flags;
    spin_lock_irqsave(&g_uart_hw_lock, &flags);
    while (n < (int)sizeof(buf) && uart_rx_ready())
        buf[n++] = uart_getc();
    spin_unlock_irqrestore(&g_uart_hw_lock, flags);

    for (int i = 0; i < n; i++) {
        char c = buf[i];
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
