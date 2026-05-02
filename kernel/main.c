/*
 * Kernel main entry point
 * Called from architecture-specific boot code
 */

#include "../boot/common/boot.h"
#include "../boot/common/platform.h"
#include "arch.h"
#include "klog.h"
#include "string.h"

#if ARCH_AARCH64
#include "irq/irq.h"
#include "timer/timer.h"
#elif ARCH_RISCV64
#include "exception.h"
#include "timer/timer.h"
#elif ARCH_X86_64
#include "exception.h"
#include "timer/timer.h"
#endif

/* Forward declarations for test functions */
extern void test_arch(void);
extern void run_klog_tests(void);
extern void test_string_functions(void);
extern void run_assert_tests(void);

void kernel_main(void)
{
    /* Initialize platform (UART, etc.) */
    platform_init();

    /* Print welcome message */
    KLOG_INFO("=== Avatar OS Kernel ===");
    KLOG_INFO("Architecture: "
#if ARCH_AARCH64
        "AArch64 (ARM 64-bit)"
#elif ARCH_X86_64
        "x86_64 (AMD64/Intel 64)"
#elif ARCH_RISCV64
        "RISC-V 64-bit"
#else
        "Unknown"
#endif
    );

#if ARCH_AARCH64

    /* Initialize GIC (interrupt controller) */
    KLOG_INFO("Initializing GICv2 interrupt controller...");
    irq_init();
    KLOG_INFO("GICv2 initialized");

    /* Initialize timer */
    KLOG_INFO("Initializing timer...");
    timer_init();
    timer_enable();
    KLOG_INFO("Timer enabled");

#elif ARCH_RISCV64

    /* Initialize exception handler (sets stvec, enables sstatus.SIE) */
    KLOG_INFO("Initializing exception handler...");
    exception_init();

    /* Initialize timer (also registers timer_handler via irq_install) */
    KLOG_INFO("Initializing timer...");
    timer_init();
    timer_enable();
    KLOG_INFO("Timer enabled");

#elif ARCH_X86_64

    /* Initialize IDT and LAPIC */
    KLOG_INFO("Initializing IDT + LAPIC...");
    exception_init();

    /* Initialize timer (registers handler, calibrates LAPIC frequency) */
    KLOG_INFO("Initializing timer...");
    timer_init();
    timer_enable();
    KLOG_INFO("Timer enabled");

#endif

    /* Run all tests */
    KLOG_INFO("Running tests...");
    run_all_tests();

    /* All tests completed */
    KLOG_INFO("All tests completed successfully!");

#if ARCH_AARCH64 || ARCH_RISCV64 || ARCH_X86_64
    /* Monitor timer interrupts - print every second */
    KLOG_INFO("Starting timer monitoring (printing every 1 second)...");
    uint64_t last_report_ticks = 0;

    while(1) {
        uint64_t current_ticks = timer_get_system_ticks();

        /* 每 100 个 tick（1秒）输出一次 */
        if (current_ticks - last_report_ticks >= 100) {
            uint64_t seconds = current_ticks / 100;
            uint64_t milliseconds = (current_ticks % 100) * 10;

            KLOG_INFO("System ticks: %llu (%llu.%03llu seconds)",
                      current_ticks, seconds, milliseconds);
            last_report_ticks = current_ticks;
        }

        /* 使用等待中断指令降低 CPU 负荷 */
#if ARCH_AARCH64
        __asm__ volatile("wfe");
#elif ARCH_X86_64
        __asm__ volatile("hlt");  /* HLT 待中断唯醒 */
#else
        __asm__ volatile("wfi");
#endif
    }
#endif /* ARCH_AARCH64 || ARCH_RISCV64 || ARCH_X86_64 */

    /* Shutdown */
    KLOG_INFO("Kernel shutting down...");
    do_platform_shutdown();
}

void run_all_tests(void)
{
    /* Run architecture test */
    KLOG_INFO("--- Architecture Detection Test ---");
    test_arch();
    KLOG_INFO("");

    /* Run klog test */
    KLOG_INFO("--- Kernel Log Test ---");
    run_klog_tests();
    KLOG_INFO("");

    /* Run string test */
    KLOG_INFO("--- String Test ---");
    test_string_functions();
    KLOG_INFO("");

    /* Run assert test */
    KLOG_INFO("--- Assert Test ---");
    run_assert_tests();
    KLOG_INFO("");

#if 0
    /* Run exception test (undefined instruction) */
    KLOG_INFO("--- Exception Test (Undefined Instruction) ---");
    KLOG_INFO("Attempting to execute an undefined instruction...");

    /* Trigger an undefined instruction exception */
    /* 0x00000000 is not a valid AArch64 instruction */
    __asm__ volatile(
        ".inst 0x00000000\n"  /* Undefined instruction */
    );

    KLOG_INFO("If you see this, exception handling failed!");
#endif
}
