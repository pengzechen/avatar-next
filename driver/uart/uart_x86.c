/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   driver/uart/uart_x86.c — x86 COM1 (16550 Port I/O) UART 驱动实现

   TX 始终同步轮询：klog 在关中断持锁的状态下逐字符输出，panic 路径还会
   绕过锁，不能依赖 TX 中断。

   RX 可选中断模式：EARLY probe 只配置硬件并关闭所有中断；DRV_PHASE_IRQ
   的 start 阶段（LAPIC/IOAPIC 已就绪）再注册 IRQ、打开 RDI，并把 COM1
   IRQ4 通过 IOAPIC GSI4 投递到 IDT 向量 0x24。

   宿主中断关闭的调用者（VMM 直启模式的 console pump、tick）仍通过
   serial_rx_ready()/serial_getc() 直接 drain 硬件 FIFO。
*/

#include "uart_x86.h"
#include "x86_64/io.h"
#include "types.h"
#include "klog.h"
#include "driver/driver.h"
#include "driver/serial.h"
#include "driver/irq.h"
#include "spinlock.h"
#include "barrier.h"

#define UART_X86_RX_BUFFER_SIZE 1024

typedef struct {
    char buffer[UART_X86_RX_BUFFER_SIZE];
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t count;
    spinlock_noirq_t lock; /* ISR 与线程都会取：必须用 IRQ-safe 变体 */
} uart_x86_buffer_t;

static uart_x86_buffer_t rx_buffer;
static volatile bool uart_initialized = false;
static volatile bool rx_irq_mode = false;

/* RX 诊断计数；仅 GDB 使用，不导出 API */
static volatile uint64_t rx_irq_count = 0;
static volatile uint64_t rx_irq_bytes = 0;
static volatile uint64_t rx_dropped = 0;

/* ── RX 软件环（调用者持锁）────────────────────────────────── */

static bool buffer_is_empty(uart_x86_buffer_t *buf)
{
    return buf->count == 0;
}

static bool buffer_is_full(uart_x86_buffer_t *buf)
{
    return buf->count >= UART_X86_RX_BUFFER_SIZE;
}

static bool buffer_put(uart_x86_buffer_t *buf, char c)
{
    if (buffer_is_full(buf))
        return false;

    buf->buffer[buf->head] = c;
    buf->head = (buf->head + 1) % UART_X86_RX_BUFFER_SIZE;
    buf->count++;
    return true;
}

static bool buffer_get(uart_x86_buffer_t *buf, char *c)
{
    if (buffer_is_empty(buf))
        return false;

    *c = buf->buffer[buf->tail];
    buf->tail = (buf->tail + 1) % UART_X86_RX_BUFFER_SIZE;
    buf->count--;
    return true;
}

/* 等待 TX 寄存器空 */
static inline int uart_x86_tx_ready(void)
{
    return (inb(UART_X86_LSR) & UART16550_LSR_THRE) != 0;
}

/*
   把硬件 RX FIFO 读空并放进软件环。调用者必须持有 rx_buffer.lock。
   读 LSR 同时清掉 OE/PE/FE/BI 错误位；软件环满时继续读硬件，避免
   FIFO 一直保持非空。
*/
static uint32_t uart_x86_rx_drain_locked(void)
{
    uint32_t moved = 0;

    while (inb(UART_X86_LSR) & UART16550_LSR_DR) {
        char c = (char)inb(UART_X86_RBR);
        if (buffer_put(&rx_buffer, c))
            moved++;
        else
            rx_dropped++;
    }

    return moved;
}

/*
   RX 中断处理：只搬字节，不打日志、不碰 tty/任务、不碰 TX。

   IIR 低 4 位：0x04 = RDI，0x0C = FIFO 接收超时；二者都表示 RBR 有数据。
   THRI 不会出现（IER 只开 RDI）。
*/
static void uart_x86_irq_handler(void *frame, void *ctx)
{
    (void)frame;
    (void)ctx;

    uint64_t flags;
    spin_lock_irqsave(&rx_buffer.lock, &flags);

    for (int i = 0; i < 16; i++) {
        uint8_t iir = inb(UART_X86_IIR);

        if (iir & UART16550_IIR_NO_INT)
            break;

        uint8_t type = iir & UART16550_IIR_MASK;
        if (type == UART16550_IIR_RDI || type == UART16550_IIR_RDTO) {
            uint32_t moved = uart_x86_rx_drain_locked();
            rx_irq_count++;
            rx_irq_bytes += moved;
        } else {
            /* 其他中断源未启用；读 LSR 清掉可能的状态 */
            (void)inb(UART_X86_LSR);
        }
    }

    spin_unlock_irqrestore(&rx_buffer.lock, flags);
}

/*
   uart_x86_early_init — 配置 COM1 为 115200-8N1, 关闭所有 UART 中断
   QEMU 通常已初始化, 但显式配置以保证行为确定。
*/
void uart_x86_early_init(void)
{
    spinlock_irq_init(&rx_buffer.lock);
    rx_buffer.head = rx_buffer.tail = rx_buffer.count = 0;

    outb(UART_X86_IER, 0x00);                /* 禁用所有中断 */
    outb(UART_X86_LCR, UART16550_LCR_DLAB);  /* 置位 DLAB, 允许写除数 */
    outb(UART_X86_DLL, 0x01);                /* 除数低字节 = 1 → 115200 */
    outb(UART_X86_DLM, 0x00);                /* 除数高字节 = 0 */
    outb(UART_X86_LCR, UART16550_LCR_WLS_8); /* 8N1, 清除 DLAB */
    outb(UART_X86_FCR,                       /* 使能并清空 FIFO */
         UART16550_FCR_ENABLE_FIFO | UART16550_FCR_CLEAR_RX | UART16550_FCR_CLEAR_TX);

    /* PC 兼容 16550 需要 OUT2=1 才把中断送到 IOAPIC/PIC */
    outb(UART_X86_MCR, UART16550_MCR_DTR | UART16550_MCR_RTS | UART16550_MCR_OUT2);

    /* 清掉可能的残留状态 */
    (void)inb(UART_X86_IIR);
    (void)inb(UART_X86_LSR);

    uart_initialized = true;
}

/*
   uart_x86_putchar — 发送单个字符(带 \n → \r\n 转换)
*/
void uart_x86_putchar(char c)
{
    if (c == '\n') {
        while (!uart_x86_tx_ready()) {}
        outb(UART_X86_THR, '\r');
    }
    while (!uart_x86_tx_ready()) {}
    outb(UART_X86_THR, (uint8_t)c);
}

/*
   uart_x86_putstr — 发送字符串
*/
void uart_x86_putstr(const char *s)
{
    while (*s)
        uart_x86_putchar(*s++);
}

/*
   uart_x86_getchar — 阻塞读取单个字符
*/
char uart_x86_getchar(void)
{
    char c;

    if (!uart_initialized || !rx_irq_mode) {
        while (!(inb(UART_X86_LSR) & UART16550_LSR_DR)) {
            __asm__ volatile("pause");
        }
        return (char)inb(UART_X86_RBR);
    }

    while (!uart_x86_getchar_nb(&c)) {
        if (uart_x86_rx_available())
            continue;
        __asm__ volatile("pause");
    }
    return c;
}

/*
   uart_x86_getchar_nb — 非阻塞读取, 无数据时返回 false
*/
bool uart_x86_getchar_nb(char *c)
{
    if (!uart_initialized)
        return false;

    bool success = false;
    uint64_t flags;
    spin_lock_irqsave(&rx_buffer.lock, &flags);

    if (rx_irq_mode) {
        success = buffer_get(&rx_buffer, c);
    } else if (inb(UART_X86_LSR) & UART16550_LSR_DR) {
        *c = (char)inb(UART_X86_RBR);
        success = true;
    }

    spin_unlock_irqrestore(&rx_buffer.lock, flags);
    return success;
}

/*
   uart_x86_rx_available — 检查是否有可读数据
*/
bool uart_x86_rx_available(void)
{
    if (!uart_initialized)
        return (inb(UART_X86_LSR) & UART16550_LSR_DR) != 0;

    bool available;
    uint64_t flags;
    spin_lock_irqsave(&rx_buffer.lock, &flags);

    if (rx_irq_mode) {
        /* VMM 直启/tick 等宿主中断关闭的场景靠这里 drain 硬件 */
        (void)uart_x86_rx_drain_locked();
        available = !buffer_is_empty(&rx_buffer);
    } else {
        available = (inb(UART_X86_LSR) & UART16550_LSR_DR) != 0;
    }

    spin_unlock_irqrestore(&rx_buffer.lock, flags);
    return available;
}

static const serial_ops_t uart_x86_serial_ops = {
    .putc = uart_x86_putchar,
    .puts = uart_x86_putstr,
    .getc = uart_x86_getchar,
    .getc_nb = uart_x86_getchar_nb,
    .rx_ready = uart_x86_rx_available,
};

static int uart_x86_probe(const device_t *dev)
{
    (void)dev; /* COM1 是固定端口，不需要平台 MMIO 配置 */
    serial_bind(&uart_x86_serial_ops);
    uart_x86_early_init();
    return 0;
}

/*
   DRV_PHASE_IRQ 的 start：LAPIC/IOAPIC 已在同阶段的 probe pass 里初始化，
   此处可以注册并打开 COM1 的中断线。

   失败时降级为轮询并返回 0：SERIAL 是必需 class，start 返回非 0 会触发
   driver_required_panic，但串口本身仍然可用，没必要因此不启动。
*/
static int uart_x86_start(const device_t *dev)
{
    const serial_cfg_t *cfg = dev ? (const serial_cfg_t *)dev->cfg : NULL;

    if (!uart_initialized || !cfg || !cfg->irq_mode || cfg->irq == 0)
        return 0;

    if (rx_irq_mode)
        return 0;

    if (!irq_available()) {
        KLOG_WARN("uart-x86: no IRQ controller; RX stays polled\n");
        return 0;
    }

    if (irq_register(cfg->irq, uart_x86_irq_handler, NULL) != 0) {
        KLOG_WARN("uart-x86: irq_register(0x%x) failed; RX stays polled\n", cfg->irq);
        return 0;
    }

    /* 启动前已经输入、还留在 FIFO 里的字节先进软件环 */
    uint64_t flags;
    spin_lock_irqsave(&rx_buffer.lock, &flags);
    (void)uart_x86_rx_drain_locked();
    spin_unlock_irqrestore(&rx_buffer.lock, flags);

    /* 只打开 RDI；TX 中断永远保持关闭 */
    outb(UART_X86_IER, UART16550_IER_RDI);

    if (irq_enable(cfg->irq, true) != 0) {
        outb(UART_X86_IER, 0);
        KLOG_WARN("uart-x86: irq_enable(0x%x) failed; RX stays polled\n", cfg->irq);
        return 0;
    }

    barrier_release();
    rx_irq_mode = true;

    KLOG_UART("uart-x86: RX IRQ mode, vector=0x%x\n", cfg->irq);
    return 0;
}

DRIVER_ENTRY(driver_uart_x86) = {
    .name = "uart-x86",
    .cls = DRV_SERIAL,
    .phase = DRV_PHASE_EARLY,
    .start_phase = DRV_PHASE_IRQ,
    .probe = uart_x86_probe,
    .start = uart_x86_start,
};
