/* platforms/rk3588-aarch64/platform.c
 *
 * Rockchip RK3588 平台初始化
 * 复用 QEMU 共享 platform.c 中的 uart_putchar/puts/panic/shutdown
 * 实际 RK3588 上的 shutdown 走 PSCI SYSTEM_OFF (SMC #0 x0=0x84000008)，
 * 与 qemu/platform.c 里的 AArch64 实现路径完全相同。
 */
#include "../qemu/platform.c"
