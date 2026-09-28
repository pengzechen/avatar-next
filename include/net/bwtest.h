#ifndef NET_BWTEST_H
#define NET_BWTEST_H

/*
 * include/net/bwtest.h — 以太网带宽测试（UDP 丢弃式接收端 + 每秒速率统计）
 *
 * 用法：
 *   1. 用 LOG=none 构建（否则量到的是串口速度，见 kernel/net/bwtest.c 说明）
 *   2. PC 侧往 <板子IP>:1235 打 UDP 包，每包前 8 字节是 "AVBW" magic + 序号
 *   3. 板子串口每秒打一行速率/丢包统计
 */

/* 绑定 UDP sink。由 net_init() 调用。 */
void bwtest_init(void);

/* 打印每秒统计。由 net_poll_once() 调用；无测试流量时是两条 load 的快路径。 */
void bwtest_poll(void);

#endif /* NET_BWTEST_H */
