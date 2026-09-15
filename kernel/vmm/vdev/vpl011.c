/*
 * kernel/vmm/vdev/vpl011.c — 虚拟 PL011 UART 设备实现
 *
 * 移植自 x-kernel: virt/vdev/vpl011/src/lib.rs，适配 Avatar OS：
 *   - klogger::kprint → klog_putchar（宿主内核日志）
 *   - RxChannel/TxChannel（宿主控制设备通道）暂不引入：avatar 无控制设备，
 *     RX 恒空、TX 直接走宿主日志。后续如需 guest 交互输入再接 RX FIFO。
 */

#include "vmm_vpl011.h"
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
#define FR_RXFE     (1u << 4)   /* RX FIFO 空 */

/* 中断位 */
#define INT_RX      (1u << 4)   /* RX 中断（RIS/MIS/IMSC bit4）*/

#define LINE_BUF_SIZE  256

/* ── 设备私有状态 ─────────────────────────────────────────── */
typedef struct {
    uint32_t cr;        /* UARTCR */
    uint32_t imsc;      /* UARTIMSC */
    char     line_buf[LINE_BUF_SIZE];
    uint32_t line_len;
} vpl011_state_t;

/* 设备私有状态（设备实例由调用方持有，见 vmm.c）*/
static vpl011_state_t g_vpl011;

/* ── 行缓冲输出到宿主日志 ─────────────────────────────────── */
/*
 * kvmm 语义：把 guest 的 console 输出按行聚合后打到宿主日志，
 * 避免逐字节输出把内核日志撕碎；对 "login: "/"# " 等无换行提示符
 * 提前 flush，使交互提示能立即出现。
 */
static void vpl011_flush_line(int newline)
{
    vpl011_state_t *s = &g_vpl011;

    if (s->line_len == 0)
        return;

    for (uint32_t i = 0; i < s->line_len; i++)
        klog_putchar(s->line_buf[i]);
    if (newline)
        klog_putchar('\n');

    s->line_len = 0;
}

static int line_ends_with(const char *suffix)
{
    uint32_t slen = (uint32_t)strlen(suffix);

    if (g_vpl011.line_len < slen)
        return 0;
    return memcmp(g_vpl011.line_buf + g_vpl011.line_len - slen,
                  suffix, slen) == 0;
}

static void vpl011_put_char(uint8_t c)
{
    vpl011_state_t *s = &g_vpl011;

    if (c == '\n' || c == '\r') {
        vpl011_flush_line(1);
        return;
    }

    if (s->line_len >= LINE_BUF_SIZE)
        vpl011_flush_line(1);

    s->line_buf[s->line_len++] = (char)c;

    /* 常见无换行提示符：立即 flush（不补换行）*/
    if (line_ends_with("login: ") ||
        line_ends_with("Password: ") ||
        line_ends_with("# ") ||
        line_ends_with("$ ")) {
        vpl011_flush_line(0);
    }
}

/* ── MMIO 读写回调 ────────────────────────────────────────── */
static uint64_t vpl011_read(mmio_device_t *dev, uint64_t off, uint8_t size)
{
    vpl011_state_t *s = (vpl011_state_t *)dev->priv;
    (void)size;

    switch (off) {
    case UARTDR:
        return 0;                    /* RX FIFO 恒空，见头文件说明 */
    case UARTFR:
        return (uint64_t)(FR_TXFE | FR_RXFE);
    case UARTCR:
        return s->cr;
    case UARTIMSC:
        return s->imsc;
    case UARTRIS:
        return 0;                    /* 无 RX 数据 → 无中断 */
    case UARTMIS:
        return (uint64_t)(0 & s->imsc);
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
        s->imsc = (uint32_t)value;
        break;
    case UARTICR:
        break;                       /* 无中断状态需清除 */
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
