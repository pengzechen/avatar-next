/*
 * kernel/vmm/vdev/vuart16550.c — 虚拟 16550A UART 实现（riscv64/x86_64）
 *
 * 移植自 x-kernel: virt/vdev/uart16550/src/lib.rs，适配 Avatar OS：
 *   - klogger::kprint → klog_write（直启模式）/ TX 环形缓冲（helper 模式）
 *   - RxChannel → 本文件内的 RX 环形缓冲 + uart16550_push_rx()
 *   - 中断：由宿主在每次进 guest 前按 uart16550_irq_asserted() 经 vPLIC
 *     置 pending（电平触发语义，见头文件）
 *
 * 与 vpl011.c 是同一个模板的两个实例：TX 双通路、RX 单 FIFO、电平触发中断。
 * 改这里时请顺带看一眼对面，两边语义应当保持一致。
 *
 * ⚠️ 与 vpl011 唯一的有意差异：**不做行缓冲**。
 * vpl011 早期版本把 guest 输出攒到换行才 flush，那会把 guest 的按键回显
 * （无换行的单字符）一直憋到回车 —— 交互式会话直接不可用。这里逐字节直通。
 * 同理，THR 空中断必须实现：Linux 的 8250 驱动靠它推进发送队列，缺了会让
 * 输出在第一批字符之后静默停住（详见头文件）。
 *
 * ── 多 VM：状态与锁都搬进了 vm_t ─────────────────────────────
 *
 * 从前上面那两样是文件级 static（g_uart16550 / g_uart16550_lock），整机只有
 * 一份 —— 第二个 VM 的 init 一句 memset 就把第一个 VM 的控制台清空。现在
 * 每个 vm_t 里各有一份，函数一律以 vm 为第一参数；MMIO 回调拿不到 vm，
 * 就走 dev->priv → state->owner 回指（与 vpl011.c 同一手法）。
 */

#include "vmm/vmm_uart16550.h"
#include "vmm/vmm.h"        /* vm_t：uart16550_state_t 的宿主 */
#include "klog.h"
#include "string.h"
#include "spinlock.h"

/* ── 16550 寄存器偏移 ─────────────────────────────────────── */
#define UART_RBR_THR_DLL   0x00
#define UART_IER_DLM       0x01
#define UART_IIR_FCR       0x02
#define UART_LCR           0x03
#define UART_MCR           0x04
#define UART_LSR           0x05
#define UART_MSR           0x06
#define UART_SCR           0x07

/* IER 位 */
#define IER_RX_AVAILABLE   (1u << 0)
#define IER_THR_EMPTY      (1u << 1)

/* IIR 值（bit0=0 表示有中断挂起，bits3:1 是中断 ID）*/
#define IIR_NO_INTERRUPT   (1u << 0)   /* 0x01 */
#define IIR_THR_EMPTY      0x02
#define IIR_RX_AVAILABLE   0x04

/* IIR bits7:6 = FIFO 状态（FCR 使能 FIFO 后为 0b11）*/
#define IIR_FIFO_ENABLED   0xC0

/* LSR 位 */
#define LSR_DATA_READY     (1u << 0)
#define LSR_THRE           (1u << 5)
#define LSR_TEMT           (1u << 6)

/* LCR 位 7：除数锁存使能（DLAB）*/
#define LCR_DLAB           (1u << 7)

/* FCR 位 0：使能 FIFO */
#define FCR_ENABLE_FIFO    (1u << 0)

/* 缓冲深度在头文件里（vm_t 要按它算大小），这里只取短名 */
#define RX_FIFO_SIZE   UART16550_RX_FIFO_SIZE
#define TX_FIFO_SIZE   UART16550_TX_FIFO_SIZE

/* ── 状态与锁的取用（每个 VM 一份）─────────────────────────── */

/*
 * MMIO 回调只有 dev->priv（→ state），没有 vm —— state->owner 就是回指。
 * 这两个宏把"从 state 拿锁"这件事写在一处，免得每个回调各写一遍强制转换。
 */
#define UART_LOCK(s)   (&(s)->owner->uart16550_lock)

static int rx_irq_asserted_locked(const uart16550_state_t *s)
{
    /* 电平触发：FIFO 非空且 guest 开了 RX 中断，或者 guest 开了 THR 空中断 */
    if (s->rx_count > 0 && (s->ier & IER_RX_AVAILABLE))
        return 1;
    return (s->ier & IER_THR_EMPTY) != 0;
}

/* ── 公开注入接口（宿主 → guest）───────────────────────────── */
void uart16550_push_rx(vm_t *vm, uint8_t c)
{
    uart16550_state_t *s = &vm->uart16550;
    uint64_t flags;

    spin_lock_irqsave(&vm->uart16550_lock, &flags);
    if (s->rx_count >= RX_FIFO_SIZE) {
        s->rx_drops++;
        spin_unlock_irqrestore(&vm->uart16550_lock, flags);
        return;
    }
    s->rx_fifo[s->rx_head] = c;
    s->rx_head = (s->rx_head + 1) % RX_FIFO_SIZE;
    s->rx_count++;
    spin_unlock_irqrestore(&vm->uart16550_lock, flags);
}

int uart16550_irq_asserted(vm_t *vm)
{
    uart16550_state_t *s = &vm->uart16550;
    uint64_t flags;
    int asserted;

    spin_lock_irqsave(&vm->uart16550_lock, &flags);
    asserted = rx_irq_asserted_locked(s);
    spin_unlock_irqrestore(&vm->uart16550_lock, flags);
    return asserted;
}

void uart16550_rx_flush(vm_t *vm)
{
    uart16550_state_t *s = &vm->uart16550;
    uint64_t flags;

    spin_lock_irqsave(&vm->uart16550_lock, &flags);
    s->rx_head  = 0;
    s->rx_tail  = 0;
    s->rx_count = 0;
    s->rx_drops = 0;
    spin_unlock_irqrestore(&vm->uart16550_lock, flags);
}

void uart16550_tx_set_enabled(vm_t *vm, int enabled)
{
    uart16550_state_t *s = &vm->uart16550;
    uint64_t flags;

    spin_lock_irqsave(&vm->uart16550_lock, &flags);
    vm->console_owned = enabled ? 1 : 0;
    if (!enabled) {
        /* 关通道时清空 TX 环：剩下的字节是上一个 guest 的，别再放给下一个 */
        s->tx_head  = 0;
        s->tx_tail  = 0;
        s->tx_count = 0;
    }
    spin_unlock_irqrestore(&vm->uart16550_lock, flags);
}

int uart16550_tx_channel_enabled(vm_t *vm)
{
    uart16550_state_t *s = &vm->uart16550;
    uint64_t flags;
    int enabled;

    spin_lock_irqsave(&vm->uart16550_lock, &flags);
    enabled = vm->console_owned;
    spin_unlock_irqrestore(&vm->uart16550_lock, flags);
    return enabled;
}

int uart16550_tx_pop(vm_t *vm, uint8_t *c)
{
    uart16550_state_t *s = &vm->uart16550;
    uint64_t flags;
    int got = 0;

    spin_lock_irqsave(&vm->uart16550_lock, &flags);
    if (s->tx_count > 0) {
        *c = s->tx_fifo[s->tx_tail];
        s->tx_tail = (s->tx_tail + 1) % TX_FIFO_SIZE;
        s->tx_count--;
        got = 1;
    }
    spin_unlock_irqrestore(&vm->uart16550_lock, flags);
    return got;
}

int uart16550_tx_has_data(vm_t *vm)
{
    uart16550_state_t *s = &vm->uart16550;
    uint64_t flags;
    int has;

    spin_lock_irqsave(&vm->uart16550_lock, &flags);
    has = s->tx_count > 0;
    spin_unlock_irqrestore(&vm->uart16550_lock, flags);
    return has;
}

/*
 * uart_put_char — guest 写 THR 的一个字节（**调用方必须已持锁**）
 *
 * 直启模式：立即走 klog 的唯一入口。宿主 uart_putchar 会把 '\n' 补成
 * "\r\n"，所以 guest 的裸 '\n'（earlycon）与 tty 已转好的 "\r\n" 都能正确换行。
 *
 * 注意本函数**在持锁状态下**调 klog_write：klog 自己也有锁，且 uart_putchar
 * 在缓冲满时会等待。这把锁的中断已关，因此不会有锁序反转；与 vpl011 的处理
 * 一致。
 */
static void uart_put_char(uart16550_state_t *s, uint8_t c)
{
    if (!s->owner->console_owned) {
        /* 逐字符走唯一入口：guest 控制台本来就是字节流，粒度只能是字节，
         * 但至少每次输出都真的经过那把锁，不和别的核各写各的。*/
        klog_write((const char *)&c, 1);
        return;
    }

    if (s->tx_count >= TX_FIFO_SIZE) {
        /* 缓冲满：丢尾。helper 读得比 guest 写得慢时才会发生 */
        s->tx_tail = (s->tx_tail + 1) % TX_FIFO_SIZE;
        s->tx_count--;
    }
    s->tx_fifo[s->tx_head] = c;
    s->tx_head = (s->tx_head + 1) % TX_FIFO_SIZE;
    s->tx_count++;
}

/* ── MMIO 读写回调 ────────────────────────────────────────── */
static uint64_t uart16550_read(mmio_device_t *dev, uint64_t off, uint8_t size)
{
    uart16550_state_t *s = (uart16550_state_t *)dev->priv;
    uint64_t flags;
    uint64_t ret = 0;
    int dlab;

    if (size != 1 && size != 4)
        return 0;

    spin_lock_irqsave(UART_LOCK(s), &flags);
    dlab = (s->lcr & LCR_DLAB) != 0;

    switch (off) {
    case UART_RBR_THR_DLL:
        if (dlab) {
            ret = s->dll;
        } else if (s->rx_count > 0) {
            ret = s->rx_fifo[s->rx_tail];
            s->rx_tail = (s->rx_tail + 1) % RX_FIFO_SIZE;
            s->rx_count--;
        }
        break;

    case UART_IER_DLM:
        ret = dlab ? s->dlm : s->ier;
        break;

    case UART_IIR_FCR:
        /*
         * 中断识别：bit0=0 表示有中断挂起。RX 优先于 THR 空
         * （与真实 16550 的优先级一致）。
         */
        if (s->rx_count > 0 && (s->ier & IER_RX_AVAILABLE))
            ret = IIR_RX_AVAILABLE;
        else if (s->ier & IER_THR_EMPTY)
            ret = IIR_THR_EMPTY;
        else
            ret = IIR_NO_INTERRUPT;

        if (s->fcr & FCR_ENABLE_FIFO)
            ret |= IIR_FIFO_ENABLED;
        break;

    case UART_LCR: ret = s->lcr; break;
    case UART_MCR: ret = s->mcr; break;

    case UART_LSR:
        /* 发送即时完成 → THRE/TEMT 恒置位；RX 有数据时加 DR */
        ret = LSR_THRE | LSR_TEMT;
        if (s->rx_count > 0)
            ret |= LSR_DATA_READY;
        break;

    case UART_MSR: ret = 0; break;
    case UART_SCR: ret = s->scr; break;
    default:       ret = 0; break;
    }

    spin_unlock_irqrestore(UART_LOCK(s), flags);
    return ret;
}

static void uart16550_write(mmio_device_t *dev, uint64_t off, uint8_t size,
                            uint64_t value)
{
    uart16550_state_t *s = (uart16550_state_t *)dev->priv;
    uint8_t v = (uint8_t)value;
    uint64_t flags;
    int dlab;

    if (size != 1 && size != 4)
        return;

    spin_lock_irqsave(UART_LOCK(s), &flags);
    dlab = (s->lcr & LCR_DLAB) != 0;

    switch (off) {
    case UART_RBR_THR_DLL:
        if (dlab)
            s->dll = v;
        else
            uart_put_char(s, v);   /* THR：输出 guest 字符 */
        break;

    case UART_IER_DLM:
        if (dlab)
            s->dlm = v;
        else
            s->ier = v;
        break;

    case UART_IIR_FCR: s->fcr = v; break;   /* 只记 FIFO 使能位 */
    case UART_LCR:     s->lcr = v; break;
    case UART_MCR:     s->mcr = v; break;
    case UART_SCR:     s->scr = v; break;
    default: break;
    }

    spin_unlock_irqrestore(UART_LOCK(s), flags);
}

static const mmio_dev_ops_t g_uart16550_ops = {
    .name  = "uart16550",
    .base  = UART16550_BASE,
    .size  = UART16550_SIZE,
    .read  = uart16550_read,
    .write = uart16550_write,
};

/*
 * uart16550_putchar — 从 VMM 自己（而非 guest 的 THR 写）往控制台送一个字节
 *
 * 唯一调用者是 RISC-V 的 SBI console_putchar：guest 在 8250 驱动起来之前
 * 用 SBI 打印早期信息 / panic。走的必须是与 THR 完全相同的那条通路，
 * 否则 helper 模式下这几行会漏进宿主内核日志（走 klog）而不是 helper 的
 * 终端 —— 表现是 guest 启动日志中间莫名少一段。
 */
void uart16550_putchar(vm_t *vm, uint8_t c)
{
    uint64_t flags;

    spin_lock_irqsave(&vm->uart16550_lock, &flags);
    uart_put_char(&vm->uart16550, c);
    spin_unlock_irqrestore(&vm->uart16550_lock, flags);
}

/* ── 端口 I/O 包装 ──────────────────────────────────────────
 *
 * x86 的 8250 驱动走端口 I/O（COM1 = 0x3F8），不是 MMIO。寄存器语义与
 * MMIO 版完全一致，所以这里只做「1 字节访问」的转发，让 PIO 后端复用
 * 同一份状态机（TX/RX 环形缓冲、IIR/LSR 逻辑）而不是再写一份。
 * 见 kernel/vmm/x86_64/vmx.c 的 x86_pio_handle()。
 *
 * 这两个不接 vm：状态在 dev->priv 里，锁再从 state->owner 回指拿。
 */
uint64_t uart16550_port_read(mmio_device_t *dev, uint64_t off)
{
    return uart16550_read(dev, off, 1);
}

void uart16550_port_write(mmio_device_t *dev, uint64_t off, uint8_t value)
{
    uart16550_write(dev, off, 1, value);
}

int uart16550_init(vm_t *vm, mmio_device_t *dev, mmio_bus_t *bus)
{
    if (!vm || !dev || !bus)
        return -1;

    /*
     * 整片清零是安全的：控制台归属（从前叫 state.tx_channel）现在住在
     * vm->console_owned，不在设备状态里，所以不再需要"先存后恢复"那一套。
     * 见 include/vmm/vmm_uart16550.h 里 state 定义下方的说明。
     */
    memset(&vm->uart16550, 0, sizeof(vm->uart16550));
    vm->uart16550.owner = vm;

    dev->ops  = &g_uart16550_ops;
    dev->priv = &vm->uart16550;

    return mmio_bus_register(bus, dev);
}
