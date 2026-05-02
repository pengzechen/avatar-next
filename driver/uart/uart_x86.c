/*
 * driver/uart/uart_x86.c — x86 COM1 (16550 Port I/O) UART 驱动实现
 *
 * 使用纯轮询模式，不依赖中断。QEMU 会预先初始化 COM1，
 * uart_x86_early_init() 重新配置一次以确保设置正确。
 */

#include "uart_x86.h"
#include "x86_64/io.h"
#include "types.h"

/* 等待 TX 寄存器空 */
static inline int
uart_x86_tx_ready(void)
{
    return (inb(UART_X86_LSR) & UART16550_LSR_THRE) != 0;
}

/*
 * uart_x86_early_init — 配置 COM1 为 115200-8N1，禁用中断
 * QEMU 通常已初始化，但显式配置以保证行为确定。
 */
void
uart_x86_early_init(void)
{
    outb(UART_X86_IER, 0x00);                   /* 禁用所有中断           */
    outb(UART_X86_LCR, UART16550_LCR_DLAB);     /* 置位 DLAB，允许写除数  */
    outb(UART_X86_DLL, 0x01);                   /* 除数低字节 = 1 → 115200 */
    outb(UART_X86_DLM, 0x00);                   /* 除数高字节 = 0          */
    outb(UART_X86_LCR, UART16550_LCR_WLS_8);    /* 8N1，清除 DLAB         */
    outb(UART_X86_FCR,                           /* 使能并清空 FIFO        */
         UART16550_FCR_ENABLE_FIFO |
         UART16550_FCR_CLEAR_RX   |
         UART16550_FCR_CLEAR_TX);
}

/*
 * uart_x86_putchar — 发送单个字符（带 \n → \r\n 转换）
 */
void
uart_x86_putchar(char c)
{
    if (c == '\n') {
        while (!uart_x86_tx_ready()) {}
        outb(UART_X86_THR, '\r');
    }
    while (!uart_x86_tx_ready()) {}
    outb(UART_X86_THR, (uint8_t)c);
}

/*
 * uart_x86_putstr — 发送字符串
 */
void
uart_x86_putstr(const char *s)
{
    while (*s)
        uart_x86_putchar(*s++);
}

/*
 * uart_x86_getchar — 阻塞读取单个字符
 */
char
uart_x86_getchar(void)
{
    while (!(inb(UART_X86_LSR) & UART16550_LSR_DR)) {}
    return (char)inb(UART_X86_RBR);
}

/*
 * uart_x86_getchar_nb — 非阻塞读取，无数据时返回 false
 */
bool
uart_x86_getchar_nb(char *c)
{
    if (!(inb(UART_X86_LSR) & UART16550_LSR_DR))
        return false;
    *c = (char)inb(UART_X86_RBR);
    return true;
}
