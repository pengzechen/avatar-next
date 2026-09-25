/*
 * kernel/vmm/vdev/vuart16550.c — 虚拟 16550A UART 实现（riscv64/x86_64）
 *
 * 移植自 x-kernel: virt/vdev/uart16550/src/lib.rs，适配 Avatar OS：
 *   - klogger::kprint → klog_putchar（直启模式）/ TX 环形缓冲（helper 模式）
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
 */

#include "vmm/vmm_uart16550.h"
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

/* 缓冲深度：TX 要能扛住 guest 启动那一大串内核日志的突发 */
#define RX_FIFO_SIZE   4096
#define TX_FIFO_SIZE   8192

/* ── 设备私有状态 ─────────────────────────────────────────── */
typedef struct {
    uint8_t  ier;
    uint8_t  lcr;
    uint8_t  mcr;
    uint8_t  scr;
    uint8_t  fcr;
    uint8_t  dll;
    uint8_t  dlm;

    /* RX 环形缓冲：宿主/helper 写入，guest 读 RBR 弹出 */
    uint8_t  rx_fifo[RX_FIFO_SIZE];
    uint32_t rx_head;
    uint32_t rx_tail;
    uint32_t rx_count;
    uint32_t rx_drops;

    /* TX 环形缓冲：guest 写 THR 推入，宿主用户态 helper 读走 */
    uint8_t  tx_fifo[TX_FIFO_SIZE];
    uint32_t tx_head;
    uint32_t tx_tail;
    uint32_t tx_count;

    int      tx_channel;    /* 1 = 走 TX 缓冲；0 = 直打宿主控制台 */
} uart16550_state_t;

static uart16550_state_t g_uart16550;

/*
 * 宿主 pump 线程与 guest 写入路径（vCPU 任务 / helper 的 write(2)）不在同
 * 一个上下文，两把环都要用 IRQ-safe 的锁保护。
 */
static spinlock_noirq_t g_uart16550_lock = SPINLOCK_NOIRQ_INIT;

static int rx_irq_asserted_locked(const uart16550_state_t *s)
{
    return (s->rx_count > 0 && (s->ier & IER_RX_AVAILABLE) != 0)
        || (s->ier & IER_THR_EMPTY) != 0;
}

/* ── RX 通路（宿主 → guest）───────────────────────────────── */
void uart16550_push_rx(uint8_t c)
{
    uint64_t flags;

    spin_lock_irqsave(&g_uart16550_lock, &flags);
    if (g_uart16550.rx_count >= RX_FIFO_SIZE) {
        g_uart16550.rx_drops++;
        spin_unlock_irqrestore(&g_uart16550_lock, flags);
        return;
    }
    g_uart16550.rx_fifo[g_uart16550.rx_head] = c;
    g_uart16550.rx_head = (g_uart16550.rx_head + 1) % RX_FIFO_SIZE;
    g_uart16550.rx_count++;
    spin_unlock_irqrestore(&g_uart16550_lock, flags);
}

int uart16550_irq_asserted(void)
{
    uint64_t flags;
    int asserted;

    spin_lock_irqsave(&g_uart16550_lock, &flags);
    asserted = rx_irq_asserted_locked(&g_uart16550);
    spin_unlock_irqrestore(&g_uart16550_lock, flags);
    return asserted;
}

void uart16550_rx_flush(void)
{
    uint64_t flags;

    spin_lock_irqsave(&g_uart16550_lock, &flags);
    g_uart16550.rx_head  = 0;
    g_uart16550.rx_tail  = 0;
    g_uart16550.rx_count = 0;
    g_uart16550.rx_drops = 0;
    spin_unlock_irqrestore(&g_uart16550_lock, flags);
}

/* ── TX 通道（guest 输出 → 用户态 helper）─────────────────── */
void uart16550_tx_set_enabled(int enabled)
{
    uint64_t flags;

    spin_lock_irqsave(&g_uart16550_lock, &flags);
    g_uart16550.tx_channel = enabled ? 1 : 0;
    if (!enabled) {
        g_uart16550.tx_head  = 0;
        g_uart16550.tx_tail  = 0;
        g_uart16550.tx_count = 0;
    }
    spin_unlock_irqrestore(&g_uart16550_lock, flags);
}

int uart16550_tx_channel_enabled(void)
{
    uint64_t flags;
    int enabled;

    spin_lock_irqsave(&g_uart16550_lock, &flags);
    enabled = g_uart16550.tx_channel;
    spin_unlock_irqrestore(&g_uart16550_lock, flags);
    return enabled;
}

int uart16550_tx_pop(uint8_t *c)
{
    uint64_t flags;
    int got = 0;

    spin_lock_irqsave(&g_uart16550_lock, &flags);
    if (g_uart16550.tx_count > 0) {
        *c = g_uart16550.tx_fifo[g_uart16550.tx_tail];
        g_uart16550.tx_tail = (g_uart16550.tx_tail + 1) % TX_FIFO_SIZE;
        g_uart16550.tx_count--;
        got = 1;
    }
    spin_unlock_irqrestore(&g_uart16550_lock, flags);
    return got;
}

int uart16550_tx_has_data(void)
{
    uint64_t flags;
    int has;

    spin_lock_irqsave(&g_uart16550_lock, &flags);
    has = g_uart16550.tx_count > 0;
    spin_unlock_irqrestore(&g_uart16550_lock, flags);
    return has;
}

/*
 * uart_put_char — guest 写 THR 的一个字节
 *
 * 直启模式：立即 klog_putchar。宿主 uart_putchar 会把 '\n' 补成 "\r\n"，
 * 所以 guest 的裸 '\n'（earlycon）与 tty 已转好的 "\r\n" 都能正确换行。
 *
 * 注意本函数**在持锁状态下**调 klog_putchar：klog 自己也有锁，且
 * uart_putchar 在缓冲满时会等待。这把锁的中断已关，因此不会有锁序反转；
 * 与 vpl011_put_char 的处理一致。
 */
static void uart_put_char(uint8_t c)
{
    uart16550_state_t *s = &g_uart16550;

    if (!s->tx_channel) {
        klog_putchar((char)c);
        return;
    }

    if (s->tx_count >= TX_FIFO_SIZE) {
        /* 缓冲满：丢尾。helper 读得比 guest 写得慢时才会发生 */
        g_uart16550.tx_tail = (g_uart16550.tx_tail + 1) % TX_FIFO_SIZE;
        g_uart16550.tx_count--;
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

    spin_lock_irqsave(&g_uart16550_lock, &flags);
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

    spin_unlock_irqrestore(&g_uart16550_lock, flags);
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

    spin_lock_irqsave(&g_uart16550_lock, &flags);
    dlab = (s->lcr & LCR_DLAB) != 0;

    switch (off) {
    case UART_RBR_THR_DLL:
        if (dlab)
            s->dll = v;
        else
            uart_put_char(v);      /* THR：输出 guest 字符 */
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

    spin_unlock_irqrestore(&g_uart16550_lock, flags);
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
void uart16550_putchar(uint8_t c)
{
    uint64_t flags;

    spin_lock_irqsave(&g_uart16550_lock, &flags);
    uart_put_char(c);
    spin_unlock_irqrestore(&g_uart16550_lock, flags);
}

/* ── 端口 I/O 包装 ──────────────────────────────────────────
 *
 * x86 的 8250 驱动走端口 I/O（COM1 = 0x3F8），不是 MMIO。寄存器语义与
 * MMIO 版完全一致，所以这里只做「1 字节访问」的转发，让 PIO 后端复用
 * 同一份状态机（TX/RX 环形缓冲、IIR/LSR 逻辑）而不是再写一份。
 * 见 kernel/vmm/x86_64/vmx.c 的 x86_pio_handle()。
 */
uint64_t uart16550_port_read(mmio_device_t *dev, uint64_t off)
{
    return uart16550_read(dev, off, 1);
}

void uart16550_port_write(mmio_device_t *dev, uint64_t off, uint8_t value)
{
    uart16550_write(dev, off, 1, value);
}

int uart16550_init(mmio_device_t *dev, mmio_bus_t *bus)
{
    if (!dev || !bus)
        return -1;

    /*
     * tx_channel 是**宿主侧的模式选择**，不是设备寄存器状态：/dev/vmm 在
     * 调 guest_loader_run_linux() 之前就把通道打开，而那条路径会走到这里
     * （vm_create → vmm_console_init → uart16550_init）。整片 memset 会把它
     * 清回 0，于是：
     *   - guest 输出不再进 TX 缓冲，而是经 klog 直打宿主控制台；
     *   - vmm_console_pump() 复活，直接读真实 UART 抢宿主键盘，和用户态
     *     helper 争同一个 FIFO —— 谁先跑谁拿到。
     * 症状是「Ctrl+[ 之后回不到宿主 shell」：内核侧 detach 成功了，但按键
     * 仍被泵喂给 guest（helper 已经退出，没人再读它的 stdin）。也会让
     * Ctrl+] 收不到（这正是 aarch64 版 vpl011_init 里那段注释的由来）。
     *
     * 所以这里先存后恢复；其余字段（FIFO、寄存器）该清还是清。
     */
    int tx_channel = g_uart16550.tx_channel;

    memset(&g_uart16550, 0, sizeof(g_uart16550));
    g_uart16550.tx_channel = tx_channel;

    dev->ops  = &g_uart16550_ops;
    dev->priv = &g_uart16550;

    return mmio_bus_register(bus, dev);
}
