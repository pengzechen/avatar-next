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

/* ── 设备私有状态 ─────────────────────────────────────────── */
typedef struct {
    uint32_t cr;                    /* UARTCR */
    uint32_t imsc;                  /* UARTIMSC */

    /* RX 环形缓冲：宿主控制台写入（push），guest MMIO 读取（UARTDR）*/
    uint8_t  rx_fifo[RX_FIFO_SIZE];
    uint32_t rx_head;               /* 写入位置 */
    uint32_t rx_tail;               /* 读出位置 */
    uint32_t rx_count;

    uint64_t rx_dropped;            /* FIFO 满而丢弃的字节数 */
} vpl011_state_t;

/*
 * 设备私有状态（设备实例由调用方持有，见 vmm.c）。
 *
 * 并发：push 与 MMIO 读写都发生在 **同一个 vCPU 任务** 里 ——
 * Guest 的 MMIO 访问在 VMM 退出路径（vmm_arch_exit_handler）中分发，
 * 宿主侧的 push 在 vmm_run_vcpu 的循环里，两者同线程，无需加锁。
 */
static vpl011_state_t g_vpl011;

/* ── 输出：逐字节直通宿主控制台 ───────────────────────────── */
static void vpl011_put_char(uint8_t c)
{
    /*
     * 立即输出。klog_putchar 直通 uart_putchar（未开中断时是直接 MMIO 写），
     * 不经行缓冲，所以 guest 的按键回显是实时的。
     *
     * 代价：guest 输出可能与宿主日志在同一个字符位置上交错。宿主日志在
     * guest 运行期间本就稀少（WFI 心跳已降为 DEBUG），交互性优先。
     */
    klog_putchar((char)c);
}

/* ── RX 中断线状态 ────────────────────────────────────────── */
static int rx_irq_asserted(const vpl011_state_t *s)
{
    return s->rx_count > 0 && (s->imsc & INT_RX) != 0;
}

int vpl011_rx_irq_asserted(void)
{
    return rx_irq_asserted(&g_vpl011);
}

/* ── 宿主侧入口：压入一个控制台字节 ───────────────────────── */
void vpl011_push_rx(uint8_t c)
{
    vpl011_state_t *s = &g_vpl011;

    if (s->rx_count >= RX_FIFO_SIZE) {
        /*
         * 满则丢弃。这里可以打日志：调用点在 vmm_run_vcpu 的循环里，
         * 既不在 klog 锁内也不在中断上下文。只报第一次，避免刷屏。
         */
        if (s->rx_dropped++ == 0)
            KLOG_WARN("[vpl011] RX FIFO full, dropping console input\n");
        return;
    }

    s->rx_fifo[s->rx_head] = c;
    s->rx_head = (s->rx_head + 1) % RX_FIFO_SIZE;
    s->rx_count++;
}

/* ── MMIO 读写回调 ────────────────────────────────────────── */
static uint64_t vpl011_read(mmio_device_t *dev, uint64_t off, uint8_t size)
{
    vpl011_state_t *s = (vpl011_state_t *)dev->priv;
    (void)size;

    switch (off) {
    case UARTDR: {
        uint8_t c = 0;
        if (s->rx_count > 0) {
            c = s->rx_fifo[s->rx_tail];
            s->rx_tail = (s->rx_tail + 1) % RX_FIFO_SIZE;
            s->rx_count--;
        }
        return (uint64_t)c;
    }
    case UARTFR: {
        uint64_t fr = FR_TXFE;              /* 输出永远不阻塞 */
        if (s->rx_count == 0)
            fr |= FR_RXFE;
        if (s->rx_count >= RX_FIFO_SIZE)
            fr |= FR_RXFF;
        return fr;
    }
    case UARTCR:
        return s->cr;
    case UARTIMSC:
        return s->imsc;
    case UARTRIS:
        /* 电平触发：数据还在 FIFO 里就一直为高 */
        return rx_irq_asserted(s) ? INT_RX : 0;
    case UARTMIS:
        return rx_irq_asserted(s) ? INT_RX : 0;
    case PERIPHID0: return 0x11;
    case PERIPHID1: return 0x10;
    case PERIPHID2: return 0x14;
    case PERIPHID3: return 0x00;
    case PCELLID0:  return 0x0D;
    case PCELLID1:  return 0xF0;
    case PCELLID2:  return 0x05;
    case PCELLID3:  return 0xB1;
    default:
        return 0;
    }
}

static void vpl011_write(mmio_device_t *dev, uint64_t off, uint8_t size,
                         uint64_t value)
{
    vpl011_state_t *s = (vpl011_state_t *)dev->priv;
    (void)size;

    switch (off) {
    case UARTDR:
        vpl011_put_char((uint8_t)value);
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

    memset(&g_vpl011, 0, sizeof(g_vpl011));
    g_vpl011.cr = 0x301;   /* UARTEN | TXE | RXE */

    dev->ops  = &g_vpl011_ops;
    dev->priv = &g_vpl011;

    return mmio_bus_register(bus, dev);
}
