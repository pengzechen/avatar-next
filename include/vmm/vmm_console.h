/*
 * include/vmm/vmm_console.h — guest 控制台 vdev 的架构无关入口
 *
 * 每个架构的 guest 控制台是**不同型号的 UART**，但两者对宿主暴露的语义
 * 完全一致（见各自头文件的「输出通路 / 输入通路」两节）：
 *
 *   AArch64 : PL011  @0x09000000, 中断线 33   —— vmm_vpl011.h
 *   RISC-V  : 16550A @0x10000000, PLIC 源 10  —— vmm_uart16550.h
 *   x86_64  : 16550A @0x10000000              —— vmm_uart16550.h
 *
 * 宿主侧只有两处需要这些接口：/dev/vmm（helper 模式搬运字节）和 VMM 主循环
 * （直启模式轮询宿主控制台）。它们不应该各写一份 #if ARCH_*，所以在这里
 * 收成一组转发函数。
 *
 * 不放进 vmm.h 的原因：vmm.h 是三个架构共用的类型定义头，已经要
 * 按架构挑 vGIC 后端；控制台型号再塞进去会让它的条件编译更难读。
 */
#ifndef VMM_CONSOLE_H
#define VMM_CONSOLE_H

#include "arch.h"
#include "klog.h"
#include "vmm_mmio.h"
#include "vmm/vmm.h"   /* vm_t：控制台状态现在是每 VM 一份 */

#if ARCH_AARCH64
#include "vmm/vmm_vpl011.h"
#else
#include "vmm/vmm_uart16550.h"
#endif

/* 控制台在 guest DTB 里的中断源编号（VPL011_IRQ / UART16550_IRQ）*/
#if ARCH_AARCH64
#define VMM_CONSOLE_IRQ   VPL011_IRQ
#else
#define VMM_CONSOLE_IRQ   UART16550_IRQ
#endif

/*
 * vmm_console_init — 初始化虚拟控制台并注册到 MMIO 总线
 * 返回 0 成功。
 */
static inline int vmm_console_init(vm_t *vm, mmio_device_t *dev, mmio_bus_t *bus)
{
#if ARCH_AARCH64
    return vpl011_init(vm, dev, bus);
#else
    (void)vm;   /* uart16550 尚未 per-VM 化（本次只做 aarch64）*/
    return uart16550_init(dev, bus);
#endif
}

/* ── 输出通路（guest → 宿主）───────────────────────────────── */
static inline void vmm_console_tx_set_enabled(vm_t *vm, int enabled)
{
#if ARCH_AARCH64
    vpl011_tx_set_enabled(vm, enabled);
#else
    (void)vm;
    uart16550_tx_set_enabled(enabled);
#endif
}

static inline int vmm_console_tx_channel_enabled(vm_t *vm)
{
#if ARCH_AARCH64
    return vpl011_tx_channel_enabled(vm);
#else
    (void)vm;   /* uart16550 尚未 per-VM 化（本次只做 aarch64）*/
    return uart16550_tx_channel_enabled();
#endif
}

/* 取一个 guest 输出字节；缓冲空时返回 0 */
static inline int vmm_console_tx_pop(vm_t *vm, uint8_t *c)
{
#if ARCH_AARCH64
    return vpl011_tx_pop(vm, c);
#else
    (void)vm;   /* uart16550 尚未 per-VM 化（本次只做 aarch64）*/
    return uart16550_tx_pop(c);
#endif
}

static inline int vmm_console_tx_has_data(vm_t *vm)
{
#if ARCH_AARCH64
    return vpl011_tx_has_data(vm);
#else
    (void)vm;   /* uart16550 尚未 per-VM 化（本次只做 aarch64）*/
    return uart16550_tx_has_data();
#endif
}

/*
 * vmm_console_putchar — VMM 自己往 guest 控制台写一个字节
 *
 * 与 guest 写 THR/UARTDR 走同一条通路，所以 helper 模式下同样进 TX 缓冲。
 * 目前只有 RISC-V 的 SBI console_putchar 用它（aarch64 没有 SBI 控制台，
 * 退回宿主日志即可）。
 */
static inline void vmm_console_putchar(uint8_t c)
{
#if ARCH_AARCH64
    klog_write((const char *)&c, 1);
#else
    uart16550_putchar(c);
#endif
}

/* ── 输入通路（宿主 → guest）───────────────────────────────── */
static inline void vmm_console_push_rx(vm_t *vm, uint8_t c)
{
#if ARCH_AARCH64
    vpl011_push_rx(vm, c);
#else
    (void)vm;
    uart16550_push_rx(c);
#endif
}

/*
 * 控制台中断线是否应保持有效（电平触发）。
 * 调用方在每次进入 guest 前调用，为真时把 VMM_CONSOLE_IRQ 置 pending。
 */
static inline int vmm_console_irq_asserted(vm_t *vm)
{
#if ARCH_AARCH64
    return vpl011_rx_irq_asserted(vm);
#else
    (void)vm;   /* uart16550 尚未 per-VM 化（本次只做 aarch64）*/
    return uart16550_irq_asserted();
#endif
}

/* 丢弃未被 guest 取走的输入字节（停在 guest 时用）*/
static inline void vmm_console_rx_flush(vm_t *vm)
{
#if ARCH_AARCH64
    vpl011_rx_flush(vm);
#else
    (void)vm;
    uart16550_rx_flush();
#endif
}

#endif /* VMM_CONSOLE_H */
