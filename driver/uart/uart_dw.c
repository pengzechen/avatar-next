#include "driver_cfg.h"
#include "uart/uart_dw.h"
#include "types.h"
#include "mmio.h"
#include "spinlock.h"
#include "klog.h"

// #include "irq.h"
/* WFI：在等待中断时让出 CPU，若架构未定义则用 nop 代替 */
#ifndef WFI
#define WFI() asm volatile("wfi" ::: "memory")
#endif

#define DW_UART_TX_BUFFER_SIZE 1024
#define DW_UART_RX_BUFFER_SIZE 1024

typedef struct
{
    char              buffer[DW_UART_TX_BUFFER_SIZE];
    volatile uint32_t head, tail, count;
    spinlock_noirq_t    lock;
} dw_uart_buffer_t;

static dw_uart_buffer_t tx_buffer           = {0};
static dw_uart_buffer_t rx_buffer           = {0};
static volatile bool    dw_uart_initialized = false;

// 调试计数器
static volatile uint32_t tx_irq_count   = 0;
static volatile uint32_t rx_irq_count   = 0;
static volatile uint32_t last_iir_value = 0;  // 记录最后一次IIR值
static volatile uint32_t tx_sent_total  = 0;  // 总共发送的字节数

static bool
buffer_is_empty(dw_uart_buffer_t *buf)
{
    return buf->count == 0;
}
static bool
buffer_is_full(dw_uart_buffer_t *buf)
{
    return buf->count >= DW_UART_TX_BUFFER_SIZE;
}
static bool
buffer_put(dw_uart_buffer_t *buf, char c)
{
    if (buffer_is_full(buf))
        return false;
    buf->buffer[buf->head] = c;
    buf->head              = (buf->head + 1) % DW_UART_TX_BUFFER_SIZE;
    buf->count++;
    return true;
}
static bool
buffer_get(dw_uart_buffer_t *buf, char *c)
{
    if (buffer_is_empty(buf))
        return false;
    *c        = buf->buffer[buf->tail];
    buf->tail = (buf->tail + 1) % DW_UART_TX_BUFFER_SIZE;
    buf->count--;
    return true;
}

/// DW_UART_THR 空了，可以写入新数据时，硬件会置位
static bool __attribute__((unused)) 
dw_uart_tx_ready(void)
{
    return (read8((void *) DW_UART_LSR) & DW_UART_LSR_THRE) != 0;
}

/// DW_UART_RBR 中的数据可读了，硬件给他置位
static bool
dw_uart_rx_ready(void)
{
    return (read8((void *) DW_UART_LSR) & DW_UART_LSR_DR) != 0;
}

// 启用发送中断， 我有数据可以发送了
static void
dw_uart_enable_tx_interrupt(void)
{
    uint8_t ier = read8((void *) DW_UART_IER);
    if (!(ier & DW_UART_IER_THRI)) {
        // 只在真正需要启用时才打印（避免重复启用的噪音）
        // logger_info("[UART_DEBUG] Enable TX interrupt, buffer=%u\n", tx_buffer.count);
    }
    ier |= DW_UART_IER_THRI;
    write8(ier, (void *) DW_UART_IER);
}

// 禁用发送中断， 我已经没有数据可以发送了
static void
dw_uart_disable_tx_interrupt(void)
{
    uint8_t ier = read8((void *) DW_UART_IER);
    // logger_info("[UART_DEBUG] Disable TX interrupt, buffer=%u\n", tx_buffer.count);
    ier &= ~DW_UART_IER_THRI;
    write8(ier, (void *) DW_UART_IER);
}

// 启用接收中断， 我准备好接收数据了
static void
dw_uart_enable_rx_interrupt(void)
{
    uint8_t ier = read8((void *) DW_UART_IER);
    ier |= DW_UART_IER_RDI;
    write8(ier, (void *) DW_UART_IER);
}

void
dw_uart_interrupt_handler(uint64_t *stack_pointer)
{
    (void)stack_pointer;
    // logger_info("Uart handler invoke...\n");

    uint32_t iir   = read8((void *) DW_UART_IIR) & 0xF;
    last_iir_value = iir;  // 记录IIR值用于调试

    if (iir == 0x4 || iir == 0xC) {  // RX 中断
        rx_irq_count++;              // 调试计数
        spin_lock_irqsave(&rx_buffer.lock);
        while (dw_uart_rx_ready()) {
            char c = (char) read8((void *) DW_UART_RBR);
            // ❌ 不要在中断中打印！会导致死锁和重复输出
            // logger_info("got key: %c\n", c);
            buffer_put(&rx_buffer, c);
        }
        spin_unlock_irqrestore(&rx_buffer.lock);
    }

    // 0x2 = TX Holding Register Empty
    if (iir == 0x2) {  // TX 中断
        tx_irq_count++;  // 调试计数
        spin_lock_irqsave(&tx_buffer.lock);

        int       sent      = 0;
        const int MAX_BATCH = 16;
        while (sent < MAX_BATCH && !buffer_is_empty(&tx_buffer)) {
            char c;
            if (buffer_get(&tx_buffer, &c)) {
                write8((uint8_t) c, (void *) DW_UART_THR);
                sent++;
                tx_sent_total++;  // 记录总共发送的字节数
            }
        }
        bool is_empty = buffer_is_empty(&tx_buffer);
        spin_unlock_irqrestore(&tx_buffer.lock);

        // 在释放锁之后再禁用TX中断，避免竞态条件
        if (is_empty) {
            /*
                它的意义是：
                    当我们已经把所有待发送的数据都写给硬件时，就没必要再关心 TX 中断了。
                因为：
                    硬件下次"THR 空了"时，我们也没数据可以写；
                    如果不关中断，硬件会不停地产生 TX 中断，每次都发现"没数据"，浪费 CPU。
            */
            dw_uart_disable_tx_interrupt();
        }
    }
}

/**
 * dw_uart_wait_idle - 等待 UART 发送器空闲
 *
 * 检查 LSR.TEMT (bit 6): 发送器完全空（TX Holding + TX Shift Register 均空）
 * 适用于标准 16550 / QEMU ns16550（byte-addressed, reg-shift=0）
 */
static void
dw_uart_wait_idle(void)
{
    for (int timeout = 100000; timeout > 0; timeout--) {
        if (read8((void *) DW_UART_LSR) & DW_UART_LSR_TEMT)
            return;
        asm volatile("nop");
    }
    // 超时也继续，避免卡死
}

void
dw_uart_early_init(void)
{
    /*
     * OpenSBI (或 QEMU) 已经完成波特率和帧格式的配置，
     * 这里只需：
     *   1. 关闭中断（轮询模式）
     *   2. 使能 FIFO 并清空收发缓冲
     * 不要碰 DLAB/DLL/DLM，否则会改变波特率导致乱码。
     */

    // 等待 UART 空闲后再配置，避免丢失正在发送的数据
    dw_uart_wait_idle();

    // 关闭所有中断
    write8(0, (void *) DW_UART_IER);

    // 使能 FIFO 并清空收发队列
    write8(DW_UART_FCR_ENABLE_FIFO | DW_UART_FCR_CLEAR_RCVR | DW_UART_FCR_CLEAR_XMIT,
            (void *) DW_UART_FCR);
}

void
dw_uart_init(void)
{
    if (dw_uart_initialized)
        return;

    spinlock_irq_init(&tx_buffer.lock);
    spinlock_irq_init(&rx_buffer.lock);

    tx_buffer.head = tx_buffer.tail = tx_buffer.count = 0;
    rx_buffer.head = rx_buffer.tail = rx_buffer.count = 0;

    // 等待 UART 空闲后再配置，避免丢失正在发送的数据
    dw_uart_wait_idle();

    // 关闭所有中断
    write8(0, (void *) DW_UART_IER);

    // 使能 FIFO 并清空收发队列（不改波特率，OpenSBI 已配好）
    write8(DW_UART_FCR_ENABLE_FIFO | DW_UART_FCR_CLEAR_RCVR | DW_UART_FCR_CLEAR_XMIT,
            (void *) DW_UART_FCR);

    // TODO: 安装中断处理
    // irq_install(DW_UART_IRQ, dw_uart_interrupt_handler);

    // 使能 RX 中断
    dw_uart_enable_rx_interrupt();

    // dw_uart_enable_tx_interrupt();

    // TODO: 配置 GICv3 中断触发模式和目标 CPU
    // gicv3_set_int_trigger(DW_UART_IRQ, 0);  // 设置为电平触发
    // gicv3_set_int_target(DW_UART_IRQ, 0x1);  // 目标 CPU 0
    // gicv3_enable_int(DW_UART_IRQ, true);
    // if (gicv3_is_int_enabled(DW_UART_IRQ)) {
    //     logger_warn("DW UART IRQ %d is enabled in GICv3\n", DW_UART_IRQ);
    // }

    dw_uart_initialized = true;

    logger_info("DWC UART interrupt driver initialized\n");
}

bool
dw_uart_putchar_nb(char c)
{
    if (!dw_uart_initialized)
        return false;
    
    spin_lock_irqsave(&tx_buffer.lock);
    bool success = false;
    bool need_tx_int = false;

    // 处理换行：如果是 '\n'，先尝试发送 '\r'
    if (c == '\n') {
        if (!buffer_put(&tx_buffer, '\r')) {
            // 缓冲区满，连 '\r' 都放不下
            spin_unlock_irqrestore(&tx_buffer.lock);
            return false;
        }
        need_tx_int = true;
    }

    // 尝试放入字符到缓冲区
    success = buffer_put(&tx_buffer, c);
    if (success) {
        need_tx_int = true;
    }
    
    // 如果有数据入队，启用 TX 中断
    if (need_tx_int) {
        dw_uart_enable_tx_interrupt();
    }
    
    spin_unlock_irqrestore(&tx_buffer.lock);
    return success;
}

bool
dw_uart_getchar_nb(char *c)
{
    if (!dw_uart_initialized)
        return false;
    spin_lock_irqsave(&rx_buffer.lock);
    bool success = buffer_get(&rx_buffer, c);
    spin_unlock_irqrestore(&rx_buffer.lock);
    return success;
}

// early init 的时候会阻塞到发送完成 (等待时间未知)
// int init 后会使用缓冲区和中断，一次失败仍会多次尝试 (可以设置重试次数)
void
dw_uart_putchar(char c)
{
    // 如果 UART 尚未初始化，直接写寄存器（阻塞模式）
    if (!dw_uart_initialized) {
        // 如果是 '\n'，先发送 '\r'
        if (c == '\n') {
            while (!(read8((void *) DW_UART_LSR) & DW_UART_LSR_THRE))
                asm volatile("nop");
            write8('\r', (void *) DW_UART_THR);
        }

        // 等待并发送字符
        while (!(read8((void *) DW_UART_LSR) & DW_UART_LSR_THRE))
            asm volatile("nop");
        write8((uint8_t)c, (void *) DW_UART_THR);
        return;
    }
    
    // 已初始化：先尝试非阻塞发送
    if (dw_uart_putchar_nb(c))
        return;

    // 缓冲区满，重试若干次
    for (int retry = 0; retry < 10000; retry++) {
        if (dw_uart_putchar_nb(c))
            return;
        // 短暂延迟，让中断有机会发送数据
        for (int i = 0; i < 100; i++)
            asm volatile("nop");
    }
    
    // 超时仍然失败，丢弃字符
    logger_warn("DWC UART TX buffer full, dropping character\n");
}

// early init 的时候会阻塞到接收数据 (等待时间未知)
// int init 后会使用缓冲区和中断，如果无数据则睡眠 WFI
char
dw_uart_getchar(void)
{
    char c;

    // 如果已经完整初始化（中断模式），使用缓冲区
    if (dw_uart_initialized) {
        while (!dw_uart_getchar_nb(&c)) {
            // 等待数据到来
            WFI();
        }
        return c;
    }

    // 否则直接轮询硬件（early init 模式或未初始化）
    // 等待数据可用
    while (!(read8((void *) DW_UART_LSR) & DW_UART_LSR_DR)) {
        asm volatile("nop");
    }

    return (char) read8((void *) DW_UART_RBR);
}

void
dw_uart_putstr(const char *str)
{
    while (*str)
        dw_uart_putchar(*str++);
}


// ================= 其它辅助函数 =================

/*
    软件缓冲区 → [中断] → THR → TSR → 串口线
        ↑                    ↑      ↑
    第一阶段检查          THRE   TEMT (第二阶段检查)
*/
void
dw_uart_flush(void)
{
    if (!dw_uart_initialized)
        return;
    
    // 第一阶段：等待软件缓冲区为空
    for (int timeout = 100000; timeout > 0; timeout--) {
        spin_lock_irqsave(&tx_buffer.lock);
        bool empty = buffer_is_empty(&tx_buffer);
        spin_unlock_irqrestore(&tx_buffer.lock);
        
        if (empty)
            break;
            
        for (int i = 0; i < 10; i++)
            asm volatile("nop");
    }
    
    // 第二阶段：等待硬件发送完成（THR 和 TSR 都为空）
    // TEMT (Transmitter Empty) 位表示发送器完全空闲
    for (int timeout = 10000; timeout > 0; timeout--) {
        uint32_t lsr = read32((void *) DW_UART_LSR);
        if (lsr & DW_UART_LSR_TEMT)
            break;
            
        for (int i = 0; i < 10; i++)
            asm volatile("nop");
    }
}

bool
dw_uart_rx_available(void)
{
    if (!dw_uart_initialized)
        /* early 模式：直接查 LSR 硬件寄存器（中断未启用，数据不经缓冲区） */
        return dw_uart_rx_ready();
    spin_lock_irqsave(&rx_buffer.lock);
    bool available = !buffer_is_empty(&rx_buffer);
    spin_unlock_irqrestore(&rx_buffer.lock);
    return available;
}

// ================= Debug Functions =================

uint32_t
dw_uart_tx_buffer_usage(void)
{
    if (!dw_uart_initialized)
        return 0;
    spin_lock_irqsave(&tx_buffer.lock);
    uint32_t usage = tx_buffer.count;
    spin_unlock_irqrestore(&tx_buffer.lock);
    return usage;
}

void
dw_uart_get_stats(uint32_t *tx_irqs, uint32_t *rx_irqs, uint32_t *tx_usage, uint32_t *rx_usage)
{
    if (tx_irqs)
        *tx_irqs = tx_irq_count;
    if (rx_irqs)
        *rx_irqs = rx_irq_count;
    if (tx_usage)
        *tx_usage = dw_uart_tx_buffer_usage();
    if (rx_usage) {
        spin_lock_irqsave(&rx_buffer.lock);
        *rx_usage = rx_buffer.count;
        spin_unlock_irqrestore(&rx_buffer.lock);
    }
}

// 调试函数：检查TX中断是否启用
bool
dw_uart_is_tx_interrupt_enabled(void)
{
    uint32_t ier = read32((void *) DW_UART_IER);
    return (ier & DW_UART_IER_THRI) != 0;
}

// 调试函数：获取最后一次IIR值
uint32_t
dw_uart_get_last_iir(void)
{
    return last_iir_value;
}

// 调试函数：获取总共发送的字节数
uint32_t
dw_uart_get_tx_sent_total(void)
{
    return tx_sent_total;
}