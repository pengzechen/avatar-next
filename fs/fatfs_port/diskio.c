/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   fs/fatfs_port/diskio.c - FatFs 的块设备适配 + 挂载

   FatFs 和 lwext4 共用同一个 struct ext4_blockdev（MBR 分区只是不同的
   part_offset/part_size，bdif 是同一个）。FAT 这边只做 512 字节对齐的整扇区
   I/O，直接走 bdif->bread/bwrite，不会碰 lwext4 的 ph_bbuf；两个文件系统的
   所有 API 调用由 include/fs_lock.h 的全局锁串行化。

   编译要求：FATFS_CFLAGS（包含 lwext4 头 + FatFs 头 + fs/fatfs_port）。
*/
#include "types.h"
#include "klog.h"
#include "fs_lock.h"
#include "fatfs_port.h"

#include <ext4_blockdev.h>
#include <ext4_errno.h>

#include "ff.h"
#include "diskio.h"

#define FAT_SECTOR_SIZE 512u
/* SDHCI 的块计数是 16 位；一次 128 扇区也避免长时间占着全局 FS 锁。 */
#define FAT_IO_CHUNK 128u

static struct ext4_blockdev *g_fat_bdev;
static FATFS g_fatfs;
static bool g_block_inited;
static bool g_mounted;

static int fat_errno(FRESULT fr)
{
    switch (fr) {
    case FR_OK:
        return 0;
    case FR_NO_FILE:
    case FR_NO_PATH:
        return -2; /* ENOENT */
    case FR_EXIST:
        return -17; /* EEXIST */
    case FR_DENIED:
        return -13; /* EACCES */
    case FR_WRITE_PROTECTED:
        return -30; /* EROFS */
    case FR_INVALID_NAME:
        return -22; /* EINVAL */
    case FR_NOT_ENOUGH_CORE:
        return -12; /* ENOMEM */
    case FR_TOO_MANY_OPEN_FILES:
        return -24; /* EMFILE */
    case FR_INVALID_OBJECT:
        return -9; /* EBADF */
    case FR_NO_FILESYSTEM:
    case FR_NOT_READY:
    case FR_INVALID_DRIVE:
        return -19; /* ENODEV */
    default:
        return -5; /* EIO */
    }
}

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0 || !g_fat_bdev)
        return STA_NOINIT;
    return 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0 || !g_fat_bdev)
        return STA_NOINIT;
    if (!g_fat_bdev->bdif || !g_fat_bdev->bdif->bread || !g_fat_bdev->bdif->bwrite)
        return STA_NOINIT;
    if (g_fat_bdev->bdif->ph_bsize != FAT_SECTOR_SIZE)
        return STA_NOINIT;

    /* root ext4 已经先挂好，这里通常只是给共享 bdif 加一次引用。 */
    if (!g_block_inited) {
        if (ext4_block_init(g_fat_bdev) != EOK)
            return STA_NOINIT;
        g_block_inited = true;
    }
    return 0;
}

static DRESULT fat_transfer(BYTE pdrv, BYTE *buf, LBA_t sector, UINT count, bool write)
{
    if (pdrv != 0 || !g_fat_bdev || !g_fat_bdev->bdif || !buf)
        return RES_PARERR;
    if (g_fat_bdev->bdif->ph_bsize != FAT_SECTOR_SIZE)
        return RES_PARERR;
    if (g_fat_bdev->part_offset % FAT_SECTOR_SIZE)
        return RES_PARERR;
    if (count == 0)
        return RES_OK;

    uint64_t total = g_fat_bdev->part_size / FAT_SECTOR_SIZE;
    if ((uint64_t)sector + count > total)
        return RES_PARERR;

    uint64_t lba = g_fat_bdev->part_offset / FAT_SECTOR_SIZE + (uint64_t)sector;
    while (count) {
        uint32_t chunk = count > FAT_IO_CHUNK ? FAT_IO_CHUNK : count;
        int rc = write ? g_fat_bdev->bdif->bwrite(g_fat_bdev, buf, lba, chunk)
                       : g_fat_bdev->bdif->bread(g_fat_bdev, buf, lba, chunk);
        if (rc != EOK)
            return RES_ERROR;
        buf += (size_t)chunk * FAT_SECTOR_SIZE;
        lba += chunk;
        count -= chunk;
    }
    return RES_OK;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    return fat_transfer(pdrv, buff, sector, count, false);
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    return fat_transfer(pdrv, (BYTE *)buff, sector, count, true);
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0 || !g_fat_bdev)
        return RES_PARERR;

    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = (LBA_t)(g_fat_bdev->part_size / FAT_SECTOR_SIZE);
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = FAT_SECTOR_SIZE;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

int fatfs_mount(struct ext4_blockdev *part, const char *mount_point)
{
    if (!part || !part->bdif || !part->part_size)
        return -22; /* EINVAL */
    if (part->bdif->ph_bsize != FAT_SECTOR_SIZE || (part->part_offset % FAT_SECTOR_SIZE))
        return -22;
    /* FF_LBA64=0：LBA_t 是 32-bit，卷不能超过 2 TiB（512B 扇区） */
    if (part->part_size / FAT_SECTOR_SIZE > 0xFFFFFFFFu)
        return -22;

    g_fat_bdev = part;

    fs_global_lock();
    FRESULT fr = f_mount(&g_fatfs, "", 1);
    fs_global_unlock();

    if (fr != FR_OK) {
        int rc = fat_errno(fr);
        KLOG_ERROR("[fs] FAT mount at %s failed: FRESULT=%d\n", mount_point, (int)fr);
        g_fat_bdev = NULL;
        return rc;
    }

    g_mounted = true;
    KLOG_INFO("[fs] FAT volume mounted at %s (FatFs R0.15, LFN)\n", mount_point);
    return 0;
}

bool fatfs_is_mounted(void)
{
    return g_mounted;
}
