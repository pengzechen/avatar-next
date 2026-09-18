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

    /*
     * 可选：把收到的帧**直接写进调用方给的缓冲**，语义与 recv 完全相同
     * （返回帧长 / 0 无帧 / -1 错误）。
     *
     * 存在的意义是省掉一次拷贝：没有它时路径是
     *     DMA 缓冲 --(驱动 memcpy)--> 栈上 frame --(pbuf_take)--> pbuf
     * 有它时是
     *     DMA 缓冲 --(驱动 memcpy)--> pbuf payload
     * 1400 字节的帧省下约 8µs，实测占每帧总开销的 ~30%。
     *
     * 为 NULL 时回退到 recv()。
     */
    int (*recv_into)(void *ctx, uint8_t *dst, size_t maxlen);
} netdev_t;

int netdev_register(netdev_t *dev);
netdev_t *netdev_default(void);
int netdev_send(const uint8_t *frame, size_t len);
int netdev_recv(uint8_t *frame, size_t maxlen);

/* 同 netdev_recv，但驱动会直接写进 dst（若驱动支持 recv_into），少一次拷贝 */
int netdev_recv_into(uint8_t *dst, size_t maxlen);

void netdev_mac(uint8_t mac[6]);

/*
 * ── 收包通知 ────────────────────────────────────────────────────────────
 *
 * 有收包中断的驱动：init 时调一次 netdev_rx_set_irq_backed()，之后在 ISR 里
 * 调 netdev_rx_wakeup() 报告"有新帧了"。
 *
 * 轮询方用 netdev_rx_pending() 挡掉空轮询。这一条的意义：空轮询要碰描述符，
 * 而描述符的 cache invalidate 带两次 fence，约 0.9µs；net-poll 每秒空转
 * 10 万次就是 ~9% 的 CPU —— 在 CPU 份额本来就紧张的场景下，这 9% 很值钱。
 *
 * 没有中断通知机制的驱动（如 virtio）**不要登记**，此时 netdev_rx_pending()
 * 恒返回 true，轮询方退化回"每次都查"，行为不变。
 *
 * netdev_rx_wakeup() 运行在中断上下文：只能置标志，不能睡眠、不能做包处理。
 */
void netdev_rx_set_irq_backed(void);
void netdev_rx_wakeup(void);

/* 自上次调用以来有没有新帧（读并清除）。无中断通知时恒为 true。 */
bool netdev_rx_pending(void);

#endif /* NET_NETDEV_H */
