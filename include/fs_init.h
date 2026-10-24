/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/fs_init.h - 文件系统初始化公开接口

   不依赖 lwext4 / FatFs 类型，普通内核代码（如 kernel/main.c）可直接包含。
*/
#ifndef __FS_INIT_H__
#define __FS_INIT_H__

/*
   fs_init - 初始化根文件系统

   - 解析 MBR（如果有）：ext4 分区挂 "/"，第一个 FAT 分区挂 FAT_MOUNT_POINT；
   - 没有 MBR：ramblk 按整盘 ext4 挂 "/"，sdblk 报错；
   - 依赖存储驱动（ramblk/sdblk）和 PMM 已完成初始化。

   成功返回 0，失败返回负错误码。
*/
int fs_init(void);

#endif /* __FS_INIT_H__ */
