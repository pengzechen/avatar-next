/*
 * kernel/vmm/vmm_console.c — 直启模式下的宿主控制台轮询泵
 *
 * 从 vmm.c 拆出来的。只有直启模式（GUEST_LINUX=1，没有宿主 shell）需要它：
 * 此时没有别的消费者，由 VMM 自己在每次退出 guest 后把宿主 UART 的 RX 字节
 * 推进 guest 控制台 vdev。helper 模式下宿主 tty 层独占真实 UART，本函数会
 * 在 tx_channel 归属检查处直接让位（见函数内的注释）。
 *
 * 原型在 include/vmm/vmm_console.h —— 那个头自己的文档就写明宿主侧只有两处
 * 需要控制台接口：/dev/vmm 和本文件的泵。
 */

#include "vmm/vmm_console.h"
#include "uart/uart.h" /* uart_rx_ready / uart_getc */

#if VMM_GUEST_LINUX_SUPPORTED
/*
 * vmm_console_pump — 宿主控制台 → guest 虚拟 UART 的输入桥
 *
 * 轮询宿主真实 UART 的 RX FIFO，把用户按键推进 guest 控制台 vdev 的
 * RX FIFO（对标 kvmm 的 RxChannel::push；注入中断由 VMM 在进入 guest
 * 前做，见各架构的 vmm_arch_restore_guest_ctx）。
 *
 * 为什么轮询而不是让宿主收 RX 中断：guest 运行期间宿主中断是关的，
 * 自己的 RX 中断根本进不来。而退出路径的调用频率足够高 ——
 *   - guest 空闲：每条 WFI 都陷入 hypervisor，实测 ~3 万次/秒；
 *   - guest 满载：宿主定时器每 10ms 也会把它踹回宿主一次。
 * 对交互式控制台来说，最坏 10ms 的输入延迟完全够用。
 *
 * uart_rx_ready() 在 UART 未开中断时直接查硬件 FIFO 状态位，非阻塞，
 * 所以这里必须先用它把关 —— uart_getc() 在没有数据时是阻塞的。
 *
 * ⚠️ 只在控制台 TX 通道**关闭**时才能跑。helper 模式（/bin/vmm-run）
 * 下宿主 tty 层独占真实 UART：用户按键由 helper 从自己的 stdin 读走，
 * 再 write() 到 /dev/vmm。此时若本函数也在跑，两个消费者会从同一个硬件
 * FIFO 抢字节（tty.c 的 signal_check_uart() 是第一个，本函数是第二个），
 * 谁先跑谁拿到，输入会随机丢给错误的一方。
 */
void vmm_console_pump(vm_t *vm)
{
    /*
     * 归属在别的 VM 手上就让位 —— 从前这是一个全局的 tx_channel 开关，
     * 多 VM 之后每个 VM 各有自己的 console_owned：只有"没有前台 VM"时才
     * 需要 VMM 自己去泵主机控制台，否则宿主 tty 层（helper）独占真实 UART。
     */
    if (vmm_console_tx_channel_enabled(vm))
        return;

    while (uart_rx_ready())
        vmm_console_push_rx(vm, (uint8_t)uart_getc());
}
#endif /* VMM_GUEST_LINUX_SUPPORTED */
