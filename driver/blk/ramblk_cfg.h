/*
 * driver/blk/ramblk_cfg.h - RAM 块设备物理内存区域配置
 *
 * rootfs 地址来自运行时 platform_cfg.h（由 platform_conf_scan() 从 Lua 提取）。
 * RAMBLK 使用与 rootfs 相同的固定物理窗口，并在 PMM 初始化时预留。
 */
#ifndef __RAMBLK_CFG_H__
#define __RAMBLK_CFG_H__

#include "platform_cfg.h"

/* ── Rootfs / RAMBLK 物理地址窗口 ─────────────────────────────── */
#define RAMBLK_PHYS_BASE      g_mem_rootfs_base
#define RAMBLK_SIZE           g_mem_rootfs_size

/* ── 区域大小与块参数 ─────────────────────────────────────────── */
#define RAMBLK_SECTOR_SZ      512u

/* PMM 标记函数使用闭区间，因此 END 为最后一个字节地址。 */
#define RAMBLK_PHYS_END_EXCL  (RAMBLK_PHYS_BASE + RAMBLK_SIZE)
#define RAMBLK_PHYS_END       (RAMBLK_PHYS_END_EXCL - 1UL)

#endif /* __RAMBLK_CFG_H__ */
