/*
 * include/vmm_vpl011.h — 虚拟 PL011 UART 设备（guest 控制台）
 *
 * 移植自 x-kernel: virt/vdev/vpl011/src/lib.rs
 *
 * 为 guest 模拟一个 PL011 UART：
 *   - guest 写 UARTDR → 见下方「输出通路」两种模式
 *   - guest 读 UARTDR → 从 RX FIFO 弹出字节
 *   - UARTFR/UARTCR/UARTIMSC/UARTRIS/UARTMIS/ID 寄存器按 PL011 语义返回
 *
 * 输出通路（对标 kvmm 的 TxChannel，两种模式由**宿主**决定）：
 *   - 通道关闭（默认）：逐字节直打到宿主控制台。这是 RUN_GUEST_LINUX
 *     直启模式用的 —— guest 独占终端，没有宿主 shell 与它抢。
 *   - 通道打开：字节进 TX 环形缓冲，由 vpl011_tx_pop() 取走交给用户态
 *     helper（/bin/vmm-run）再写到自己 stdout。宿主 tty 层保持对真实
 *     UART 的独占，VMM 不再碰硬件（见 vmm_console_pump 的判据）。
 *
 * 输入通路（对标 kvmm 的 RxChannel）：
 *   - 通道关闭：vmm_console_pump() 轮询真实 PL011 → vpl011_push_rx()
 *   - 通道打开：用户态 helper 把 stdin 字节 write() 到 /dev/vmm
 *   两条路都汇入同一个 RX FIFO；vpl011_rx_irq_asserted()（FIFO 非空且
 *   UARTIMSC 的 RX 位开着）为真时，调用方把 VPL011_IRQ 经 vGIC 置
 *   pending，guest 收中断后取走数据。
 *   RX 中断是**电平触发**语义：只要 FIFO 非空且 IMSC 里 RX 位开着，
 *   每次进入 guest 前都要重新置 pending（guest 应答时 vGIC 会清掉该位）。
 *
 * MMIO 基址与 QEMU virt 的真实 PL011 一致（0x09000000）；因此在启用
 * Stage-2 隔离时，须将该设备及其它设备所在区间映射为「无效」，
 * guest 访问才会陷入 VMM 并被本设备接管（见 stage2 的 trap 模式）。
 */
#ifndef VMM_VPL011_H
#define VMM_VPL011_H

#include "vmm_mmio.h"
#include "spinlock.h"

/* QEMU virt：PL011 基址与大小 */
#define VPL011_BASE   0x09000000ULL
#define VPL011_SIZE   0x1000ULL

/* ── 设备私有状态 ─────────────────────────────────────────────
 *
 * ⚠️ 从前这是一个**文件级 static**（`static vpl011_state_t g_vpl011;`），
 * 于是整个内核只有一份控制台状态 —— 第二个 VM 的 vpl011_init() 里那句
 * memset 会把第一个 VM 的 RX/TX FIFO 清空，两个 VM 从此抢同一个控制台。
 * 现在它嵌在 vm_t 里，每个 VM 一份。
 *
 * 并发：两个环都是跨任务的 ——
 *   - guest MMIO 读写发生在 vCPU 任务里（VMM 退出路径分发）；
 *   - 宿主侧 push_rx / tx_pop 在 /dev/vmm 的 read/write 里，属于 helper 任务。
 * 所以都要用 IRQ-safe 的锁（锁也跟着进 vm_t，见 vm_t 的 vpl011_lock）。
 */
#define VPL011_RX_FIFO_SIZE 256
#define VPL011_TX_FIFO_SIZE 8192

struct vm;

typedef struct vpl011_state {
    /*
     * 回指所属的 VM —— MMIO ops 只拿得到 dev->priv（指向本结构），
     * 而它要访问 vm->console_owned（控制台归属）等 VM 级字段。
     */
    struct vm *owner;

    uint32_t cr;                    /* UARTCR */
    uint32_t imsc;                  /* UARTIMSC */

    /* RX 环形缓冲：宿主用户态写入（push），guest MMIO 读取（UARTDR）*/
    uint8_t  rx_fifo[VPL011_RX_FIFO_SIZE];
    uint32_t rx_head;               /* 写入位置 */
    uint32_t rx_tail;               /* 读出位置 */
    uint32_t rx_count;

    /* TX 环形缓冲：guest MMIO 写入（UARTDR），宿主用户态读取（tx_pop）*/
    uint8_t  tx_fifo[VPL011_TX_FIFO_SIZE];
    uint32_t tx_head;
    uint32_t tx_tail;
    uint32_t tx_count;

    uint64_t rx_dropped;            /* FIFO 满而丢弃的字节数 */
    uint64_t tx_dropped;
} vpl011_state_t;

/* RX 中断线（QEMU virt：PL011 = SPI 1 = IRQ 33）*/
#define VPL011_IRQ    33

/*
 * vpl011_init — 初始化虚拟 PL011 并注册到 MMIO 总线
 * @dev:  调用方提供的设备实例（静态存储）
 * @bus:  目标 MMIO 总线
 * 返回 0 成功。
 */
struct vm;
typedef struct vm vm_t;
int vpl011_init(vm_t *vm, mmio_device_t *dev, mmio_bus_t *bus);

/*
 * vpl011_push_rx — 把宿主控制台收到的一个字节喂给 guest
 * @c: 字符。FIFO 满时丢弃（只计数，不打印：调用点可能持有宿主日志锁）。
 */
void vpl011_push_rx(vm_t *vm, uint8_t c);

/*
 * vpl011_rx_irq_asserted — RX 中断线是否应保持有效
 * 返回 1 表示 RX FIFO 非空 **且** guest 已在 UARTIMSC 里打开 RX 中断。
 *
 * PL011 的 RX 中断是电平触发：调用方应在每次进入 guest 前调用本函数，
 * 为真时把 VPL011_IRQ 置 pending。只在 push 时置一次是不够的 —— guest
 * 应答中断时 vGIC 会清掉 pending 位，FIFO 里剩余的字节会因此没人再取。
 */
int vpl011_rx_irq_asserted(vm_t *vm);

/*
 * vpl011_rx_flush — 丢弃 RX FIFO 里所有未被 guest 取走的字节
 *
 * 停在 guest 时用：不清的话，上一轮没消费的按键会在下一次启动时先喂给新
 * guest 的 getty。
 */
void vpl011_rx_flush(vm_t *vm);

/*
 * ── TX 通道（guest 输出 → 用户态 helper）─────────────────────────
 *
 * vpl011_tx_set_enabled — 切换输出目标
 * @enabled: 1 = 字节进 TX 环形缓冲供 vpl011_tx_pop() 取走；
 *           0 = 逐字节直打宿主控制台（直启模式）。
 *
 * 打开通道同时意味着「宿主 tty 独占真实 UART」，因此 vmm_console_pump()
 * 的调用方必须用 vpl011_tx_channel_enabled() 把关，否则用户态 helper 与
 * VMM 会同时从硬件 FIFO 抢字节。
 */
void vpl011_tx_set_enabled(vm_t *vm, int enabled);
int  vpl011_tx_channel_enabled(vm_t *vm);

/*
 * vpl011_tx_pop — 取一个 guest 输出字节
 * @c: 输出参数。缓冲空时返回 0（不修改 *c）。
 */
int  vpl011_tx_pop(vm_t *vm, uint8_t *c);

/* TX 缓冲里是否还有数据（helper 的 poll 用它报 EPOLLIN）*/
int  vpl011_tx_has_data(vm_t *vm);

#endif /* VMM_VPL011_H */
