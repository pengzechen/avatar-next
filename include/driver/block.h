/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/driver/block.h — 根块设备接口

   驱动 probe 时通过 block_register_root() 交出自己；fs_init() 只问
   block_root_device()，不再按平台 #if 选择 ramblk/sdblk。
*/

#ifndef DRIVER_BLOCK_H
#define DRIVER_BLOCK_H

#include "types.h"

struct ext4_blockdev;

typedef struct {
    uintptr_t base;    /* ramblk：rootfs 物理基址 */
    uint64_t size;     /* ramblk：rootfs 大小 */
    uintptr_t sd_base; /* sdblk：SDHCI 基址 */
    uintptr_t top_base;
    uint32_t pwrsw_off;
} block_cfg_t;

int block_register_root(struct ext4_blockdev *bdev);
int block_register_root_mbr(struct ext4_blockdev *bdev);
struct ext4_blockdev *block_root_device(void);
bool block_root_needs_mbr(void);

#endif /* DRIVER_BLOCK_H */
