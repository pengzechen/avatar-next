/*
 * driver/uart/uart_16550.h — NS16550A / 8250 UART 共享寄存器定义
 *
 * 两种地址布局：
 *   UART16550_MMIO_*  : 寄存器按 4 字节对齐，供 DesignWare (RISC-V) 使用
 *   UART16550_PIO_*   : 寄存器按字节对齐，供 x86 COM1 Port I/O 使用
 *
 * 位定义 (LSR/IER/FCR/LCR/IIR) 两种布局共享，只需包含一次。
 */

#ifndef DRIVER_UART_16550_H
#define DRIVER_UART_16550_H

/* -----------------------------------------------------------------------
 * Register offsets — MMIO 32-bit (DesignWare, 4 bytes per register)
 * ----------------------------------------------------------------------- */
#define UART16550_MMIO_RBR  0x00U   /* Receive Buffer Register (read)        */
#define UART16550_MMIO_THR  0x00U   /* Transmit Holding Register (write)     */
#define UART16550_MMIO_IER  0x04U   /* Interrupt Enable Register             */
#define UART16550_MMIO_IIR  0x08U   /* Interrupt ID Register (read)          */
#define UART16550_MMIO_FCR  0x08U   /* FIFO Control Register (write)         */
#define UART16550_MMIO_LCR  0x0CU   /* Line Control Register                 */
#define UART16550_MMIO_MCR  0x10U   /* Modem Control Register                */
#define UART16550_MMIO_LSR  0x14U   /* Line Status Register                  */
#define UART16550_MMIO_MSR  0x18U   /* Modem Status Register                 */
#define UART16550_MMIO_SCR  0x1CU   /* Scratch Register                      */
#define UART16550_MMIO_DLL  0x00U   /* Divisor Latch Low  (DLAB=1)           */
#define UART16550_MMIO_DLM  0x04U   /* Divisor Latch High (DLAB=1)           */

/* -----------------------------------------------------------------------
 * Register offsets — Port I/O byte-addressed (x86 COM1)
 * ----------------------------------------------------------------------- */
#define UART16550_PIO_RBR   0U      /* Receive Buffer Register (read)        */
#define UART16550_PIO_THR   0U      /* Transmit Holding Register (write)     */
#define UART16550_PIO_IER   1U      /* Interrupt Enable Register             */
#define UART16550_PIO_IIR   2U      /* Interrupt ID Register (read)          */
#define UART16550_PIO_FCR   2U      /* FIFO Control Register (write)         */
#define UART16550_PIO_LCR   3U      /* Line Control Register                 */
#define UART16550_PIO_MCR   4U      /* Modem Control Register                */
#define UART16550_PIO_LSR   5U      /* Line Status Register                  */
#define UART16550_PIO_MSR   6U      /* Modem Status Register                 */
#define UART16550_PIO_DLL   0U      /* Divisor Latch Low  (DLAB=1)           */
#define UART16550_PIO_DLM   1U      /* Divisor Latch High (DLAB=1)           */

/* -----------------------------------------------------------------------
 * LSR — Line Status Register bits
 * ----------------------------------------------------------------------- */
#define UART16550_LSR_DR    (1U << 0)  /* Data Ready (RX data available)     */
#define UART16550_LSR_OE    (1U << 1)  /* Overrun Error                      */
#define UART16550_LSR_PE    (1U << 2)  /* Parity Error                       */
#define UART16550_LSR_FE    (1U << 3)  /* Framing Error                      */
#define UART16550_LSR_BI    (1U << 4)  /* Break Interrupt                    */
#define UART16550_LSR_THRE  (1U << 5)  /* TX Holding Register Empty          */
#define UART16550_LSR_TEMT  (1U << 6)  /* Transmitter Empty                  */

/* -----------------------------------------------------------------------
 * IER — Interrupt Enable Register bits
 * ----------------------------------------------------------------------- */
#define UART16550_IER_RDI   (1U << 0)  /* Receive Data Available IRQ         */
#define UART16550_IER_THRI  (1U << 1)  /* TX Holding Register Empty IRQ      */

/* -----------------------------------------------------------------------
 * FCR — FIFO Control Register bits
 * ----------------------------------------------------------------------- */
#define UART16550_FCR_ENABLE_FIFO  (1U << 0)  /* Enable FIFO                 */
#define UART16550_FCR_CLEAR_RX     (1U << 1)  /* Clear RX FIFO               */
#define UART16550_FCR_CLEAR_TX     (1U << 2)  /* Clear TX FIFO               */

/* -----------------------------------------------------------------------
 * LCR — Line Control Register bits
 * ----------------------------------------------------------------------- */
#define UART16550_LCR_WLS_8  0x03U     /* 8 data bits                        */
#define UART16550_LCR_DLAB   (1U << 7) /* Divisor Latch Access Bit           */

/* -----------------------------------------------------------------------
 * IIR — Interrupt Identification Register (low nibble)
 * ----------------------------------------------------------------------- */
#define UART16550_IIR_NO_INT  0x01U   /* No interrupt pending                */
#define UART16550_IIR_THRI    0x02U   /* TX Holding Register Empty           */
#define UART16550_IIR_RDI     0x04U   /* Received Data Available             */
#define UART16550_IIR_RDTO    0x0CU   /* Receive Data Timeout (FIFO mode)    */
#define UART16550_IIR_MASK    0x0FU   /* Interrupt type mask                 */

#endif /* DRIVER_UART_16550_H */
