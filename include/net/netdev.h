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

/*
 * ── 收包唤醒链路 ────────────────────────────────────────────────────────
 *
 * 驱动在自己的收包中断里调 netdev_rx_wakeup()，netdev 层转发给注册者
 * （由网络栈注册，用它把 net-poll 任务从阻塞中唤醒）。这样驱动只需要知道
 * netdev 这一层，不需要知道网络栈的任务模型。
 *
 * netdev_rx_wakeup() 运行在**中断上下文**：实现只能做非阻塞的入队/置标志，
 * 不能睡眠、不能做包处理。
 *
 * 目前**没有任何注册者**：net-poll 走协作式让出，不是阻塞等唤醒。
 * 原因见 kernel/net/net.c 的"收包唤醒链路（暂缓）"—— 在任务上下文里阻塞、
 * 再由中断唤醒这个模式，本内核的异常返回路径还不支持。这里的接口先留着，
 * 等那条路修好再挂上去。
 */
void netdev_set_rx_wakeup(void (*fn)(void));
void netdev_rx_wakeup(void);

#endif /* NET_NETDEV_H */
