/*
 * Platform abstraction layer
 * Allows support for multiple platforms (QEMU, real hardware, etc.)
 */

#ifndef PLATFORM_H
#define PLATFORM_H

/*
 * Platform operations structure
 * Each platform implements these functions
 */
struct platform_ops {
    /*
     * UART/Serial output
     */
    void (*uart_putc)(char c);
    void (*uart_puts)(const char *s);

    /*
     * Panic handler
     * Never returns
     */
    void (*panic)(void) __attribute__((noreturn));

    /*
     * Shutdown / power-off
     * Never returns
     */
    void (*shutdown)(void) __attribute__((noreturn));
};

/*
 * Get current platform operations
 * Returns pointer to platform-specific operations
 */
struct platform_ops *platform_get_ops(void);

/*
 * Initialize platform
 * Sets up UART and other platform-specific hardware
 */
void platform_init(void);

/*
 * Convenience wrappers
 */
static inline void platform_putc(char c)
{
    platform_get_ops()->uart_putc(c);
}

static inline void platform_puts(const char *s)
{
    platform_get_ops()->uart_puts(s);
}

/*
 * Platform panic - never returns
 */
void do_platform_panic(void) __attribute__((noreturn));

/*
 * Platform shutdown / power-off - never returns
 */
void do_platform_shutdown(void) __attribute__((noreturn));

#endif /* PLATFORM_H */
