#ifndef __T_DW_UART_H__
#define __T_DW_UART_H__

#include "types.h"
#include "platform_cfg.h"
#include "uart_16550.h"   /* 共享 16550 寄存器偏移与位定义 */

/*
 * DesignWare UART 基地址（由 dw_uart_early_init() 从 platform_get_mmio 填充）
 *
 * QEMU virt 机器的 ns16550 UART 使用字节寻址（reg-shift=0），
 * 寄存器偏移与标准 16550 PIO 布局相同：0,1,2,3,4,5,6,7。
 * 必须使用 read8/write8（8-bit 访问），32-bit 访问会访问到错误寄存器。
 */

extern uintptr_t dw_uart_base;
/* reg_shift: 0 = 字节寻址 (QEMU ns16550)；2 = 4字节 MMIO (SG2002 DW-APB) */
extern uint8_t   dw_uart_reg_shift;

#define DW_UART_BASE    (dw_uart_base)

/*
 * _DW_REG(n): 寄存器绝对地址 = base + (reg_index << reg_shift)
 *   reg_shift=0: offset 0,1,2,3,4,5,6,7     (PIO 字节偏移)
 *   reg_shift=2: offset 0,4,8,12,16,20,24,28 (MMIO 4字节偏移)
 */
#define _DW_REG(n)   (DW_UART_BASE + ((uintptr_t)(n) << dw_uart_reg_shift))

#define DW_UART_RBR  _DW_REG(0)   /* Receive Buffer   */
#define DW_UART_THR  _DW_REG(0)   /* Transmit Holding */
#define DW_UART_IER  _DW_REG(1)   /* Interrupt Enable */
#define DW_UART_IIR  _DW_REG(2)   /* Interrupt ID     */
#define DW_UART_FCR  _DW_REG(2)   /* FIFO Control     */
#define DW_UART_LCR  _DW_REG(3)   /* Line Control     */
#define DW_UART_MCR  _DW_REG(4)   /* Modem Control    */
#define DW_UART_LSR  _DW_REG(5)   /* Line Status      */
#define DW_UART_MSR  _DW_REG(6)   /* Modem Status     */
#define DW_UART_SCR  _DW_REG(7)   /* Scratch          */
#define DW_UART_DLL  _DW_REG(0)   /* Divisor Low  (DLAB=1) */
#define DW_UART_DLM  _DW_REG(1)   /* Divisor High (DLAB=1) */

/*
 * dw_reg_r8 / dw_reg_w8 — 寄存器读写辅助（自动适配 reg_shift）
 *   reg_shift=0 → read8/write8（8-bit）
 *   reg_shift=2 → read32/write32（32-bit，有效数据在低 8 位）
 */
#include "mmio.h"
static inline uint8_t dw_reg_r8(uintptr_t abs_addr)
{
    if (dw_uart_reg_shift == 2)
        return (uint8_t)read32((void *)abs_addr);
    return read8((void *)abs_addr);
}
static inline void dw_reg_w8(uint8_t val, uintptr_t abs_addr)
{
    if (dw_uart_reg_shift == 2)
        write32((uint32_t)val, (void *)abs_addr);
    else
        write8(val, (void *)abs_addr);
}

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