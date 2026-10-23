/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   PL011 UART driver

   TX 始终是同步/轮询的：klog 在关中断、持 g_klog_lock 的状态下逐字符输出，
   panic 路径还会绕过锁，所以发送不能依赖 TX 中断或任何可能sleep/自旋等待
   的缓冲排空机制。

   RX 可选中断模式：EARLY probe 只配置硬件并把所有中断源屏蔽；到了
   DRV_PHASE_IRQ 的 start 阶段（GIC 已 probe）才注册 IRQ、打开 RX/RT 中断。
   中断只把字节搬进 rx_buffer，不碰 tty/任务/信号，也不打日志。

   宿主中断关闭的调用者（tick、VMM 直启模式的 console pump）仍然通过
   serial_rx_ready()/serial_getc() 直接 drain 硬件 FIFO，所以没有 IRQ 也
   不会丢输入。
*/

#include "uart_pl011.h"
#include "types.h"
#include "mmio.h"
#include "klog.h"
#include "driver/time.h"
#include "driver/irq.h"
#include "driver/driver.h"
#include "driver/serial.h"
#include "spinlock.h"
#include "barrier.h"

/*
   UART 基地址运行时变量
   初始值为物理地址, 供 MMU 开启前的早期 boot 使用。
   驱动 probe 会按设备表把它更新为 KERNEL_VMA + 0x09000000。
*/
volatile uintptr_t g_pl011_base = 0x09000000UL;

#define UART_RX_BUFFER_SIZE 1024

typedef struct {
    char buffer[UART_RX_BUFFER_SIZE];
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t count;
    spinlock_noirq_t lock; /* ISR 与线程都会取：必须用 IRQ-safe 变体 */
} uart_buffer_t;

static uart_buffer_t rx_buffer;
static volatile bool uart_initialized = false;
static volatile bool rx_irq_mode = false;

/* 阻塞发送超时被丢弃的字符数(只计数不打日志, 理由见 pl011_putchar) */
static volatile uint64_t tx_dropped = 0;

/* RX 诊断计数；仅 GDB 使用，不导出 API */
static volatile uint64_t rx_irq_count = 0;
static volatile uint64_t rx_irq_bytes = 0;
static volatile uint64_t rx_dropped = 0;

/* Buffer operations（只用于 RX 软件环；调用者负责加锁） */
static bool buffer_is_empty(uart_buffer_t *buf)
{
    return buf->count == 0;
}

static bool buffer_is_full(uart_buffer_t *buf)
{
    return buf->count >= UART_RX_BUFFER_SIZE;
}

static bool buffer_put(uart_buffer_t *buf, char c)
{
    if (buffer_is_full(buf))
        return false;

    buf->buffer[buf->head] = c;
    buf->head = (buf->head + 1) % UART_RX_BUFFER_SIZE;
    buf->count++;
    return true;
}

static bool buffer_get(uart_buffer_t *buf, char *c)
{
    if (buffer_is_empty(buf))
        return false;

    *c = buf->buffer[buf->tail];
    buf->tail = (buf->tail + 1) % UART_RX_BUFFER_SIZE;
    buf->count--;
    return true;
}

/* UART hardware operations */
static bool uart_tx_fifo_full(void)
{
    return (read32((void *)UART_FR) & UART_FR_TXFF) != 0;
}

static bool uart_rx_fifo_empty(void)
{
    return (read32((void *)UART_FR) & UART_FR_RXFE) != 0;
}

/*
   把硬件 RX FIFO 读空并放进软件环。调用者必须持有 rx_buffer.lock。

   即使软件环已满也必须继续读硬件：PL011 的 RX 是电平中断，FIFO 里留一个
   字节就会让中断线一直有效，ISR 会被反复触发。
*/
static uint32_t pl011_rx_drain_locked(void)
{
    uint32_t moved = 0;

    /* 读 RSR 清掉 FE/PE/BE/OE 状态；溢出字节已经丢了，没有恢复手段 */
    (void)read32((void *)UART_RSR);

    while (!uart_rx_fifo_empty()) {
        char c = (char)read32((void *)UART_DR);
        if (buffer_put(&rx_buffer, c))
            moved++;
        else
            rx_dropped++;
    }

    return moved;
}

/*
   RX 中断处理：只搬字节，不打日志、不碰 tty/任务、不碰 TX。

   TX 中断源始终不打开，所以这里不需要处理 UART_INT_TX。
*/
static void pl011_irq_handler(void *frame, void *ctx)
{
    (void)frame;
    (void)ctx;

    uint64_t flags;
    spin_lock_irqsave(&rx_buffer.lock, &flags);

    uint32_t mis = read32((void *)UART_MIS);
    if (mis & (UART_INT_RX | UART_INT_RT)) {
        uint32_t moved = pl011_rx_drain_locked();
        rx_irq_count++;
        rx_irq_bytes += moved;
        write32(UART_INT_RX | UART_INT_RT, (void *)UART_ICR);
    }

    spin_unlock_irqrestore(&rx_buffer.lock, flags);
}

/* Initialize UART hardware. All interrupt sources stay masked until start. */
void pl011_init(void)
{
    if (uart_initialized)
        return;

    /* 基址由 probe 从设备表设置（device_mmio 已按平台规则换算） */
    spinlock_irq_init(&rx_buffer.lock);
    rx_buffer.head = rx_buffer.tail = rx_buffer.count = 0;

    /* Disable UART first */
    write32(0, (void *)UART_CR);

    /* Clear all interrupts and mask every source */
    write32(0x7FF, (void *)UART_ICR);
    write32(0, (void *)UART_IMSC);

    /*
       Configure UART (115200 baud, 8N1)
       For 24MHz clock: IBRD = 24000000 / (16 * 115200) = 13
       FBRD = (0.0208 * 64) + 0.5 = 1
    */
    write32(13, (void *)UART_IBRD);
    write32(1, (void *)UART_FBRD);

    /* 8 bits, no parity, 1 stop bit, enable FIFOs */
    write32((3 << 5) | (1 << 4), (void *)UART_LCR_H);

    /* Enable UART, TX, RX */
    write32(UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE, (void *)UART_CR);

    uart_initialized = true;

    KLOG_UART("UART initialized (RX IRQ armed in start phase when configured)\n");
}

static const serial_ops_t pl011_serial_ops = {
    .putc = pl011_putchar,
    .puts = pl011_putstr,
    .getc = pl011_getchar,
    .getc_nb = pl011_getchar_nb,
    .rx_ready = pl011_rx_available,
};

static int pl011_probe(const device_t *dev)
{
    const serial_cfg_t *cfg = dev->cfg;

    g_pl011_base = device_mmio(cfg->base);
    serial_bind(&pl011_serial_ops);
    pl011_init();
    return 0;
}

/*
   DRV_PHASE_IRQ 的 start：GIC 已在同阶段的 probe pass 里初始化完成，
   此处可以安全注册并打开 UART 中断线。

   失败时降级为轮询并返回 0：SERIAL 是必需 class，start 返回非 0 会触发
   driver_required_panic，但串口本身仍然可用，没必要因此不启动。
*/
static int pl011_start(const device_t *dev)
{
    const serial_cfg_t *cfg = dev ? (const serial_cfg_t *)dev->cfg : NULL;

    if (!uart_initialized || !cfg || !cfg->irq_mode || cfg->irq == 0)
        return 0;

    if (rx_irq_mode)
        return 0;

    if (!irq_available()) {
        KLOG_WARN("pl011: no IRQ controller; RX stays polled\n");
        return 0;
    }

    if (irq_register(cfg->irq, pl011_irq_handler, NULL) != 0) {
        KLOG_WARN("pl011: irq_register(%u) failed; RX stays polled\n", cfg->irq);
        return 0;
    }

    /* 启动前已经输入、还留在硬件 FIFO 里的字节先进软件环 */
    uint64_t flags;
    spin_lock_irqsave(&rx_buffer.lock, &flags);
    (void)pl011_rx_drain_locked();
    spin_unlock_irqrestore(&rx_buffer.lock, flags);

    /* 只打开 RX 和 RX timeout；TX 中断永远保持关闭 */
    write32(UART_INT_RX | UART_INT_RT, (void *)UART_IMSC);

    if (irq_enable(cfg->irq, true) != 0) {
        write32(0, (void *)UART_IMSC);
        KLOG_WARN("pl011: irq_enable(%u) failed; RX stays polled\n", cfg->irq);
        return 0;
    }

    barrier_release();
    rx_irq_mode = true;

    KLOG_UART("pl011: RX IRQ mode, irq=%u\n", cfg->irq);
    return 0;
}

DRIVER_ENTRY(driver_pl011) = {
    .name = "pl011",
    .cls = DRV_SERIAL,
    .phase = DRV_PHASE_EARLY,
    .start_phase = DRV_PHASE_IRQ,
    .probe = pl011_probe,
    .start = pl011_start,
};

/* Non-blocking character output: direct FIFO write, never queue/IRQ. */
bool pl011_putchar_nb(char c)
{
    if (!uart_initialized || uart_tx_fifo_full())
        return false;

    write32((uint32_t)(unsigned char)c, (void *)UART_DR);
    return true;
}

/* Blocking character output (bounded wait; TX stays synchronous). */
void pl011_putchar(char c)
{
    if (!uart_initialized) {
        /* Fallback to direct write if not initialized */
        write32((uint32_t)(unsigned char)c, (void *)UART_DR);
        return;
    }

    for (int timeout = 0; timeout < 10000; timeout++) {
        if (pl011_putchar_nb(c))
            return;
        timer_spin(100);
    }

    /*
       仍然失败：丢弃这个字符, 只计数, 不打印。

       这里不能打日志：pl011_putchar 是 uart_putchar 的实现, 而 klog 是
       持着 g_klog_lock 逐字符调用它的。若在此 logger_warn → klog → 同一把
       锁, 同一颗 CPU, 中断已关 → 自旋死锁。丢弃计数由 pl011_tx_dropped()
       读出去。
    */
    tx_dropped++;
}

/* String output */
void pl011_putstr(const char *str)
{
    while (*str)
        pl011_putchar(*str++);
}

/* Non-blocking character input */
bool pl011_getchar_nb(char *c)
{
    if (!uart_initialized)
        return false;

    bool success = false;
    uint64_t flags;
    spin_lock_irqsave(&rx_buffer.lock, &flags);

    if (rx_irq_mode) {
        success = buffer_get(&rx_buffer, c);
    } else if (!uart_rx_fifo_empty()) {
        /* 轮询模式：直接读硬件，不经过软件环 */
        *c = (char)read32((void *)UART_DR);
        success = true;
    }

    spin_unlock_irqrestore(&rx_buffer.lock, flags);
    return success;
}

/* Check if RX data is available */
bool pl011_rx_available(void)
{
    if (!uart_initialized)
        /* early 模式：直接查硬件 FR.RXFE 位(中断未启用) */
        return !uart_rx_fifo_empty();

    bool available;
    uint64_t flags;
    spin_lock_irqsave(&rx_buffer.lock, &flags);

    if (rx_irq_mode) {
        /*
           IRQ 模式下也要先 drain 硬件：VMM 直启模式运行 guest 时宿主中断
           是关的，ISR 进不来；tick/其他消费者同样可能在这种情况下调用。
        */
        (void)pl011_rx_drain_locked();
        available = !buffer_is_empty(&rx_buffer);
    } else {
        available = !uart_rx_fifo_empty();
    }

    spin_unlock_irqrestore(&rx_buffer.lock, flags);
    return available;
}

uint64_t pl011_tx_dropped(void)
{
    return tx_dropped;
}

/*
   pl011_getchar - 阻塞式读取一个字符

   IRQ 模式优先从软件环取；为空时调用 rx_available() 触发一次硬件 drain，
   等待时不持锁。未初始化时直接轮询硬件 FIFO。
*/
char pl011_getchar(void)
{
    if (!uart_initialized) {
        while (uart_rx_fifo_empty())
            timer_spin(1);
        return (char)(read32((void *)UART_DR) & 0xFF);
    }

    char c;
    while (!pl011_getchar_nb(&c)) {
        if (pl011_rx_available())
            continue;
        timer_spin(1);
    }
    return c;
}
