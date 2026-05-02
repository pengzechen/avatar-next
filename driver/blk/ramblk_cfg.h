/*
 * driver/blk/ramblk_cfg.h - RAM 块设备物理内存区域配置
 *
 * 为每个架构规划固定的 rootfs 区域（32MB），
 * 位于内核镜像之后的安全地址，并须在 PMM 中标记为已分配。
 *
 * 内存布局：
 *   AArch64  : RAM 0x40000000, 内核 @0x40080000, DTB @0x48000000
 *              rootfs @0x60000000 (+512MB)，避开 QEMU 放置的 DTB
 *   RISC-V64 : RAM 0x80000000, 内核 @0x80200000, rootfs @0x88000000 (+128MB)
 *   x86_64   : RAM 0x00100000, 内核 @0x00200000, rootfs @0x04000000 (+64MB)
 */
#ifndef __RAMBLK_CFG_H__
#define __RAMBLK_CFG_H__

#include "arch.h"

/* ── Rootfs 区域起始物理地址 ──────────────────────────────────── */
#if ARCH_AARCH64
    /* 0x60000000 = RAM_BASE(0x40000000) + 512MB
     * QEMU virt 机器将 DTB 放在 0x48000000 附近，需要跳过该区域 */
#   define RAMBLK_PHYS_BASE   0x60000000UL
#elif ARCH_RISCV64
#   define RAMBLK_PHYS_BASE   0x88000000UL
#elif ARCH_X86_64
#   define RAMBLK_PHYS_BASE   0x04000000UL
#else
#   error "Unsupported architecture for RAMBLK"
#endif

/* ── 区域大小与块参数 ─────────────────────────────────────────── */
#define RAMBLK_SIZE           (32UL * 1024UL * 1024UL)  /* 32 MB */
#define RAMBLK_SECTOR_SZ      512u
#define RAMBLK_SECTOR_CNT     (RAMBLK_SIZE / RAMBLK_SECTOR_SZ)  /* 65536 */

#define RAMBLK_PHYS_END       (RAMBLK_PHYS_BASE + RAMBLK_SIZE)

#endif /* __RAMBLK_CFG_H__ */
