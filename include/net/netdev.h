#ifndef NET_NETDEV_H
#define NET_NETDEV_H

#include "types.h"

#define NETDEV_FRAME_MAX 1514U

typedef struct netdev {
    const char *name;
    uint8_t mac[6];
    void *ctx;
    int (*send)(void *ctx, const uint8_t *frame, size_t len);
    int (*recv)(void *ctx, uint8_t *frame, size_t maxlen);
} netdev_t;

int netdev_register(netdev_t *dev);
netdev_t *netdev_default(void);
int netdev_send(const uint8_t *frame, size_t len);
int netdev_recv(uint8_t *frame, size_t maxlen);
void netdev_mac(uint8_t mac[6]);

#endif /* NET_NETDEV_H */
