/*
 * Boot common definitions
 * Shared between all architectures
 */

#ifndef BOOT_COMMON_H
#define BOOT_COMMON_H

/*
 * C kernel entry point
 * Called from architecture-specific boot code
 */
void kernel_main(void);

/*
 * Platform initialization
 * Must be called before any platform operations
 */
void platform_init(void);

/*
 * Test runner
 * Runs all tests in tests/
 */
void run_all_tests(void);

#endif /* BOOT_COMMON_H */
