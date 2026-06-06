#ifndef __DRIVER_ETH_VIRTIO_NET_H__
#define __DRIVER_ETH_VIRTIO_NET_H__

#include "types.h"

typedef struct virtio_net_nic VirtioNetNic_t;

VirtioNetNic_t *eth_init(uint64_t base);
int eth_send(VirtioNetNic_t *nic, const uint8_t *data, size_t len);
int eth_recv(VirtioNetNic_t *nic, uint8_t *buf, size_t maxlen);
void eth_mac_addr(VirtioNetNic_t *nic, uint8_t mac[6]);

void virtio_net_init_from_platform(void);
void virtio_net_poll_demo_task(void *arg);

#endif /* __DRIVER_ETH_VIRTIO_NET_H__ */
