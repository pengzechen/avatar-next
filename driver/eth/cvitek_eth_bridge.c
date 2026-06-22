#include "eth_api.h"
#include "klog.h"
#include "net/netdev.h"
#include "string.h"
#include "mm_vm.h"

#ifndef DEVICE_ETH_BASE_RAW
#define DEVICE_ETH_BASE_RAW 0x04070000U
#endif

static CvitekEthNic_t *g_cvitek_nic;

static int cvitek_netdev_send(void *ctx, const uint8_t *frame, size_t len)
{
    return eth_send((CvitekEthNic_t *)ctx, frame, len);
}

static int cvitek_netdev_recv(void *ctx, uint8_t *frame, size_t maxlen)
{
    return eth_recv((CvitekEthNic_t *)ctx, frame, maxlen);
}

void cvitek_eth_init_from_platform(void)
{
    uint64_t base = (uint64_t)DEVICE_ETH_BASE_RAW;
#if DEVICE_MMIO_NEEDS_VMA
    base += KERNEL_VMA;
#endif
    KLOG_INFO("[cvitek-eth] init base=0x%llx\n", (unsigned long long)base);

    g_cvitek_nic = eth_init(base);
    if (!g_cvitek_nic) {
        KLOG_ERROR("[cvitek-eth] eth_init failed\n");
        return;
    }

    static netdev_t cvitek_dev;
    memset(&cvitek_dev, 0, sizeof(cvitek_dev));
    cvitek_dev.name = "cvitek0";
    cvitek_dev.ctx = g_cvitek_nic;
    cvitek_dev.send = cvitek_netdev_send;
    cvitek_dev.recv = cvitek_netdev_recv;
    eth_mac_addr(g_cvitek_nic, cvitek_dev.mac);
    netdev_register(&cvitek_dev);
}
