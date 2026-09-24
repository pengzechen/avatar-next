/*
 * include/vmm_vpl011.h — 虚拟 PL011 UART 设备（guest 控制台）
 *
 * 移植自 x-kernel: virt/vdev/vpl011/src/lib.rs
 *
 * 为 guest 模拟一个 PL011 UART：
 *   - guest 写 UARTDR → 立即打到宿主控制台（逐字节直通，见 vpl011.c）
 *   - guest 读 UARTDR → 从 RX FIFO 弹出字节
 *   - UARTFR/UARTCR/UARTIMSC/UARTRIS/UARTMIS/ID 寄存器按 PL011 语义返回
 *
 * 输入通路（对标 kvmm 的 RxChannel）：
 *   宿主控制台按键 → vmm_console_pump() 轮询真实 PL011
 *                  → vpl011_push_rx() 压入 RX FIFO
 *                  → vpl011_rx_irq_asserted() 为真时，调用方把 VPL011_IRQ
 *                    经 vGIC 置 pending，guest 收到 RX 中断后取走数据。
 *   RX 中断是**电平触发**语义：只要 FIFO 非空且 UARTIMSC 里 RX 位开着，
 *   每次进入 guest 前都要重新置 pending（guest 应答时 vGIC 会清掉该位）。
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

/*
 * vpl011_push_rx — 把宿主控制台收到的一个字节喂给 guest
 * @c: 字符。FIFO 满时丢弃（只计数，不打印：调用点可能持有宿主日志锁）。
 */
void vpl011_push_rx(uint8_t c);

/*
 * vpl011_rx_irq_asserted — RX 中断线是否应保持有效
 * 返回 1 表示 RX FIFO 非空 **且** guest 已在 UARTIMSC 里打开 RX 中断。
 *
 * PL011 的 RX 中断是电平触发：调用方应在每次进入 guest 前调用本函数，
 * 为真时把 VPL011_IRQ 置 pending。只在 push 时置一次是不够的 —— guest
 * 应答中断时 vGIC 会清掉 pending 位，FIFO 里剩余的字节会因此没人再取。
 */
int vpl011_rx_irq_asserted(void);

#endif /* VMM_VPL011_H */
