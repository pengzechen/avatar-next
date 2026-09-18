#ifndef CVITEK_ETH_H
#define CVITEK_ETH_H

/*
 * driver/eth/cvitek_eth.h — SG2002 / CV1812H 板载以太网驱动对外接口
 *
 * 硬件：Synopsys DWMAC 3.70a + 内部 EPHY，GMAC 基址来自 platform.conf 的
 *       eth.base（sg2002-riscv64 上是 0x04070000）。
 *
 * 驱动走**纯轮询**：不接 IRQ，由 net_poll_task → avatar_lwip_poll_rx → netdev_recv
 * 驱动收包。这与 netdev_t 抽象层（只有 send/recv，没有 poll/init 回调）一致。
 *
 * 编译条件：ARCH=riscv64 + ETH=cvitek（见 Makefile §6c）。
 */

#include "types.h"

/*
 * cvitek_eth_init_from_platform - 从平台配置取 GMAC 基址并初始化驱动
 *
 * 初始化顺序：时钟/复位 → DMA 软复位 → 描述符环 → MAC 配置 → PHY 自协商 →
 * 启动 DMA 收发 → 注册 netdev。
 *
 * 失败（platform.conf 无 eth.base 或分配失败）时只打日志并返回，不注册 netdev。
 */
void cvitek_eth_init_from_platform(void);

/*
 * cvitek_eth_send_probe - 发送一个 0x88b5 自定义 EtherType 帧
 *
 * 真机 bring-up 用：不依赖 lwIP 和 IP 配置，直接验证「时钟-复位-MDIO-PHY-DMA」
 * 整条通路。对面用 `tcpdump -i <if> 'ether proto 0x88b5'` 观察。
 * 未初始化时只打告警。
 */
void cvitek_eth_send_probe(void);

#endif /* CVITEK_ETH_H */
