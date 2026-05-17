#ifndef ETH_API_H
#define ETH_API_H

/*
 * include/eth_api.h — Rust 以太网驱动的 C 接口声明
 *
 * 由 rust/avatar_eth/src/lib.rs 实现，通过 #[no_mangle] extern "C" 导出。
 * 需要 ARCH=riscv64 + ETH=cvitek 编译选项。
 *
 * 硬件：SG2002 / CV1812H 板载 DWMAC 3.70a + 内部 EPHY
 *   GMAC 默认基地址：0x0407_0000
 */

#include "types.h"

/*
 * 以太网驱动句柄（Rust CvitekEthNic 的不透明指针）
 * C 侧不直接访问内部成员。
 */
typedef struct CvitekEthNicOpaque CvitekEthNic_t;

/*
 * eth_init - 初始化 cvitek 以太网驱动
 *
 * @base: GMAC MMIO 基地址（物理地址，默认 0x04070000）
 *
 * 返回：驱动句柄（Rust Box<CvitekEthNic> 裸指针），失败返回 NULL
 *
 * 初始化流程：时钟/复位 → DMA → MDIO → PHY 自协商 → 启动 TX/RX
 */
CvitekEthNic_t *eth_init(uint64_t base);

/*
 * eth_send - 发送一帧以太网数据
 *
 * @nic:  eth_init 返回的句柄
 * @data: 待发送帧数据（不含 FCS）
 * @len:  数据长度（字节）
 *
 * 返回：0 = 成功，-1 = TX 描述符忙（可重试），-2 = 其他错误
 */
int eth_send(CvitekEthNic_t *nic, const uint8_t *data, size_t len);

/*
 * eth_recv - 轮询接收一帧（非阻塞）
 *
 * @nic:    eth_init 返回的句柄
 * @buf:    接收缓冲区
 * @maxlen: 缓冲区最大长度
 *
 * 返回：收到的帧字节数（>0），无新数据返回 0，错误返回 -1
 */
int eth_recv(CvitekEthNic_t *nic, uint8_t *buf, size_t maxlen);

/*
 * eth_mac_addr - 读取设备 MAC 地址
 *
 * @nic: 驱动句柄
 * @mac: 输出缓冲区（6 字节）
 */
void eth_mac_addr(CvitekEthNic_t *nic, uint8_t mac[6]);

#endif /* ETH_API_H */
