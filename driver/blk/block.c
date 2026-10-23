/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   driver/blk/block.c — 根块设备注册

   ramblk/sdblk 在 probe 时把自己的 ext4_blockdev 注册到这里；fs_init()
   只问 block_root_device()，不再按平台/驱动 #if 选择。

   SD 卡需要先扫描 MBR 取第 2 分区，ramblk 是直接可挂载的 ext4 镜像，
   这个差异用 block_root_needs_mbr() 表达。
*/

#include "driver/block.h"

static struct ext4_blockdev *g_root_bdev;
static bool g_root_needs_mbr;

static int block_set_root(struct ext4_blockdev *bdev, bool needs_mbr)
{
    if (!bdev)
        return -1;
    g_root_bdev = bdev;
    g_root_needs_mbr = needs_mbr;
    return 0;
}

int block_register_root(struct ext4_blockdev *bdev)
{
    return block_set_root(bdev, false);
}

int block_register_root_mbr(struct ext4_blockdev *bdev)
{
    return block_set_root(bdev, true);
}

struct ext4_blockdev *block_root_device(void)
{
    return g_root_bdev;
}

bool block_root_needs_mbr(void)
{
    return g_root_needs_mbr;
}
