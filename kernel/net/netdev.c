#include "net/netdev.h"

#include "klog.h"
#include "string.h"

static netdev_t *g_default_netdev;

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
    netdev_t *dev = g_default_netdev;
    if (!dev || !dev->recv || !frame || maxlen == 0U)
        return -1;
    return dev->recv(dev->ctx, frame, maxlen);
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
