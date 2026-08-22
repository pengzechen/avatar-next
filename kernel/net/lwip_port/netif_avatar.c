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
    uint8_t frame[NETDEV_FRAME_MAX];

    for (;;) {
        int n = netdev_recv(frame, sizeof(frame));
        if (n <= 0)
            break;

        rx_count++;
        if (rx_count <= 16U || (rx_count % 32U) == 0U) {
            uint16_t etype = n >= 14 ? ((uint16_t)frame[12] << 8) | frame[13] : 0U;
            KLOG_INFO("[lwip] rx #%u len=%d etype=0x%04x\n", rx_count, n, etype);
        }

        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
        if (!p) {
            KLOG_WARN("[lwip] drop rx frame len=%d: no pbuf\n", n);
            continue;
        }

        err_t rc = pbuf_take(p, frame, (u16_t)n);
        if (rc != ERR_OK) {
            KLOG_WARN("[lwip] pbuf_take failed len=%d rc=%d\n", n, rc);
            pbuf_free(p);
            continue;
        }

        rc = netif->input(p, netif);
        if (rc != ERR_OK) {
            KLOG_WARN("[lwip] input failed len=%d rc=%d\n", n, rc);
            pbuf_free(p);
        }
    }
}
