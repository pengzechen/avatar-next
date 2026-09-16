/*
 * include/vmm_uart16550.h — 虚拟 16550A UART 设备（riscv64 guest 控制台）
 *
 * 移植自 x-kernel: virt/vdev/uart16550/src/lib.rs
 *
 * QEMU virt 上 RISC-V guest 的串口是 16550A（不是 PL011），基址 0x10000000。
 * 为 guest 模拟该 UART：
 *   - guest 写 THR → 输出到宿主内核日志（行缓冲）
 *   - guest 读 RBR → 从 RX FIFO 取字节（当前无输入源，恒空）
 *   - IER/LCR/MCR/SCR/DLL/DLM 按 16550 语义保存；LSR/IIR 返回就绪状态
 *
 * 中断（PLIC source 10）暂未接线：无 vPLIC，UART 处于轮询模式。
 * 启用 Stage-2 隔离时须把该设备区间映射为「无效」以触发陷入。
 */
#ifndef VMM_UART16550_H
#define VMM_UART16550_H

#include "vmm_mmio.h"

/* QEMU virt：16550A 基址与大小 */
#define UART16550_BASE   0x10000000ULL
#define UART16550_SIZE   0x100ULL

/* guest PLIC 中断源（对应 guest DTB uart@10000000 的 interrupts = <10>）*/
#define UART16550_IRQ    10

/*
 * uart16550_init — 初始化虚拟 16550A 并注册到 MMIO 总线
 * 返回 0 成功。
 */
int uart16550_init(mmio_device_t *dev, mmio_bus_t *bus);

#endif /* VMM_UART16550_H */
