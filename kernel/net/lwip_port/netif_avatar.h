#ifndef NET_LWIP_PORT_NETIF_AVATAR_H
#define NET_LWIP_PORT_NETIF_AVATAR_H

#include "lwip/netif.h"

err_t avatar_netif_init(struct netif *netif);
void avatar_lwip_poll_rx(struct netif *netif);

#endif /* NET_LWIP_PORT_NETIF_AVATAR_H */
