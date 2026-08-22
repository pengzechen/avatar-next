#ifndef NET_DHCP_SERVER_H
#define NET_DHCP_SERVER_H

#include "lwip/ip4_addr.h"

void dhcp_server_init(const ip4_addr_t *server_ip,
                      const ip4_addr_t *netmask,
                      const ip4_addr_t *lease_ip);

#endif /* NET_DHCP_SERVER_H */
