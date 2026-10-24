/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   fs/fatfs_port/fatfs_port.h - FatFs port API

   这个头不包含 ff.h，普通内核代码（fs_init.c）可以直接包含。
*/
#ifndef FATFS_PORT_H
#define FATFS_PORT_H

#include "types.h"

struct ext4_blockdev;

/* 编译期可由 Makefile -DFAT_MOUNT_POINT=\"...\" 覆盖 */
#ifndef FAT_MOUNT_POINT
#define FAT_MOUNT_POINT "/boot"
#endif

/*
   fatfs_mount - 把 MBR 的 FAT 分区挂到 FatFs 的卷 0
   @part:       已设置 part_offset/part_size、共享父设备 bdif 的块设备
   @mount_point: 仅用于日志（VFS 挂载点由调用方注册）

   成功返回 0；失败返回负错误码，且不会影响已经挂好的 root ext4。
*/
int fatfs_mount(struct ext4_blockdev *part, const char *mount_point);

bool fatfs_is_mounted(void);

#endif /* FATFS_PORT_H */
