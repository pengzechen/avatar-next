#ifndef __T_DW_UART_H__
#define __T_DW_UART_H__

#include "types.h"
#include "driver_cfg.h"
#include "uart_16550.h"   /* 共享 16550 寄存器偏移与位定义 */

/*
 * DesignWare UART 基地址来自 driver_cfg.h（ARCH_RISCV64 → 0x10000000）
 *
 * QEMU virt 机器的 ns16550 UART 使用字节寻址（reg-shift=0），
 * 寄存器偏移与标准 16550 PIO 布局相同：0,1,2,3,4,5,6,7。
 * 必须使用 read8/write8（8-bit 访问），32-bit 访问会访问到错误寄存器。
 */

#define DW_UART_BASE    (UART_BASE)

/* 绝对地址（字节偏移，reg-shift=0） */
#define DW_UART_RBR  (DW_UART_BASE + UART16550_PIO_RBR)   /* Receive Buffer   */
#define DW_UART_THR  (DW_UART_BASE + UART16550_PIO_THR)   /* Transmit Holding */
#define DW_UART_IER  (DW_UART_BASE + UART16550_PIO_IER)   /* Interrupt Enable */
#define DW_UART_IIR  (DW_UART_BASE + UART16550_PIO_IIR)   /* Interrupt ID     */
#define DW_UART_FCR  (DW_UART_BASE + UART16550_PIO_FCR)   /* FIFO Control     */
#define DW_UART_LCR  (DW_UART_BASE + UART16550_PIO_LCR)   /* Line Control     */
#define DW_UART_MCR  (DW_UART_BASE + UART16550_PIO_MCR)   /* Modem Control    */
#define DW_UART_LSR  (DW_UART_BASE + UART16550_PIO_LSR)   /* Line Status      */
#define DW_UART_MSR  (DW_UART_BASE + UART16550_PIO_MSR)   /* Modem Status     */
#define DW_UART_SCR  (DW_UART_BASE + 7U)                  /* Scratch          */
#define DW_UART_DLL  (DW_UART_BASE + UART16550_PIO_DLL)   /* Divisor Low (DLAB=1)  */
#define DW_UART_DLM  (DW_UART_BASE + UART16550_PIO_DLM)   /* Divisor High (DLAB=1) */
/* DesignWare USR 寄存器在字节布局中不可靠（QEMU 无此寄存器），不再使用 */

/* LSR 位 — 别名自 uart_16550.h */
#define DW_UART_LSR_DR    UART16550_LSR_DR
#define DW_UART_LSR_THRE  UART16550_LSR_THRE
#define DW_UART_LSR_TEMT  UART16550_LSR_TEMT

/* IER 位 — 别名自 uart_16550.h */
#define DW_UART_IER_RDI   UART16550_IER_RDI
#define DW_UART_IER_THRI  UART16550_IER_THRI

/* FCR 位 — 别名自 uart_16550.h */
#define DW_UART_FCR_ENABLE_FIFO  UART16550_FCR_ENABLE_FIFO
#define DW_UART_FCR_CLEAR_RCVR   UART16550_FCR_CLEAR_RX
#define DW_UART_FCR_CLEAR_XMIT   UART16550_FCR_CLEAR_TX

/* LCR 位 — 别名自 uart_16550.h */
#define DW_UART_LCR_DLAB  UART16550_LCR_DLAB

// TODO: 替换为实际中断号
#define DW_UART_IRQ UART_IRQ

// Early init (无中断，用于启动早期)
void dw_uart_early_init(void);

// 完整初始化 (带中断)
void dw_uart_init(void);

// 输出函数
void dw_uart_putchar(char c);
bool dw_uart_putchar_nb(char c);
void dw_uart_putstr(const char *str);
void dw_uart_flush(void);

// 输入函数
char dw_uart_getchar(void);       // 阻塞读取（自动判断是否已初始化）
bool dw_uart_getchar_nb(char *c); // 非阻塞读取
bool dw_uart_rx_available(void);

uint32_t
dw_uart_tx_buffer_usage(void);
void dw_uart_get_stats(uint32_t *tx_irqs, uint32_t *rx_irqs, uint32_t *tx_usage, uint32_t *rx_usage);
bool dw_uart_is_tx_interrupt_enabled(void);
uint32_t
dw_uart_get_last_iir(void);
uint32_t
dw_uart_get_tx_sent_total(void);
void dw_uart_interrupt_handler(uint64_t *stack_pointer);

#endif // __T_DW_UART_H__