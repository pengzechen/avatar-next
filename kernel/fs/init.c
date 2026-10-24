/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/fs/init.c - 根文件系统与 MBR 分区初始化

   这是**文件系统总入口**，不属于 lwext4 移植层：它把块设备注册到 lwext4，
   把 ext4 分区挂到 "/"；如果磁盘是 MBR 分区表，再把第一个 FAT 分区挂到
   FAT_MOUNT_POINT（默认 /boot）。

   根块设备由驱动框架提供：ramblk/sdblk 在 DRV_PHASE_STORAGE 注册自己，
   fs_init() 只问 block_root_device()，不再按平台/驱动 #if 选择。

   MBR 解析必须自己做，不能复用 lwext4 的 ext4_mbr_scan()：它只保留
   0x83（Linux native），FAT 分区会被直接丢弃（part_size 留 0）。

   编译要求：FATFS_CFLAGS（同时包含 lwext4 与 FatFs 头文件）。
*/

/*
   vfs.h 必须先包含：它会把 kernel_stat.h 带进来，而 ext4_misc.h 定义了
   __unused 宏（__attribute__((unused))），之后 kernel_stat.h 的
   `int64_t __unused[3]` 会被宏展开成语法错误。
*/
#include "vfs.h"
#include <ext4.h>
#include <ext4_blockdev.h>
#include <ext4_errno.h>
#include <ext4_config.h>
#include <ext4_misc.h>
#include <ext4_super.h>
#include <ext4_types.h>
#include "klog.h"
#include "string.h"
#include "fs_init.h"
#include "fatfs_port.h"
#include "driver/block.h"

#define ROOTFS_MP     "/"
#define BDEV_NAME_RAM "ramblk0"
#define BDEV_NAME_MBR "root"

#define FS_ENOENT 2
#define FS_EIO    5
#define FS_ENODEV 19
#define FS_EINVAL 22

#define MBR_MAX_PARTS   4
#define MBR_SECTOR_SIZE 512u
#define MBR_SIGNATURE   0xAA55u

/* MBR 分区项（LBA 单位固定是 512 字节扇区） */
struct mbr_part {
    uint8_t type;
    uint32_t first_lba;
    uint32_t sectors;
};

struct mbr_table {
    bool valid;
    struct mbr_part parts[MBR_MAX_PARTS];
};

/* 分区块设备在启动后一直要用，放静态区 */
static struct ext4_blockdev g_part_bdevs[MBR_MAX_PARTS];

static uint32_t mbr_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool mbr_type_is_ext4(uint8_t type)
{
    return type == 0x83;
}

static bool mbr_type_is_fat(uint8_t type)
{
    switch (type) {
    case 0x01: /* FAT12 */
    case 0x04: /* FAT16 < 32M */
    case 0x06: /* FAT16 */
    case 0x0B: /* FAT32 */
    case 0x0C: /* FAT32 LBA */
    case 0x0E: /* FAT16 LBA */
    case 0xEF: /* EFI system partition (FAT) */
        return true;
    default:
        return false;
    }
}

/*
   mbr_read - 读扇区 0 并解析 4 个主分区

   返回 0 表示读取成功（out->valid 表示是否找到了可用的 MBR 分区）；
   负值表示设备错误。没有 MBR 签名时 out->valid = false，调用方按整盘
   ext4 处理（QEMU/ramblk 的旧镜像就是这种）。

   判据刻意收紧：只有至少一条“类型已知 + LBA/长度在设备范围内”的分区
   才算 MBR。裸 FAT 镜像的引导扇区同样有 0x55AA，不能只看签名。
*/
static int mbr_read(struct ext4_blockdev *disk, struct mbr_table *out)
{
    memset(out, 0, sizeof(*out));

    if (!disk || !disk->bdif)
        return -FS_ENODEV;
    if (disk->bdif->ph_bsize != MBR_SECTOR_SIZE) {
        KLOG_ERROR("[fs] unsupported physical sector size %u (need 512)\n", disk->bdif->ph_bsize);
        return -FS_EINVAL;
    }

    /* 确保 sdblk 的 ph_bcnt/part_size 已经由 open 回调填好 */
    int rc = ext4_block_init(disk);
    if (rc != EOK) {
        KLOG_ERROR("[fs] block device init failed: %d\n", rc);
        return -FS_EIO;
    }

    uint8_t sec[MBR_SECTOR_SIZE];
    rc = disk->bdif->bread(disk, sec, 0, 1);
    ext4_block_fini(disk);
    if (rc != EOK) {
        KLOG_ERROR("[fs] failed to read MBR sector: %d\n", rc);
        return -FS_EIO;
    }

    uint16_t signature = (uint16_t)sec[510] | ((uint16_t)sec[511] << 8);
    if (signature != MBR_SIGNATURE)
        return 0; /* 不是 MBR */

    bool any = false;
    for (int i = 0; i < MBR_MAX_PARTS; i++) {
        const uint8_t *e = sec + 446 + i * 16;
        uint8_t type = e[4];
        uint32_t first_lba = mbr_le32(e + 8);
        uint32_t sectors = mbr_le32(e + 12);

        if (sectors == 0 || first_lba == 0)
            continue;

        if ((uint64_t)first_lba + sectors > disk->bdif->ph_bcnt) {
            KLOG_WARN("[fs] MBR p%d out of range: lba=%u sectors=%u (disk %llu)\n", i + 1, first_lba, sectors,
                      (unsigned long long)disk->bdif->ph_bcnt);
            continue;
        }

        if (type == 0xEE) {
            KLOG_WARN("[fs] GPT protective MBR is not supported (p%d)\n", i + 1);
            continue;
        }
        if (type == 0x05 || type == 0x0F) {
            KLOG_WARN("[fs] extended partitions are not supported (p%d)\n", i + 1);
            continue;
        }
        if (!mbr_type_is_ext4(type) && !mbr_type_is_fat(type)) {
            KLOG_FS("[fs] MBR p%d type 0x%02x ignored\n", i + 1, type);
            continue;
        }

        out->parts[i].type = type;
        out->parts[i].first_lba = first_lba;
        out->parts[i].sectors = sectors;
        any = true;

        KLOG_FS("[fs] MBR p%d: type=0x%02x offset=%llu MiB size=%llu MiB\n", i + 1, type,
                (unsigned long long)((uint64_t)first_lba * MBR_SECTOR_SIZE >> 20),
                (unsigned long long)((uint64_t)sectors * MBR_SECTOR_SIZE >> 20));
    }

    out->valid = any;
    return 0;
}

static void mbr_make_bdev(struct ext4_blockdev *dst, struct ext4_blockdev *parent, const struct mbr_part *part)
{
    memset(dst, 0, sizeof(*dst));
    dst->bdif = parent->bdif;
    dst->part_offset = (uint64_t)part->first_lba * MBR_SECTOR_SIZE;
    dst->part_size = (uint64_t)part->sectors * MBR_SECTOR_SIZE;
}

static void fs_log_ext4_features(struct ext4_blockdev *bdev)
{
    struct ext4_sblock sb;
    int rc = ext4_block_init(bdev);
    if (rc != EOK) {
        KLOG_ERROR("[fs] ext4_block_init for diagnostics failed: %d\n", rc);
        return;
    }

    rc = ext4_sb_read(bdev, &sb);
    if (rc != EOK) {
        KLOG_ERROR("[fs] ext4_sb_read for diagnostics failed: %d\n", rc);
        ext4_block_fini(bdev);
        return;
    }

    uint32_t fcom = ext4_get32(&sb, features_compatible);
    uint32_t fincom = ext4_get32(&sb, features_incompatible);
    uint32_t fro = ext4_get32(&sb, features_read_only);
    /* 成功读到了超级块 —— 这是"生效配置", 不是错误。 */
    KLOG_INFO("[fs] superblock features: compat=0x%08x incompat=0x%08x ro=0x%08x\n", fcom, fincom, fro);

    /*
       只在确实存在不支持的位时才 WARN：非零意味着我们在静默忽略磁盘上的
       语义, 那是真该被看见的；全零是正常情况, 不该占 WARN 的注意力。
    */
    uint32_t unsup_incom = fincom & ~CONFIG_SUPPORTED_FINCOM;
    uint32_t unsup_ro = fro & ~CONFIG_SUPPORTED_FRO_COM;
    if (unsup_incom || unsup_ro) {
        KLOG_WARN("[fs] unsupported features: incompat=0x%08x ro=0x%08x\n", unsup_incom, unsup_ro);
    } else {
        KLOG_DEBUG("[fs] superblock: no unsupported feature bits\n");
    }

    ext4_block_fini(bdev);
}

static void fs_dump_root_dir(void)
{
    ext4_dir dir;
    int rc = ext4_dir_open(&dir, ROOTFS_MP);
    if (rc != EOK)
        return;

    const ext4_direntry *de;
    /* 目录项数量由镜像内容决定 —— 逐项一行, 只能采样, 不能留在 INFO */
    KLOG_MODULE_DEBUG(LOG_MODULE_FS, "[fs] Root directory entries:\n");
    while ((de = ext4_dir_entry_next(&dir)) != NULL)
        KLOG_MODULE_DEBUG_SAMPLE(LOG_MODULE_FS, "[fs]   %s\n", de->name);
    ext4_dir_close(&dir);
}

static int fs_mount_bdev(struct ext4_blockdev *bdev, const char *name, int partitioned)
{
    int rc;

    KLOG_FS("[fs] Registering block device '%s'...\n", name);
    rc = ext4_device_register(bdev, name);
    if (rc != EOK) {
        KLOG_ERROR("[fs] ext4_device_register failed: %d\n", rc);
        return -rc;
    }

    KLOG_FS("[fs] Mounting '%s' at '%s'...\n", name, ROOTFS_MP);
    rc = ext4_mount(name, ROOTFS_MP, false);
    if (rc != EOK) {
        KLOG_ERROR("[fs] ext4_mount failed: %d\n", rc);
        if (partitioned) {
            fs_log_ext4_features(bdev);
        } else {
            KLOG_ERROR("[fs] Make sure rootfs.img was loaded by QEMU at the correct address.\n");
            KLOG_ERROR("[fs] Run: make ARCH=... run-fs\n");
        }
        ext4_device_unregister(name);
        return -rc;
    }

    KLOG_INFO("[fs] Root filesystem mounted successfully at '%s'\n", ROOTFS_MP);
    fs_dump_root_dir();
    return 0;
}

static int fs_init_mbr(struct ext4_blockdev *raw, const struct mbr_table *tbl)
{
    struct ext4_blockdev *root = NULL;
    struct ext4_blockdev *fat = NULL;
    int root_idx = -1;
    int fat_idx = -1;

    for (int i = 0; i < MBR_MAX_PARTS; i++) {
        const struct mbr_part *p = &tbl->parts[i];
        if (p->sectors == 0)
            continue;

        mbr_make_bdev(&g_part_bdevs[i], raw, p);

        if (mbr_type_is_ext4(p->type)) {
            if (!root) {
                root = &g_part_bdevs[i];
                root_idx = i;
            } else {
                KLOG_FS("[fs] extra ext4 partition p%d ignored (root is p%d)\n", i + 1, root_idx + 1);
            }
        } else if (mbr_type_is_fat(p->type)) {
            if (!fat) {
                fat = &g_part_bdevs[i];
                fat_idx = i;
            } else {
                KLOG_FS("[fs] extra FAT partition p%d ignored (using p%d)\n", i + 1, fat_idx + 1);
            }
        }
    }

    if (!root) {
        KLOG_ERROR("[fs] no ext4 (type 0x83) partition found in MBR\n");
        return -FS_ENODEV;
    }

    KLOG_INFO("[fs] root: MBR p%d (ext4, %llu MiB)\n", root_idx + 1, (unsigned long long)(root->part_size >> 20));
    int rc = fs_mount_bdev(root, BDEV_NAME_MBR, 1);
    if (rc < 0)
        return rc;

    if (!fat) {
        KLOG_WARN("[fs] no FAT partition in MBR; %s stays on ext4\n", FAT_MOUNT_POINT);
        return 0;
    }

    KLOG_INFO("[fs] FAT: MBR p%d (%llu MiB)\n", fat_idx + 1, (unsigned long long)(fat->part_size >> 20));
    rc = fatfs_mount(fat, FAT_MOUNT_POINT);
    if (rc < 0) {
        KLOG_WARN("[fs] FAT partition p%d exists but mount failed (%d); %s stays on ext4\n", fat_idx + 1, rc,
                  FAT_MOUNT_POINT);
        return 0;
    }

    rc = vfs_register_fat_mount(FAT_MOUNT_POINT);
    if (rc < 0) {
        KLOG_ERROR("[fs] FAT mounted but VFS registration at %s failed: %d\n", FAT_MOUNT_POINT, rc);
        return 0; /* root 仍然可用 */
    }

    KLOG_INFO("[fs] FAT partition mounted at %s\n", FAT_MOUNT_POINT);
    return 0;
}

int fs_init(void)
{
    struct ext4_blockdev *raw = block_root_device();

    if (!raw) {
        KLOG_ERROR("[fs] no root block device registered\n");
        return -FS_ENODEV;
    }

    struct mbr_table tbl;
    int rc = mbr_read(raw, &tbl);
    if (rc < 0)
        return rc;

    if (tbl.valid)
        return fs_init_mbr(raw, &tbl);

    /* 没有 MBR：ramblk 平台保持“整盘 ext4”的旧行为，sdblk 则视为错误 */
    if (block_root_needs_mbr()) {
        KLOG_ERROR("[fs] this platform expects an MBR partition table, but none was found\n");
        return -FS_ENOENT;
    }

    return fs_mount_bdev(raw, BDEV_NAME_RAM, 0);
}
