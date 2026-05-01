/*
 * Kernel main entry point
 * Called from architecture-specific boot code
 */

#include "../boot/common/boot.h"
#include "../boot/common/platform.h"
#include "arch.h"
#include "klog.h"
#include "string.h"

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

    /* Run all tests */
    KLOG_INFO("Running tests...");
    run_all_tests();

    /* All tests completed */
    KLOG_INFO("All tests completed successfully!");

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
}
