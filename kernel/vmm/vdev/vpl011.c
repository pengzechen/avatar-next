/*
 * kernel/vmm/vdev/vpl011.c — 虚拟 PL011 UART 设备实现
 *
 * 移植自 x-kernel: virt/vdev/vpl011/src/lib.rs，适配 Avatar OS：
 *   - klogger::kprint → klog_putchar（宿主内核日志）
 *   - RxChannel → 本文件内的 RX 环形缓冲 + vpl011_push_rx()；
 *     宿主侧由 vmm_console_pump() 轮询真实 PL011 喂入
 *   - TxChannel：kvmm 里只有控制设备打开了通道才逐字节直通，否则走宿主
 *     日志。avatar 没有独立控制设备 —— 宿主控制台本身就是终端，故**始终**
 *     直通：行缓冲会把 guest 的按键回显（无换行的单字符）一直憋到回车，
 *     交互式会话不可用。见 vpl011_put_char()。
 */

#include "vmm/vmm_vpl011.h"
#include "klog.h"
#include "string.h"
#include "spinlock.h"

/* ── PL011 寄存器偏移 ─────────────────────────────────────── */
#define UARTDR      0x000
#define UARTFR      0x018
#define UARTCR      0x030
#define UARTIMSC    0x038
#define UARTRIS     0x03C
#define UARTMIS     0x040
#define UARTICR     0x044

/* PrimeCell / Peripheral ID（ARM PL011 签名）*/
#define PERIPHID0   0xFE0
#define PERIPHID1   0xFE4
#define PERIPHID2   0xFE8
#define PERIPHID3   0xFEC
#define PCELLID0    0xFF0
#define PCELLID1    0xFF4
#define PCELLID2    0xFF8
#define PCELLID3    0xFFC

/* FR 标志位 */
#define FR_TXFE     (1u << 7)   /* TX FIFO 空 */
#define FR_RXFF     (1u << 6)   /* RX FIFO 满 */
#define FR_RXFE     (1u << 4)   /* RX FIFO 空 */

/* 中断位 */
#define INT_RX      (1u << 4)   /* RX 中断（RIS/MIS/IMSC bit4）*/

/*
 * RX FIFO 深度。真实 PL011 的硬件 FIFO 只有 16 字节，这里放宽到 256：
 * 宿主一次粘贴多字符时，guest 要等到下一次进中断才来取，16 字节不够用。
 */
#define RX_FIFO_SIZE   256

/* TX 缓冲深度：要能扛住 guest 启动那一大串内核日志的突发 */
#define TX_FIFO_SIZE   8192

/* ── 设备私有状态 ─────────────────────────────────────────── */
typedef struct {
    uint32_t cr;                    /* UARTCR */
    uint32_t imsc;                  /* UARTIMSC */

    /* RX 环形缓冲：宿主用户态写入（push），guest MMIO 读取（UARTDR）*/
    uint8_t  rx_fifo[RX_FIFO_SIZE];
    uint32_t rx_head;               /* 写入位置 */
    uint32_t rx_tail;               /* 读出位置 */
    uint32_t rx_count;

    /* TX 环形缓冲：guest MMIO 写入（UARTDR），宿主用户态读取（tx_pop）*/
    uint8_t  tx_fifo[TX_FIFO_SIZE];
    uint32_t tx_head;
    uint32_t tx_tail;
    uint32_t tx_count;
    int      tx_channel;            /* 1 = 走 TX 缓冲；0 = 直打宿主控制台 */

    uint64_t rx_dropped;            /* FIFO 满而丢弃的字节数 */
    uint64_t tx_dropped;
} vpl011_state_t;

/*
 * 设备私有状态（设备实例由调用方持有，见 vmm.c）。
 *
 * 并发：两个环都是**跨任务**的 ——
 *   - guest MMIO 读写发生在 vCPU 任务里（VMM 退出路径分发），
 *     此时宿主中断是开的（vmm_run_vcpu 在调用 exit_handler 前已 restore）；
 *   - 宿主侧 push_rx / tx_pop 在 /dev/vmm 的 read/write 里，属于 helper 任务。
 * 所以两把环都要用 IRQ-safe 的锁保护，不能像同线程时期那样裸操作。
 */
static vpl011_state_t g_vpl011;
static spinlock_noirq_t g_vpl011_lock = SPINLOCK_NOIRQ_INIT;

/* ── RX 中断线状态 ────────────────────────────────────────── */
/* 调用者需持有 g_vpl011_lock */
static int rx_irq_asserted_locked(const vpl011_state_t *s)
{
    return s->rx_count > 0 && (s->imsc & INT_RX) != 0;
}

int vpl011_rx_irq_asserted(void)
{
    uint64_t flags;
    int asserted;

    spin_lock_irqsave(&g_vpl011_lock, &flags);
    asserted = rx_irq_asserted_locked(&g_vpl011);
    spin_unlock_irqrestore(&g_vpl011_lock, flags);
    return asserted;
}

/* ── 宿主侧入口：压入一个控制台字节 ───────────────────────── */
void vpl011_push_rx(uint8_t c)
{
    uint64_t flags;

    spin_lock_irqsave(&g_vpl011_lock, &flags);
    if (g_vpl011.rx_count >= RX_FIFO_SIZE) {
        g_vpl011.rx_dropped++;
        spin_unlock_irqrestore(&g_vpl011_lock, flags);
        /*
         * 满则丢弃。这里可以打日志：调用点是任务上下文（/dev/vmm 的
         * write 或 vmm_run_vcpu 的循环），不在 klog 锁内。只报第一次。
         */
        if (g_vpl011.rx_dropped == 1)
            KLOG_WARN("[vpl011] RX FIFO full, dropping console input\n");
        return;
    }

    g_vpl011.rx_fifo[g_vpl011.rx_head] = c;
    g_vpl011.rx_head = (g_vpl011.rx_head + 1) % RX_FIFO_SIZE;
    g_vpl011.rx_count++;
    spin_unlock_irqrestore(&g_vpl011_lock, flags);
}

void vpl011_rx_flush(void)
{
    uint64_t flags;

    spin_lock_irqsave(&g_vpl011_lock, &flags);
    g_vpl011.rx_head = g_vpl011.rx_tail = g_vpl011.rx_count = 0;
    spin_unlock_irqrestore(&g_vpl011_lock, flags);
}

/* ── TX 通道（guest 输出 → 用户态 helper）─────────────────── */
void vpl011_tx_set_enabled(int enabled)
{
    uint64_t flags;

    spin_lock_irqsave(&g_vpl011_lock, &flags);
    g_vpl011.tx_channel = enabled ? 1 : 0;
    if (!enabled) {
        /* 关通道时清空残留，免得下次开通道吐出上一次会话的尾巴 */
        g_vpl011.tx_head = g_vpl011.tx_tail = g_vpl011.tx_count = 0;
    }
    spin_unlock_irqrestore(&g_vpl011_lock, flags);
}

int vpl011_tx_channel_enabled(void)
{
    return g_vpl011.tx_channel;
}

int vpl011_tx_has_data(void)
{
    uint64_t flags;
    int has;

    spin_lock_irqsave(&g_vpl011_lock, &flags);
    has = g_vpl011.tx_count > 0;
    spin_unlock_irqrestore(&g_vpl011_lock, flags);
    return has;
}

int vpl011_tx_pop(uint8_t *c)
{
    uint64_t flags;
    vpl011_state_t *s = &g_vpl011;

    if (!c)
        return 0;

    spin_lock_irqsave(&g_vpl011_lock, &flags);
    if (s->tx_count == 0) {
        spin_unlock_irqrestore(&g_vpl011_lock, flags);
        return 0;
    }
    *c = s->tx_fifo[s->tx_tail];
    s->tx_tail = (s->tx_tail + 1) % TX_FIFO_SIZE;
    s->tx_count--;
    spin_unlock_irqrestore(&g_vpl011_lock, flags);
    return 1;
}

/* ── 输出：通道模式走缓冲，否则直通宿主控制台 ─────────────── */
/* 调用者需持有 g_vpl011_lock */
static void put_char_locked(vpl011_state_t *s, uint8_t c)
{
    if (!s->tx_channel) {
        /*
         * 直启模式：立即输出。klog_putchar 直通 uart_putchar（未开中断时
         * 是直接 MMIO 写），不经行缓冲，所以 guest 按键回显是实时的。
         *
         * 注意这里**在持锁状态下**调 klog_putchar：klog 自己也有锁，且
         * uart_putchar 在缓冲满时会 timer_spin 等待。这把锁的中断已关，
         * 不至于死锁，但属于「锁内做 I/O」—— 换来的是直启模式下不引入
         * 新的锁层级。通道模式（helper）下不会走到这里。
         */
        klog_putchar((char)c);
        return;
    }

    if (s->tx_count >= TX_FIFO_SIZE) {
        s->tx_dropped++;
        return;                         /* 丢弃最新字节，与 kvmm 一致 */
    }
    s->tx_fifo[s->tx_head] = c;
    s->tx_head = (s->tx_head + 1) % TX_FIFO_SIZE;
    s->tx_count++;
}

/* ── MMIO 读写回调 ────────────────────────────────────────── */
static uint64_t vpl011_read(mmio_device_t *dev, uint64_t off, uint8_t size)
{
    vpl011_state_t *s = (vpl011_state_t *)dev->priv;
    uint64_t flags;
    uint64_t ret = 0;
    (void)size;

    spin_lock_irqsave(&g_vpl011_lock, &flags);

    switch (off) {
    case UARTDR: {
        uint8_t c = 0;
        if (s->rx_count > 0) {
            c = s->rx_fifo[s->rx_tail];
            s->rx_tail = (s->rx_tail + 1) % RX_FIFO_SIZE;
            s->rx_count--;
        }
        ret = (uint64_t)c;
        break;
    }
    case UARTFR: {
        uint64_t fr = FR_TXFE;              /* 输出永远不阻塞 */
        if (s->rx_count == 0)
            fr |= FR_RXFE;
        if (s->rx_count >= RX_FIFO_SIZE)
            fr |= FR_RXFF;
        ret = fr;
        break;
    }
    case UARTCR:
        ret = s->cr;
        break;
    case UARTIMSC:
        ret = s->imsc;
        break;
    case UARTRIS:
        /* 电平触发：数据还在 FIFO 里就一直为高 */
        ret = rx_irq_asserted_locked(s) ? INT_RX : 0;
        break;
    case UARTMIS:
        ret = rx_irq_asserted_locked(s) ? INT_RX : 0;
        break;
    case PERIPHID0: ret = 0x11; break;
    case PERIPHID1: ret = 0x10; break;
    case PERIPHID2: ret = 0x14; break;
    case PERIPHID3: ret = 0x00; break;
    case PCELLID0:  ret = 0x0D; break;
    case PCELLID1:  ret = 0xF0; break;
    case PCELLID2:  ret = 0x05; break;
    case PCELLID3:  ret = 0xB1; break;
    default:
        ret = 0;
        break;
    }

    spin_unlock_irqrestore(&g_vpl011_lock, flags);
    return ret;
}

static void vpl011_write(mmio_device_t *dev, uint64_t off, uint8_t size,
                         uint64_t value)
{
    vpl011_state_t *s = (vpl011_state_t *)dev->priv;
    uint64_t flags;
    (void)size;

    spin_lock_irqsave(&g_vpl011_lock, &flags);

    switch (off) {
    case UARTDR:
        put_char_locked(s, (uint8_t)value);
        break;
    case UARTCR:
        s->cr = (uint32_t)value;
        break;
    case UARTIMSC:
        /* RX 中断的使能位只影响 RIS/MIS 的呈现，不改变 FIFO 内容；
         * 置 pending 由调用方在进入 guest 前按 vpl011_rx_irq_asserted() 做。*/
        s->imsc = (uint32_t)value;
        break;
    case UARTICR:
        /* RIS 完全由 FIFO 状态导出（电平触发），没有需要清的粘滞位 */
        break;
    default:
        break;
    }

    spin_unlock_irqrestore(&g_vpl011_lock, flags);
}

static const mmio_dev_ops_t g_vpl011_ops = {
    .name  = "vpl011",
    .base  = VPL011_BASE,
    .size  = VPL011_SIZE,
    .read  = vpl011_read,
    .write = vpl011_write,
};

int vpl011_init(mmio_device_t *dev, mmio_bus_t *bus)
{
    if (!dev || !bus)
        return -1;

    /*
     * tx_channel 是**宿主侧的模式选择**，不是设备寄存器状态：/dev/vmm 在
     * 调 guest_loader_run_linux() 之前就把通道打开，而那条路径会走到这里
     * （vm_create → vpl011_init）。整片 memset 会把它清回 0，于是通道模式
     * 失效、vmm_console_pump 继续跑，结果就是宿主泵和用户态 helper 抢同一个
     * UART —— 用户按键被泵吃掉，helper 永远收不到 Ctrl+]。
     * 所以这里先存后恢复；其余字段（FIFO、寄存器）该清还是清。
     */
    int tx_channel = g_vpl011.tx_channel;

    memset(&g_vpl011, 0, sizeof(g_vpl011));
    g_vpl011.cr = 0x301;   /* UARTEN | TXE | RXE */
    g_vpl011.tx_channel = tx_channel;

    dev->ops  = &g_vpl011_ops;
    dev->priv = &g_vpl011;

    return mmio_bus_register(bus, dev);
}
