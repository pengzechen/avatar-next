/*
 * kernel/vmm/vdev/vuart16550.c — 虚拟 16550A UART 实现（riscv64）
 *
 * 移植自 x-kernel: virt/vdev/uart16550/src/lib.rs，适配 Avatar OS：
 *   - klogger::kprint → klog_putchar
 *   - RxChannel/TxChannel 暂不引入（avatar 无控制设备）
 *   - 中断（PLIC）暂未接线 → 轮询模式
 */

#include "vmm/vmm_uart16550.h"
#include "klog.h"
#include "string.h"

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

/* LSR 位 */
#define LSR_DATA_READY     (1u << 0)
#define LSR_THRE           (1u << 5)
#define LSR_TEMT           (1u << 6)

/* IIR 值 */
#define IIR_NO_INTERRUPT   (1u << 0)
#define IIR_RX_AVAILABLE   0x04
#define IIR_THR_EMPTY      0x02

/* LCR 位 7：除数锁存使能（DLAB）*/
#define LCR_DLAB           (1u << 7)

#define LINE_BUF_SIZE  256

/* ── 设备私有状态 ─────────────────────────────────────────── */
typedef struct {
    uint8_t ier;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t scr;
    uint8_t dll;
    uint8_t dlm;
    char     line_buf[LINE_BUF_SIZE];
    uint32_t line_len;
} uart16550_state_t;

static uart16550_state_t g_uart16550;

/* ── 行缓冲输出到宿主日志（同 vpl011 语义）────────────────── */
static void uart_flush_line(int newline)
{
    uart16550_state_t *s = &g_uart16550;

    if (s->line_len == 0)
        return;

    for (uint32_t i = 0; i < s->line_len; i++)
        klog_putchar(s->line_buf[i]);
    if (newline)
        klog_putchar('\n');

    s->line_len = 0;
}

static int uart_line_ends_with(const char *suffix)
{
    uint32_t slen = (uint32_t)strlen(suffix);

    if (g_uart16550.line_len < slen)
        return 0;
    return memcmp(g_uart16550.line_buf + g_uart16550.line_len - slen,
                  suffix, slen) == 0;
}

static void uart_put_char(uint8_t c)
{
    uart16550_state_t *s = &g_uart16550;

    if (c == '\r')
        return;                    /* CR 忽略，由 LF 断行（对标 kvmm）*/
    if (c == '\n') {
        uart_flush_line(1);
        return;
    }

    if (s->line_len >= LINE_BUF_SIZE)
        uart_flush_line(1);

    s->line_buf[s->line_len++] = (char)c;

    if (uart_line_ends_with("login: ") ||
        uart_line_ends_with("Password: ") ||
        uart_line_ends_with("# ") ||
        uart_line_ends_with("$ ")) {
        uart_flush_line(0);
    }
}

/* ── MMIO 读写回调 ────────────────────────────────────────── */
static uint64_t uart16550_read(mmio_device_t *dev, uint64_t off, uint8_t size)
{
    uart16550_state_t *s = (uart16550_state_t *)dev->priv;
    int dlab = (s->lcr & LCR_DLAB) != 0;

    if (size != 1 && size != 4)
        return 0;

    switch (off) {
    case UART_RBR_THR_DLL: return dlab ? s->dll : 0;   /* RX 恒空 */
    case UART_IER_DLM:     return dlab ? s->dlm : s->ier;
    case UART_IIR_FCR:
        /* 无中断挂起（THR 恒空但 guest 若未使能则无中断）*/
        return IIR_NO_INTERRUPT;
    case UART_LCR:         return s->lcr;
    case UART_MCR:         return s->mcr;
    case UART_LSR:
        /* THR/TEMT 恒置位（发送即时完成）；无 RX 数据 */
        return LSR_THRE | LSR_TEMT;
    case UART_MSR:         return 0;
    case UART_SCR:         return s->scr;
    default:               return 0;
    }
}

static void uart16550_write(mmio_device_t *dev, uint64_t off, uint8_t size,
                            uint64_t value)
{
    uart16550_state_t *s = (uart16550_state_t *)dev->priv;
    uint8_t v = (uint8_t)value;
    int dlab;

    if (size != 1 && size != 4)
        return;

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
    case UART_IIR_FCR: break;      /* FIFO 控制无实际效果 */
    case UART_LCR:     s->lcr = v; break;
    case UART_MCR:     s->mcr = v; break;
    case UART_SCR:     s->scr = v; break;
    default: break;
    }
}

static const mmio_dev_ops_t g_uart16550_ops = {
    .name  = "uart16550",
    .base  = UART16550_BASE,
    .size  = UART16550_SIZE,
    .read  = uart16550_read,
    .write = uart16550_write,
};

int uart16550_init(mmio_device_t *dev, mmio_bus_t *bus)
{
    if (!dev || !bus)
        return -1;

    memset(&g_uart16550, 0, sizeof(g_uart16550));

    dev->ops  = &g_uart16550_ops;
    dev->priv = &g_uart16550;

    return mmio_bus_register(bus, dev);
}
