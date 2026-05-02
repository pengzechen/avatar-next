
#ifndef __T_UART_PL011_H__
#define __T_UART_PL011_H__

#include "types.h"
#include "driver_cfg.h"

/*
 * UART 基地址运行时变量
 *
 * MMU 开启前（vm_init 等早期 boot 中的 KLOG）：物理地址 0x09000000
 * MMU 开启后（kernel_main 开始处更新）：KERNEL_VMA + 0x09000000
 *
 * 使用运行时变量而非编译期常量，避免 MMU 开启前访问不存在的高地址。
 */
extern volatile uintptr_t g_pl011_base;

// UART PL011 register definitions
#define UART_BASE_ADDR  g_pl011_base

#define UART_DR         (UART_BASE_ADDR + 0x000)  // Data Register
#define UART_RSR        (UART_BASE_ADDR + 0x004)  // Receive Status Register
#define UART_FR         (UART_BASE_ADDR + 0x018)  // Flag Register
#define UART_ILPR       (UART_BASE_ADDR + 0x020)  // IrDA Low-Power Counter Register
#define UART_IBRD       (UART_BASE_ADDR + 0x024)  // Integer Baud Rate Register
#define UART_FBRD       (UART_BASE_ADDR + 0x028)  // Fractional Baud Rate Register
#define UART_LCR_H      (UART_BASE_ADDR + 0x02C)  // Line Control Register
#define UART_CR         (UART_BASE_ADDR + 0x030)  // Control Register
#define UART_IFLS       (UART_BASE_ADDR + 0x034)  // Interrupt FIFO Level Select Register
#define UART_IMSC       (UART_BASE_ADDR + 0x038)  // Interrupt Mask Set/Clear Register
#define UART_RIS        (UART_BASE_ADDR + 0x03C)  // Raw Interrupt Status Register
#define UART_MIS        (UART_BASE_ADDR + 0x040)  // Masked Interrupt Status Register
#define UART_ICR        (UART_BASE_ADDR + 0x044)  // Interrupt Clear Register

// UART Flag Register bits
#define UART_FR_TXFF    (1 << 5)  // Transmit FIFO full
#define UART_FR_RXFE    (1 << 4)  // Receive FIFO empty
#define UART_FR_BUSY    (1 << 3)  // UART busy

// UART Interrupt bits
#define UART_INT_TX     (1 << 5)  // Transmit interrupt
#define UART_INT_RX     (1 << 4)  // Receive interrupt
#define UART_INT_RT     (1 << 6)  // Receive timeout interrupt

// UART Control Register bits
#define UART_CR_UARTEN  (1 << 0)  // UART enable
#define UART_CR_TXE     (1 << 8)  // Transmit enable
#define UART_CR_RXE     (1 << 9)  // Receive enable

/* -----------------------------------------------------------------------
 * 函数声明（均以 pl011_ 为前缀，避免与 platform 层的 uart_putchar 冲突）
 * ----------------------------------------------------------------------- */
void     pl011_init(void);
void     pl011_putchar(char c);
void     pl011_putstr(const char *s);
bool     pl011_putchar_nb(char c);
char     pl011_getchar(void);
bool     pl011_getchar_nb(char *c);
bool     pl011_rx_available(void);
uint32_t pl011_tx_buffer_usage(void);

#endif // __T_UART_PL011_H__