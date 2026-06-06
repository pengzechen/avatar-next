/*
 * fs/lwext4_port/fs_init.c - 根文件系统初始化
 *
 * 将 RAM 块设备注册到 lwext4，挂载 ext4 分区到 "/"。
 *
 * 编译要求：使用 LWEXT4_CFLAGS（包含 third_party/lwext4/include 和 lwext4_port/libc_shim）。
 */

#include <ext4.h>
#include <ext4_errno.h>
#include "klog.h"
#include "fs_init.h"

/* 声明 blk 驱动中定义的符号（避免包含带 lwext4 类型的头文件） */
struct ext4_blockdev;
#if DRIVER_SDBLK_SG2002
extern struct ext4_blockdev *sdblk_get_bdev(void);
#define fs_get_bdev()  sdblk_get_bdev()
#define BDEV_NAME      "sdblk0"
#else
extern struct ext4_blockdev *ramblk_get_bdev(void);
#define fs_get_bdev()  ramblk_get_bdev()
#define BDEV_NAME      "ramblk0"
#endif

/* 挂载点名称 */
#define ROOTFS_MP   "/"

int fs_init(void)
{
    int rc;

    KLOG_INFO("[fs] Registering block device '%s'...\n", BDEV_NAME);
    rc = ext4_device_register(fs_get_bdev(), BDEV_NAME);
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

    /* 简单验证：列出根目录 */
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
