#include "netif_avatar.h"

#include "klog.h"
#include "net/netdev.h"
#include "string.h"

#include "lwip/etharp.h"
#include "lwip/pbuf.h"

#include "netif/ethernet.h"

static err_t avatar_linkoutput(struct netif *netif, struct pbuf *p)
{
    (void)netif;

    uint8_t frame[NETDEV_FRAME_MAX];
    size_t off = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        if (off + q->len > sizeof(frame)) {
            KLOG_WARN("[lwip] tx frame too large len=%zu add=%u\n",
                      off, (unsigned)q->len);
            return ERR_BUF;
        }
        memcpy(frame + off, q->payload, q->len);
        off += q->len;
        if (q->tot_len == q->len)
            break;
    }

    return netdev_send(frame, off) == 0 ? ERR_OK : ERR_IF;
}

err_t avatar_netif_init(struct netif *netif)
{
    uint8_t mac[6];
    netdev_mac(mac);

    netif->name[0] = 'a';
    netif->name[1] = 'v';
    netif->output = etharp_output;
    netif->linkoutput = avatar_linkoutput;
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, mac, 6);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;

    KLOG_INFO("[lwip] netif av0 mac=%02x:%02x:%02x:%02x:%02x:%02x mtu=%u\n",
              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
              (unsigned)netif->mtu);
    return ERR_OK;
}

void avatar_lwip_poll_rx(struct netif *netif)
{
    static unsigned rx_count;
    static unsigned idle_spins;

    /*
     * ── 空轮询快速路径 ──────────────────────────────────────────────────
     *
     * 收包中断是可靠的"有新帧了"信号（驱动在 ISR 里 netdev_rx_wakeup()）。
     * 没有该信号时整段跳过，连描述符都不碰 —— 描述符的 cache invalidate
     * 每次带两次 fence，而 net-poll 空闲时每秒空转约 10 万次，那是 ~9% 的
     * CPU，在 CPU 份额紧张时这 9% 直接决定丢不丢包。
     *
     * 每 16 次兜底真查一遍：万一中断丢了（或驱动没有中断通知机制）也不会
     * 永久停摆，最坏是空载时多花 ~0.6% 的 CPU。
     */
    if (!netdev_rx_pending() && (++idle_spins & 0x0FU) != 0U)
        return;
    idle_spins = 0;

    for (;;) {
        /*
         * 先按最大帧长分配 pbuf，再让驱动把帧**直接写进它的 payload** ——
         * 这样全路径只剩一次 1400 字节的拷贝（此前是"驱动→栈上 frame→pbuf"
         * 两次，实测共占每帧开销的 62%）。
         *
         * 按 PBUF_POOL_BUFSIZE(1536) 分配，pool 本来就不管请求长度、总是给
         * 一整块，所以按最大帧长要并不会更贵。收到后 pbuf_realloc 收窄到实际
         * 长度（单缓冲 pbuf 只是改 len/tot_len，不搬数据）。
         */
        struct pbuf *p = pbuf_alloc(PBUF_RAW, NETDEV_FRAME_MAX, PBUF_POOL);
        if (!p) {
            KLOG_WARN("[lwip] rx: no pbuf\n");
            break;                      /* 池满，下一轮再试 */
        }

        int n = netdev_recv_into((uint8_t *)p->payload, NETDEV_FRAME_MAX);
        if (n <= 0) {
            pbuf_free(p);
            break;
        }

        pbuf_realloc(p, (u16_t)n);

        rx_count++;
        if (rx_count <= 16U || (rx_count % 32U) == 0U) {
            uint8_t *f = (uint8_t *)p->payload;
            uint16_t etype = n >= 14 ? ((uint16_t)f[12] << 8) | f[13] : 0U;
            KLOG_INFO("[lwip] rx #%u len=%d etype=0x%04x\n", rx_count, n, etype);
        }

        err_t rc = netif->input(p, netif);
        if (rc != ERR_OK) {
            KLOG_WARN("[lwip] input failed len=%d rc=%d\n", n, rc);
            pbuf_free(p);
        }
    }
}
