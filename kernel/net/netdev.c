#include "net/netdev.h"

#include "klog.h"
#include "string.h"

static netdev_t *g_default_netdev;

/*
 * 收包通知状态。只在启动阶段写一次（irq_backed），运行期读；
 * pending 由 ISR 置位、轮询方读取并清除。
 */
static bool g_rx_irq_backed;
static volatile bool g_rx_pending;

void netdev_rx_set_irq_backed(void)
{
    g_rx_irq_backed = true;
}

/* 中断上下文：只置一个标志 */
void netdev_rx_wakeup(void)
{
    g_rx_pending = true;
}

bool netdev_rx_pending(void)
{
    /* 没有中断通知机制的驱动：老实报告"可能有帧"，轮询方每次都查 */
    if (!g_rx_irq_backed)
        return true;

    if (!g_rx_pending)
        return false;

    g_rx_pending = false;
    return true;
}

int netdev_register(netdev_t *dev)
{
    if (!dev || !dev->send || !dev->recv) {
        KLOG_ERROR("[netdev] invalid device registration\n");
        return -1;
    }

    g_default_netdev = dev;
    KLOG_INFO("[netdev] registered %s mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
              dev->name ? dev->name : "net0",
              dev->mac[0], dev->mac[1], dev->mac[2],
              dev->mac[3], dev->mac[4], dev->mac[5]);
    return 0;
}

netdev_t *netdev_default(void)
{
    return g_default_netdev;
}

int netdev_send(const uint8_t *frame, size_t len)
{
    netdev_t *dev = g_default_netdev;
    if (!dev || !dev->send || !frame || len == 0U)
        return -1;
    return dev->send(dev->ctx, frame, len);
}

int netdev_recv(uint8_t *frame, size_t maxlen)
{
    static unsigned rx_count;
    static unsigned empty_count;
    netdev_t *dev = g_default_netdev;
    if (!dev || !dev->recv || !frame || maxlen == 0U)
        return -1;

    int n = dev->recv(dev->ctx, frame, maxlen);
    if (n > 0) {
        rx_count++;
        if (rx_count <= 16U || (rx_count % 32U) == 0U) {
            uint16_t etype = n >= 14 ? ((uint16_t)frame[12] << 8) | frame[13] : 0U;
            KLOG_INFO("[netdev] rx #%u dev=%s len=%d etype=0x%04x\n",
                      rx_count, dev->name ? dev->name : "net0", n, etype);
        }
    } else if (n == 0) {
        empty_count++;
        if (empty_count <= 8U || (empty_count & (empty_count - 1U)) == 0U) {
            // KLOG_INFO("[netdev] rx empty #%u dev=%s\n",
                    //   empty_count, dev->name ? dev->name : "net0");
        }
    }
    return n;
}

/*
 * 同 netdev_recv，但让驱动直接写进 dst。
 * 驱动没有实现 recv_into 时回退到 recv（此时 driver 会经由内部缓冲再拷一次）。
 */
int netdev_recv_into(uint8_t *dst, size_t maxlen)
{
    netdev_t *dev = g_default_netdev;

    if (!dev || !dst || maxlen == 0U)
        return -1;

    if (dev->recv_into != NULL)
        return dev->recv_into(dev->ctx, dst, maxlen);

    return netdev_recv(dst, maxlen);
}

void netdev_mac(uint8_t mac[6])
{
    if (!mac)
        return;
    if (!g_default_netdev) {
        memset(mac, 0, 6);
        return;
    }
    memcpy(mac, g_default_netdev->mac, 6);
}
