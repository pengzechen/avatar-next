/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/driver/net.h — 网络设备的平台配置

   数据接口沿用 include/net/netdev.h 的 netdev_t；驱动 probe 时调用
   netdev_register()。本头只描述硬件配置。
*/

#ifndef DRIVER_NET_H
#define DRIVER_NET_H

#include "types.h"

typedef struct {
    uintptr_t base; /* virtio-mmio 或 GMAC 基址 */
    uint32_t irq;   /* 0 = 纯轮询 */
    const char *compatible;
} net_cfg_t;

#endif /* DRIVER_NET_H */
