/*
 * driver/blk/ramblk_cfg.h - RAM 块设备物理内存区域配置
 *
 * 内存布局来自 include/mem_layout.h（由 config/mem_layout.table 生成）。
 * RAMBLK 使用与 rootfs 相同的固定物理窗口，并在 PMM 初始化时预留。
 */
#ifndef __RAMBLK_CFG_H__
#define __RAMBLK_CFG_H__

#include "mem_layout.h"

/* ── Rootfs / RAMBLK 物理地址窗口 ─────────────────────────────── */
#define RAMBLK_PHYS_BASE      MEM_ROOTFS_BASE
#define RAMBLK_SIZE           MEM_ROOTFS_SIZE

/* ── 区域大小与块参数 ─────────────────────────────────────────── */
#define RAMBLK_SECTOR_SZ      512u
#define RAMBLK_SECTOR_CNT     (RAMBLK_SIZE / RAMBLK_SECTOR_SZ)  /* 65536 */

/* PMM 标记函数使用闭区间，因此 END 为最后一个字节地址。 */
#define RAMBLK_PHYS_END_EXCL  (RAMBLK_PHYS_BASE + RAMBLK_SIZE)
#define RAMBLK_PHYS_END       (RAMBLK_PHYS_END_EXCL - 1UL)

#endif /* __RAMBLK_CFG_H__ */
