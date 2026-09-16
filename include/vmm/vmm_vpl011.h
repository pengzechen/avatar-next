/*
 * include/vmm_vpl011.h — 虚拟 PL011 UART 设备（guest 控制台）
 *
 * 移植自 x-kernel: virt/vdev/vpl011/src/lib.rs
 *
 * 为 guest 模拟一个 PL011 UART：
 *   - guest 写 UARTDR → 输出到宿主内核日志（行缓冲）
 *   - guest 读 UARTDR → 从 RX FIFO 取字节（当前无输入源，始终为空）
 *   - UARTFR/UARTCR/UARTIMSC/UARTRIS/UARTMIS/ID 寄存器按 PL011 语义返回
 *
 * MMIO 基址与 QEMU virt 的真实 PL011 一致（0x09000000）；因此在启用
 * Stage-2 隔离时，须将该设备及其它设备所在区间映射为「无效」，
 * guest 访问才会陷入 VMM 并被本设备接管（见 stage2 的 trap 模式）。
 */
#ifndef VMM_VPL011_H
#define VMM_VPL011_H

#include "vmm_mmio.h"

/* QEMU virt：PL011 基址与大小 */
#define VPL011_BASE   0x09000000ULL
#define VPL011_SIZE   0x1000ULL

/* RX 中断线（QEMU virt：PL011 = SPI 1 = IRQ 33）*/
#define VPL011_IRQ    33

/*
 * vpl011_init — 初始化虚拟 PL011 并注册到 MMIO 总线
 * @dev:  调用方提供的设备实例（静态存储）
 * @bus:  目标 MMIO 总线
 * 返回 0 成功。
 */
int vpl011_init(mmio_device_t *dev, mmio_bus_t *bus);

#endif /* VMM_VPL011_H */
