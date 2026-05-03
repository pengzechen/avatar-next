/*
 * driver/blk/ramblk.c - 基于物理内存区域的 RAM 块设备
 *
 * 将固定的物理内存区域（由 QEMU -device loader 预加载 ext4 镜像）
 * 封装为 lwext4 的 ext4_blockdev 接口。
 *
 * 编译要求：使用 LWEXT4_CFLAGS（包含 fs/lwext4/include 和 fs/compat）。
 */

#include <ext4_blockdev.h>
#include <ext4_errno.h>
#include "blk/ramblk_cfg.h"
#include "klog.h"
#include "string.h"   /* memcpy */
#include "mm_vm.h"       /* phys_to_virt */

/* ── 内部 I/O 回调 ──────────────────────────────────────────────── */

static int ramblk_open(struct ext4_blockdev *bdev)
{
    (void)bdev;
    /* 设备是内存映射的，无需实际打开操作 */
    return EOK;
}

static int ramblk_bread(struct ext4_blockdev *bdev, void *buf,
                        uint64_t blk_id, uint32_t blk_cnt)
{
    uint32_t bsize = bdev->bdif->ph_bsize;
    uint8_t *src   = (uint8_t *)phys_to_virt(RAMBLK_PHYS_BASE)
                     + blk_id * bsize;
    memcpy(buf, src, (size_t)blk_cnt * bsize);
    return EOK;
}

static int ramblk_bwrite(struct ext4_blockdev *bdev, const void *buf,
                         uint64_t blk_id, uint32_t blk_cnt)
{
    uint32_t bsize = bdev->bdif->ph_bsize;
    uint8_t *dst   = (uint8_t *)phys_to_virt(RAMBLK_PHYS_BASE)
                     + blk_id * bsize;
    memcpy(dst, buf, (size_t)blk_cnt * bsize);
    return EOK;
}

static int ramblk_close(struct ext4_blockdev *bdev)
{
    (void)bdev;
    return EOK;
}

/* ── 静态块设备实例（由 lwext4 宏生成）─────────────────────────── */

EXT4_BLOCKDEV_STATIC_INSTANCE(
    g_ramblk,
    RAMBLK_SECTOR_SZ,
    RAMBLK_SECTOR_CNT,
    ramblk_open,
    ramblk_bread,
    ramblk_bwrite,
    ramblk_close,
    NULL,   /* lock   - 单核暂不需要 */
    NULL    /* unlock */
);

/* ── 公开接口 ───────────────────────────────────────────────────── */

struct ext4_blockdev *ramblk_get_bdev(void)
{
    return &g_ramblk;
}

void ramblk_init(void)
{
    KLOG_INFO("[ramblk] RAM block device:\n");
    KLOG_INFO("[ramblk]   phys base = 0x%llx\n", (uint64_t)RAMBLK_PHYS_BASE);
    KLOG_INFO("[ramblk]   size      = %llu MB\n", (uint64_t)(RAMBLK_SIZE >> 20));
    KLOG_INFO("[ramblk]   sectors   = %llu x %u bytes\n",
              (uint64_t)RAMBLK_SECTOR_CNT, RAMBLK_SECTOR_SZ);
}
