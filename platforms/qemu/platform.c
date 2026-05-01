/*
 * QEMU platform implementation
 * Provides UART output for QEMU virt machines
 */

#include "../../boot/common/platform.h"
#include "mmio.h"
#include "types.h"
#include "arch.h"
#if ARCH_X86_64
#include "io.h"
#endif

/*
 * QEMU UART base addresses for different architectures
 */
#if ARCH_AARCH64
    /* QEMU virt (AArch64): UART at 0x09000000 */
    #define UART_BASE    0x09000000
    #define UART_IS_MMIO 1
#elif ARCH_RISCV64
    /* QEMU virt (RISC-V): UART at 0x10000000 */
    #define UART_BASE    0x10000000
    #define UART_IS_MMIO 1
#elif ARCH_X86_64
    /* QEMU PC (x86_64): Use serial port at 0x3F8 (COM1) */
    #define UART_BASE    0x3F8
    #define UART_IS_MMIO 0
#else
    #error "Unsupported architecture"
#endif

/*
 * UART registers (8-bit registers)
 */
#define UART_RBR    0    /* Receive Buffer Register (read) */
#define UART_THR    0    /* Transmit Holding Register (write) */
#define UART_LSR    5    /* Line Status Register */

/*
 * Line Status Register bits
 */
#define UART_LSR_THRE   (1 << 5)  /* Transmit-hold-register empty */
#define UART_LSR_TEMT   (1 << 6)  /* Transmitter empty */

/*
 * UART I/O functions
 */

/* Check if UART is ready to transmit */
static inline int uart_tx_ready(void)
{
#if UART_IS_MMIO
    /* AArch64 and RISC-V use memory-mapped I/O */
    return (mmio_readb((void *)(UART_BASE + UART_LSR)) & UART_LSR_THRE) != 0;
#else
    /* x86_64 uses port I/O */
    return (inb(UART_BASE + UART_LSR) & UART_LSR_THRE) != 0;
#endif
}

/* Wait until UART is ready to transmit */
static inline void uart_wait_tx_ready(void)
{
    while (!uart_tx_ready()) {
        /* Busy wait */
    }
}

/* Transmit a single character */
static void qemu_uart_putc(char c)
{
#if UART_IS_MMIO
    #if ARCH_AARCH64
        /* AArch64 uses PL011 UART (32-bit data register) */
        volatile uint32_t *uart_dr = (volatile uint32_t *)UART_BASE;
        *uart_dr = (uint32_t)c;
    #elif ARCH_RISCV64
        /* RISC-V uses 16550 UART (8-bit data register) */
        volatile uint8_t *uart_dr = (volatile uint8_t *)UART_BASE;
        *uart_dr = (uint8_t)c;
    #endif
#else
    /* x86_64 uses port I/O */
    uart_wait_tx_ready();
    outb(UART_BASE + UART_THR, c);
#endif
}

/* Transmit a null-terminated string */
static void qemu_uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            /* Convert LF to CRLF */
            qemu_uart_putc('\r');
        }
        qemu_uart_putc(*s++);
    }
}

/*
 * UART output functions for klog
 */
void uart_putchar(char c)
{
    qemu_uart_putc(c);
}

void uart_putstr(const char *s)
{
    qemu_uart_puts(s);
}

/* Platform panic - hang forever */
static void qemu_panic(void) __attribute__((noreturn));

static void qemu_panic(void)
{
    /* Disable interrupts */
#if ARCH_AARCH64
    __asm__ volatile("msr daifset, #0xF" ::: "memory");
#elif ARCH_RISCV64
    /* RISC-V: Disable all interrupts in S-mode */
    __asm__ volatile(
        "csrw sie, zero\n"     /* Disable supervisor interrupt enable */
        "csrw sip, zero\n"     /* Clear supervisor interrupt pending */
        ::: "memory"
    );
#elif ARCH_X86_64
    __asm__ volatile("cli" ::: "memory");
#endif

    /* Hang */
    while (1) {
#if ARCH_AARCH64
        __asm__ volatile("wfe");
#elif ARCH_RISCV64
        /* RISC-V: Use infinite loop with wfi, but also add a memory barrier */
        __asm__ volatile("wfi" ::: "memory");
#elif ARCH_X86_64
        __asm__ volatile("hlt");
#endif
    }

    /* Never reached */
    __builtin_unreachable();
}

/* Platform shutdown - ask QEMU to exit */
static void qemu_shutdown(void) __attribute__((noreturn));

static void qemu_shutdown(void)
{
#if ARCH_AARCH64 || ARCH_RISCV64
        /*
        * QEMU virt: write "shutdown" to the QEMU Power Management register.
        * On the QEMU virt machine this is a syscon-poweroff device at
        * 0x08000000 (AArch64) / 0x100000 (RISC-V) -- but the portable
        * way that works on both is the SBI SRST extension (RISC-V) or
        * the PSCI SYSTEM_OFF call (AArch64).
        */
    #if ARCH_AARCH64
        /* PSCI SYSTEM_OFF via HVC (SMCCC 32-bit convention, funcid 0x84000008) */
        register unsigned long x0 __asm__("x0") = 0x84000008UL;
        __asm__ volatile("hvc #0" :: "r"(x0) : "memory");
    #elif ARCH_RISCV64
        /* SBI SRST extension: sbi_system_reset(SHUTDOWN, GRACEFUL) */
        register unsigned long a7 __asm__("a7") = 0x53525354UL; /* SBI_EXT_SRST */
        register unsigned long a6 __asm__("a6") = 0x0UL;        /* SBI_SRST_RESET */
        register unsigned long a0 __asm__("a0") = 0x0UL;        /* reset_type: shutdown */
        register unsigned long a1 __asm__("a1") = 0x0UL;        /* reason: no reason */
        __asm__ volatile("ecall"
            : "+r"(a0)
            : "r"(a7), "r"(a6), "r"(a1)
            : "memory");
    #endif
#elif ARCH_X86_64
    /*
     * x86_64 QEMU: write 0x2000 to ACPI PM1a control port (0x604)
     * This triggers an ACPI power-off on QEMU Q35/i440fx machines.
     * PM1a_CNT is a 16-bit register; must be written as a single outw.
     */
    outw(0x604, 0x2000);
#endif

    /* Fallback: hang if shutdown did not take effect */
    while (1) {
        #if ARCH_AARCH64
        __asm__ volatile("wfe");
        #elif ARCH_RISCV64
        __asm__ volatile("wfi" ::: "memory");
        #elif ARCH_X86_64
        __asm__ volatile("hlt");
        #endif
    }
    __builtin_unreachable();
}

/*
 * Platform operations structure
 */
static struct platform_ops qemu_platform = {
    .uart_putc = qemu_uart_putc,
    .uart_puts = qemu_uart_puts,
    .panic    = qemu_panic,
    .shutdown = qemu_shutdown,
};

/*
 * Platform initialization
 */
void platform_init(void)
{
    /* UART is initialized by QEMU, no setup needed */
}

/*
 * Get platform operations
 */
struct platform_ops *platform_get_ops(void)
{
    return &qemu_platform;
}

/*
 * Platform panic wrapper
 */
void do_platform_panic(void)
{
    qemu_panic();
}

/*
 * Platform shutdown wrapper
 */
void do_platform_shutdown(void)
{
    qemu_shutdown();
}
