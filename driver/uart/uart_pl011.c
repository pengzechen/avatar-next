/*
 * UART interrupt-driven driver for PL011
 * 
 * This implements interrupt-based UART communication to replace
 * the busy-waiting approach.
 */

#include "uart_pl011.h"
#include "types.h"
#include "mmio.h"
#include "klog.h"
#include "driver_cfg.h"
// #include "irq.h"
#include "spinlock.h"

/*
 * UART 基地址运行时变量
 * 初始值为物理地址，供 MMU 开启前的早期 boot 使用。
 * kernel_main() 开始处会更新为 KERNEL_VMA + 0x09000000。
 */
volatile uintptr_t g_pl011_base = 0x09000000UL;

// Circular buffer for transmit data
#define UART_TX_BUFFER_SIZE 1024
#define UART_RX_BUFFER_SIZE 1024

typedef struct {
    char buffer[UART_TX_BUFFER_SIZE];
    volatile uint32_t head;
    volatile uint32_t tail;
    volatile uint32_t count;
    spinlock_t lock;
} uart_buffer_t;

static uart_buffer_t tx_buffer = {0};
static uart_buffer_t rx_buffer = {0};
static volatile bool uart_initialized = false;

// Buffer operations
static bool buffer_is_empty(uart_buffer_t *buf) {
    return buf->count == 0;
}

static bool buffer_is_full(uart_buffer_t *buf) {
    return buf->count >= UART_TX_BUFFER_SIZE;
}

static bool buffer_put(uart_buffer_t *buf, char c) {
    if (buffer_is_full(buf)) {
        return false;
    }
    
    buf->buffer[buf->head] = c;
    buf->head = (buf->head + 1) % UART_TX_BUFFER_SIZE;
    buf->count++;
    return true;
}

static bool buffer_get(uart_buffer_t *buf, char *c) {
    if (buffer_is_empty(buf)) {
        return false;
    }
    
    *c = buf->buffer[buf->tail];
    buf->tail = (buf->tail + 1) % UART_TX_BUFFER_SIZE;
    buf->count--;
    return true;
}

// UART hardware operations
static void uart_enable_tx_interrupt(void) {
    uint32_t imsc = read32((void *)UART_IMSC);
    imsc |= UART_INT_TX;
    write32(imsc, (void *)UART_IMSC);
}

static void uart_disable_tx_interrupt(void) {
    uint32_t imsc = read32((void *)UART_IMSC);
    imsc &= ~UART_INT_TX;
    write32(imsc, (void *)UART_IMSC);
}

static void uart_enable_rx_interrupt(void) {
    uint32_t imsc = read32((void *)UART_IMSC);
    imsc |= (UART_INT_RX | UART_INT_RT);
    write32(imsc, (void *)UART_IMSC);
}

static bool uart_tx_fifo_full(void) {
    return (read32((void *)UART_FR) & UART_FR_TXFF) != 0;
}

static bool uart_rx_fifo_empty(void) {
    return (read32((void *)UART_FR) & UART_FR_RXFE) != 0;
}

// UART interrupt handler
void uart_interrupt_handler(uint64_t *stack_pointer) {
    (void)stack_pointer;
    uint32_t mis = read32((void *)UART_MIS);
    
    // Handle transmit interrupt
    if (mis & UART_INT_TX) {
        spin_lock(&tx_buffer.lock);
        
        // Send as many characters as possible
        while (!uart_tx_fifo_full() && !buffer_is_empty(&tx_buffer)) {
            char c;
            if (buffer_get(&tx_buffer, &c)) {
                write32((uint32_t)c, (void *)UART_DR);
            }
        }
        
        // If buffer is empty, disable TX interrupt
        if (buffer_is_empty(&tx_buffer)) {
            uart_disable_tx_interrupt();
        }
        
        spin_unlock(&tx_buffer.lock);
        
        // Clear TX interrupt
        write32(UART_INT_TX, (void *)UART_ICR);
    }
    
    // Handle receive interrupt
    if (mis & (UART_INT_RX | UART_INT_RT)) {
        spin_lock(&rx_buffer.lock);
        
        // Read all available characters
        while (!uart_rx_fifo_empty()) {
            char c = (char)read32((void *)UART_DR);
            if (!buffer_is_full(&rx_buffer)) {
                buffer_put(&rx_buffer, c);
            }
            // If buffer is full, we drop the character
        }
        
        spin_unlock(&rx_buffer.lock);
        
        // Clear RX interrupts
        write32(UART_INT_RX | UART_INT_RT, (void *)UART_ICR);
    }
}

// Initialize UART with interrupt support
void pl011_init(void) {
    if (uart_initialized) {
        return;
    }
    
    // Initialize buffers
    spinlock_init(&tx_buffer.lock);
    spinlock_init(&rx_buffer.lock);
    tx_buffer.head = tx_buffer.tail = tx_buffer.count = 0;
    rx_buffer.head = rx_buffer.tail = rx_buffer.count = 0;
    
    // Disable UART first
    write32(0, (void *)UART_CR);
    
    // Clear all interrupts
    write32(0x7FF, (void *)UART_ICR);
    
    // Configure UART (115200 baud, 8N1)
    // For 24MHz clock: IBRD = 24000000 / (16 * 115200) = 13
    // FBRD = (0.0208 * 64) + 0.5 = 1
    write32(13, (void *)UART_IBRD);
    write32(1, (void *)UART_FBRD);
    
    // 8 bits, no parity, 1 stop bit, enable FIFOs
    write32((3 << 5) | (1 << 4), (void *)UART_LCR_H);
    
    // Enable UART, TX, RX
    write32(UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE, (void *)UART_CR);
    
    // Enable RX interrupts (TX interrupts enabled on demand)
    uart_enable_rx_interrupt();
    
    // TODO: Install UART interrupt handler
    // Install interrupt handler
    // irq_install(UART_IRQ, uart_interrupt_handler);
    // Enable UART interrupt in GIC
    // gic_enable_int(UART_IRQ, 0);
    
    // uart_initialized = true;
    
    logger_info("UART interrupt driver initialized\n");
}

// Non-blocking character output
bool pl011_putchar_nb(char c) {
    if (!uart_initialized) {
        return false;
    }
    
    spin_lock(&tx_buffer.lock);
    
    bool success = false;
    
    // Try to put directly to FIFO if buffer is empty and FIFO not full
    if (buffer_is_empty(&tx_buffer) && !uart_tx_fifo_full()) {
        write32((uint32_t)c, (void *)UART_DR);
        success = true;
    } else {
        // Put to buffer
        success = buffer_put(&tx_buffer, c);
        if (success) {
            // Enable TX interrupt to drain the buffer
            uart_enable_tx_interrupt();
        }
    }
    
    spin_unlock(&tx_buffer.lock);
    return success;
}

// Blocking character output (with timeout)
void pl011_putchar(char c) {
    if (!uart_initialized) {
        // Fallback to direct write if not initialized
        volatile unsigned int *const UART0DR = (unsigned int *)UART_DR;
        *UART0DR = (unsigned int)c;
        return;
    }
    
    // Try non-blocking first
    if (pl011_putchar_nb(c)) {
        return;
    }
    
    // If buffer is full, wait a bit and retry
    int timeout = 10000;
    while (timeout-- > 0) {
        if (pl011_putchar_nb(c)) {
            return;
        }
        // Small delay
        for (int i = 0; i < 100; i++) {
            asm volatile("nop");
        }
    }
    
    // If still failed, drop the character
    logger_warn("UART TX buffer full, dropping character\n");
}

// String output
void pl011_putstr(const char *str) {
    while (*str) {
        pl011_putchar(*str++);
    }
}

// Non-blocking character input
bool pl011_getchar_nb(char *c) {
    if (!uart_initialized) {
        return false;
    }
    
    spin_lock(&rx_buffer.lock);
    bool success = buffer_get(&rx_buffer, c);
    spin_unlock(&rx_buffer.lock);
    
    return success;
}

// Check if RX data is available
bool pl011_rx_available(void) {
    if (!uart_initialized)
        /* early 模式：直接查硬件 FR.RXFE 位（中断未启用） */
        return !uart_rx_fifo_empty();

    spin_lock(&rx_buffer.lock);
    bool available = !buffer_is_empty(&rx_buffer);
    spin_unlock(&rx_buffer.lock);
    return available;
}

// Get TX buffer usage
uint32_t pl011_tx_buffer_usage(void) {
    if (!uart_initialized) {
        return 0;
    }
    
    spin_lock(&tx_buffer.lock);
    uint32_t usage = tx_buffer.count;
    spin_unlock(&tx_buffer.lock);
    
    return usage;
}

/*
 * pl011_getchar - 阻塞式读取一个字符（硬件轮询）
 *
 * 优先从中断驱动的 rx_buffer 中取；若 UART 未初始化，
 * 直接轮询硬件 FIFO（UART_FR_RXFE）。
 */
char pl011_getchar(void) {
    if (uart_initialized) {
        /* 优先尝试软件缓冲区 */
        char c;
        while (!buffer_get(&rx_buffer, &c)) {
            /* 中断未启用时直接轮询硬件 FIFO */
            if (!(read32((void *)UART_FR) & UART_FR_RXFE)) {
                return (char)(read32((void *)UART_DR) & 0xFF);
            }
        }
        return c;
    } else {
        /* 未初始化：直接轮询硬件 FIFO */
        while (read32((void *)UART_FR) & UART_FR_RXFE)
            ;
        return (char)(read32((void *)UART_DR) & 0xFF);
    }
}
