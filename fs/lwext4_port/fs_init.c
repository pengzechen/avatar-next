/*
 * fs/lwext4_port/fs_init.c - 根文件系统初始化
 *
 * 将块设备注册到 lwext4，挂载 ext4 分区到 "/"。
 * - DRIVER_SDBLK_SG2002: 从 SD 卡读取 MBR，挂载第 2 分区
 * - 默认: 使用 RAM 块设备（QEMU -device loader 预加载的 ext4 镜像）
 *
 * 编译要求：使用 LWEXT4_CFLAGS（包含 third_party/lwext4/include 和 lwext4_port/libc_shim）。
 */

#include <ext4.h>
#include <ext4_blockdev.h>
#include <ext4_errno.h>
#include <ext4_config.h>
#include <ext4_misc.h>
#include <ext4_super.h>
#include <ext4_types.h>
#include "klog.h"
#include "fs_init.h"

#if DRIVER_SDBLK_SG2002
#include <ext4_mbr.h>

struct ext4_blockdev;
extern struct ext4_blockdev *sdblk_get_bdev(void);

#define BDEV_NAME   "sdblk0p2"
#define ROOTFS_MP   "/"

static struct ext4_mbr_bdevs g_mbr_bdevs;

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
    KLOG_ERROR("[fs] superblock features: compat=0x%08x incompat=0x%08x ro=0x%08x\n",
               fcom, fincom, fro);
    KLOG_ERROR("[fs] unsupported features: incompat=0x%08x ro=0x%08x\n",
               fincom & ~CONFIG_SUPPORTED_FINCOM,
               fro & ~CONFIG_SUPPORTED_FRO_COM);

    ext4_block_fini(bdev);
}

int fs_init(void)
{
    int rc;
    struct ext4_blockdev *raw = sdblk_get_bdev();

    KLOG_INFO("[fs] Scanning MBR on SD card...\n");
    rc = ext4_mbr_scan(raw, &g_mbr_bdevs);
    if (rc != EOK) {
        KLOG_ERROR("[fs] ext4_mbr_scan failed: %d\n", rc);
        return -rc;
    }

    struct ext4_blockdev *part = &g_mbr_bdevs.partitions[1];
    if (part->part_size == 0) {
        KLOG_ERROR("[fs] SD partition 2 not found in MBR\n");
        return -ENODEV;
    }
    KLOG_INFO("[fs] Found partition 2: offset=%llu size=%llu MiB\n",
              (unsigned long long)part->part_offset,
              (unsigned long long)(part->part_size >> 20));

    KLOG_INFO("[fs] Registering block device '%s'...\n", BDEV_NAME);
    rc = ext4_device_register(part, BDEV_NAME);
    if (rc != EOK) {
        KLOG_ERROR("[fs] ext4_device_register failed: %d\n", rc);
        return -rc;
    }

    KLOG_INFO("[fs] Mounting '%s' at '%s'...\n", BDEV_NAME, ROOTFS_MP);
    rc = ext4_mount(BDEV_NAME, ROOTFS_MP, false);
    if (rc != EOK) {
        KLOG_ERROR("[fs] ext4_mount failed: %d\n", rc);
        fs_log_ext4_features(part);
        ext4_device_unregister(BDEV_NAME);
        return -rc;
    }

    KLOG_INFO("[fs] Root filesystem mounted successfully at '%s'\n", ROOTFS_MP);

    ext4_dir dir;
    rc = ext4_dir_open(&dir, ROOTFS_MP);
    if (rc == EOK) {
        const ext4_direntry *de;
        KLOG_INFO("[fs] Root directory entries:\n");
        while ((de = ext4_dir_entry_next(&dir)) != NULL)
            KLOG_INFO("[fs]   %s\n", de->name);
        ext4_dir_close(&dir);
    }

    return 0;
}

#else /* ramblk path */

struct ext4_blockdev;
extern struct ext4_blockdev *ramblk_get_bdev(void);

#define BDEV_NAME   "ramblk0"
#define ROOTFS_MP   "/"

int fs_init(void)
{
    int rc;

    KLOG_INFO("[fs] Registering block device '%s'...\n", BDEV_NAME);
    rc = ext4_device_register(ramblk_get_bdev(), BDEV_NAME);
    if (rc != EOK) {
        KLOG_ERROR("[fs] ext4_device_register failed: %d\n", rc);
        return -rc;
    }

    KLOG_INFO("[fs] Mounting '%s' at '%s'...\n", BDEV_NAME, ROOTFS_MP);
    rc = ext4_mount(BDEV_NAME, ROOTFS_MP, false);
    if (rc != EOK) {
        KLOG_ERROR("[fs] ext4_mount failed: %d\n", rc);
        KLOG_ERROR("[fs] Make sure rootfs.img was loaded by QEMU at the correct address.\n");
        KLOG_ERROR("[fs] Run: make ARCH=... run-fs\n");
        ext4_device_unregister(BDEV_NAME);
        return -rc;
    }

    KLOG_INFO("[fs] Root filesystem mounted successfully at '%s'\n", ROOTFS_MP);

    ext4_dir dir;
    rc = ext4_dir_open(&dir, ROOTFS_MP);
    if (rc == EOK) {
        const ext4_direntry *de;
        KLOG_INFO("[fs] Root directory entries:\n");
        while ((de = ext4_dir_entry_next(&dir)) != NULL)
            KLOG_INFO("[fs]   %s\n", de->name);
        ext4_dir_close(&dir);
    }

    return 0;
}

#endif /* DRIVER_SDBLK_SG2002 */
