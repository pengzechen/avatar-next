/*
 * include/vmm/vmm_uart16550.h — 虚拟 16550A UART 设备（riscv64/x86_64 guest 控制台）
 *
 * 移植自 x-kernel: virt/vdev/uart16550/src/lib.rs
 *
 * QEMU virt 上 RISC-V guest 的串口是 16550A（不是 PL011），基址 0x10000000。
 * 为 guest 模拟该 UART：
 *   - guest 写 THR → 见下方「输出通路」两种模式
 *   - guest 读 RBR → 从 RX FIFO 弹出字节
 *   - IER/LCR/MCR/SCR/DLL/DLM 按 16550 语义保存
 *   - LSR 恒报 THRE|TEMT（发送即时完成），有 RX 数据时加 DR
 *   - IIR 按 16550 优先级报「RX 有数据」>「THR 空」>「无中断」
 *
 * 输出通路（对标 kvmm 的 TxChannel，与 vpl011 完全一致，两种模式由宿主决定）：
 *   - 通道关闭（默认）：逐字节直打宿主控制台。RUN_GUEST_LINUX 直启模式用 ——
 *     guest 独占终端，没有宿主 shell 与它抢。
 *   - 通道打开：字节进 TX 环形缓冲，由 uart16550_tx_pop() 取走交给用户态
 *     helper（/bin/vmm-run）再写到自己 stdout。宿主 tty 层保持对真实 UART
 *     的独占，VMM 不再碰硬件（见 vmm_console_pump 的判据）。
 *
 * 输入通路（对标 kvmm 的 RxChannel）：
 *   - 通道关闭：vmm_console_pump() 轮询真实 UART → uart16550_push_rx()
 *   - 通道打开：用户态 helper 把 stdin 字节 write() 到 /dev/vmm
 *   两条路都汇入同一个 RX FIFO；uart16550_irq_asserted() 为真时，调用方把
 *   UART16550_IRQ 经 vPLIC 置 pending，guest 收中断后取走数据。
 *   RX 是**电平触发**语义：只要条件成立，每次进入 guest 前都要重新置 pending
 *   （guest claim 时 vPLIC 会清掉 pending 位，FIFO 里剩的字节否则没人再取）。
 *
 * ⚠️ THR 空中断（IIR=0x02）是必需的，不是可选的锦上添花：
 * Linux 的 8250 驱动只有在收到 THR 空中断时才会调 serial8250_tx_chars()
 * 把下一批字符写进 THR（它并不靠写 IER 之后直接推）。没有这条中断，
 * 输出会在首批字符之后**静默停住**，且没有任何报错。
 *
 * MMIO 基址与 QEMU virt 的真实 UART 一致（0x10000000）；启用 G-stage 隔离
 * 时必须把该区间映射为「无效」，guest 访问才会陷入 VMM 并被本设备接管。
 */
#ifndef VMM_UART16550_H
#define VMM_UART16550_H

#include "vmm_mmio.h"
#include "spinlock.h"

/* QEMU virt：16550A 基址与大小 */
#define UART16550_BASE   0x10000000ULL
#define UART16550_SIZE   0x100ULL

/* guest PLIC 中断源（对应 guest DTB uart@10000000 的 interrupts = <10>）*/
#define UART16550_IRQ    10

/* 缓冲深度：TX 要能扛住 guest 启动那一大串内核日志的突发 */
#define UART16550_RX_FIFO_SIZE  4096
#define UART16550_TX_FIFO_SIZE  8192

/* ── 设备私有状态 ─────────────────────────────────────────────
 *
 * ⚠️ 从前这是一个**文件级 static**（`static uart16550_state_t g_uart16550;`），
 * 于是整个内核只有一份控制台状态 —— 第二个 VM 的 uart16550_init() 里那句
 * memset 会把第一个 VM 的 RX/TX FIFO 清空，两个 VM 从此抢同一个控制台。
 * 现在它嵌在 vm_t 里，每个 VM 一份（与 aarch64 的 vpl011_state_t 对称）。
 *
 * 并发：两个环都是跨任务的 ——
 *   - guest MMIO/PIO 读写发生在 vCPU 任务里（VMM 退出路径分发）；
 *   - 宿主侧 push_rx / tx_pop 在 /dev/vmm 的 read/write 里，属于 helper 任务。
 * 所以都要用 IRQ-safe 的锁（锁也跟着进 vm_t，见 vm_t 的 uart16550_lock）。
 */
typedef struct uart16550_state {
    /*
     * 回指所属的 VM —— MMIO ops 只拿得到 dev->priv（指向本结构），而它要
     * 访问 vm->console_owned（控制台归属）等 VM 级字段。
     */
    struct vm *owner;

    uint8_t  ier;
    uint8_t  lcr;
    uint8_t  mcr;
    uint8_t  scr;
    uint8_t  fcr;
    uint8_t  dll;
    uint8_t  dlm;

    /* RX 环形缓冲：宿主/helper 写入，guest 读 RBR 弹出 */
    uint8_t  rx_fifo[UART16550_RX_FIFO_SIZE];
    uint32_t rx_head;
    uint32_t rx_tail;
    uint32_t rx_count;
    uint32_t rx_drops;

    /* TX 环形缓冲：guest 写 THR 推入，宿主用户态 helper 读走 */
    uint8_t  tx_fifo[UART16550_TX_FIFO_SIZE];
    uint32_t tx_head;
    uint32_t tx_tail;
    uint32_t tx_count;
} uart16550_state_t;

/*
 * 输出目标（走 TX 环还是直打宿主控制台）**不在**本结构里，而是
 * vm->console_owned —— 那是宿主侧的运行模式选择，不是设备寄存器状态。
 * 从前它叫 state.tx_channel，于是 uart16550_init() 的 memset 必须特意把它
 * 存下来再恢复（GUEST_CONSOLE.md §2 记着这个陷阱）。挪进 vm_t 之后
 * memset 可以整片清零，陷阱自然消失。与 aarch64 的 vpl011 处理一致。
 */

struct vm;
typedef struct vm vm_t;

/*
 * uart16550_init — 初始化虚拟 16550A 并注册到 MMIO 总线
 *
 * @vm:  所属 VM（状态按 vm->slot 取自 vuart16550.c 的静态池）
 * @bus: 目标 MMIO 总线
 *
 * 设备对象也在那个池里，不再由调用方传进来（与 vpl011_init 同形）。
 * 返回 0 成功。
 */
int uart16550_init(vm_t *vm, mmio_bus_t *bus);

/* 归还本 VM 的槽位（由 vmm_arch_vm_destroy → vm_free 调用）。幂等。*/
void uart16550_destroy(vm_t *vm);

/* 取本 VM 的设备对象（x86 的 PIO 路径不走 MMIO 总线，需要它）。*/
mmio_device_t *uart16550_dev(vm_t *vm);

/*
 * uart16550_push_rx — 把宿主控制台收到的一个字节喂给 guest
 * @c: 字符。FIFO 满时丢弃（只计数，不打印：调用点可能持有宿主日志锁）。
 */
void uart16550_push_rx(vm_t *vm, uint8_t c);

/*
 * uart16550_irq_asserted — 中断线是否应保持有效
 * 返回 1 表示 (RX FIFO 非空 && IER.RX 开着) 或 (IER.THR 开着)。
 *
 * 16550 的中断是电平触发：调用方应在每次进入 guest 前调用本函数，
 * 为真时把 UART16550_IRQ 置 pending。只在 push 时置一次是不够的 ——
 * guest 应答中断时 vPLIC 会清掉 pending 位。
 */
int uart16550_irq_asserted(vm_t *vm);

/*
 * uart16550_rx_flush — 丢弃 RX FIFO 里所有未被 guest 取走的字节
 *
 * 停在 guest 时用：不清的话，上一轮没消费的按键会在下一次启动时先喂给新
 * guest 的 getty。
 */
void uart16550_rx_flush(vm_t *vm);

/*
 * ── TX 通道（guest 输出 → 用户态 helper）─────────────────────────
 *
 * uart16550_tx_set_enabled — 切换输出目标
 * @enabled: 1 = 字节进 TX 环形缓冲供 uart16550_tx_pop() 取走；
 *           0 = 逐字节直打宿主控制台（直启模式）。
 *
 * 打开通道同时意味着「宿主 tty 独占真实 UART」，因此 vmm_console_pump()
 * 的调用方必须用 uart16550_tx_channel_enabled() 把关，否则用户态 helper 与
 * VMM 会同时从硬件 FIFO 抢字节。
 */
void uart16550_tx_set_enabled(vm_t *vm, int enabled);
int  uart16550_tx_channel_enabled(vm_t *vm);

/*
 * uart16550_tx_pop — 取一个 guest 输出字节
 * @c: 输出参数。缓冲空时返回 0（不修改 *c）。
 */
int  uart16550_tx_pop(vm_t *vm, uint8_t *c);

/* TX 缓冲里是否还有数据（helper 的 poll 用它报 EPOLLIN）*/
int  uart16550_tx_has_data(vm_t *vm);

/*
 * uart16550_putchar — 从 VMM 自己往 guest 控制台送一个字节
 * 走与 guest 写 THR 完全相同的输出通路（见文件头的「输出通路」）。
 * 目前唯一调用者是 RISC-V 的 SBI console_putchar。
 */
void uart16550_putchar(vm_t *vm, uint8_t c);

/*
 * 端口 I/O 包装（x86 COM1 走 0x3F8 端口，不走 MMIO）。
 * off = 端口号 - UART16550 基址，访问宽度固定 1 字节。
 * 复用同一份寄存器状态机，见 vuart16550.c 的说明。
 */
uint64_t uart16550_port_read(mmio_device_t *dev, uint64_t off);
void     uart16550_port_write(mmio_device_t *dev, uint64_t off, uint8_t value);

#endif /* VMM_UART16550_H */
