/*
 * QEMU platform implementation
 * Provides UART output for QEMU virt machines
 */

#include "../../boot/common/platform.h"
#include "types.h"
#include "arch.h"
#include "uart/uart.h"   /* 统一 UART 驱动，根据架构自动选择 */
#if ARCH_X86_64
#include "x86_64/io.h"          /* x86 Port I/O: outw 用于 ACPI shutdown */
#endif

/* Transmit a single character */
static void qemu_uart_putc(char c)
{
    uart_putc(c);
}

/* Transmit a null-terminated string */
static void qemu_uart_puts(const char *s)
{
    uart_puts(s);
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
        /*
         * VHE 模式下内核运行在 EL2，无法用 HVC 向上调用固件（HVC 从 EL2
         * 执行会触发 EC=0x16 回绕到自身异常向量）。
         * 改用 SMC 走 EL3 PSCI：SMCCC 32-bit PSCI SYSTEM_OFF = 0x84000008。
         */
        register unsigned long x0 __asm__("x0") = 0x84000008UL;
        __asm__ volatile("smc #0" :: "r"(x0) : "memory");
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
    uart_init();
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
 * Platform panic entry point (for assert.h)
 */
void platform_panic(void)
{
    do_platform_panic();
}

/*
 * Platform shutdown wrapper
 */
void do_platform_shutdown(void)
{
    qemu_shutdown();
}
