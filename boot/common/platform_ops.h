/*
 * Platform abstraction layer
 * Allows support for multiple platforms (QEMU, real hardware, etc.)
 */

#ifndef BOOT_COMMON_PLATFORM_OPS_H
#define BOOT_COMMON_PLATFORM_OPS_H

/*
 * Initialize platform
 * Sets up UART and other platform-specific hardware
 */
void platform_init(void);

/*
 * Platform panic - never returns
 */
void platform_panic(void) __attribute__((noreturn));

/*
 * Platform shutdown / power-off - never returns
 */
void platform_shutdown(void) __attribute__((noreturn));

#endif /* BOOT_COMMON_PLATFORM_OPS_H */
