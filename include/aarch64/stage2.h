/*
 * include/aarch64/stage2.h — AArch64 Stage-2 MMU（IPA→PA）API
 *
 * 移植自 ref/bare-vm/arch/aarch64/include/stage2.h，适配 Avatar OS。
 * 使用三级 LPAE 页表（VTCR SL0=1，T0SZ=32）进行 identity map。
 */
#ifndef AARCH64_STAGE2_H
#define AARCH64_STAGE2_H

#include "types.h"

/* ── Guest 物理内存默认布局（QEMU virt）────────────────────── */
#define GUEST_RAM_BASE   0x40000000ULL   /* 1 GiB（QEMU virt 默认 RAM 起始）*/
#define GUEST_RAM_SIZE   0x08000000ULL   /* 128 MiB                         */

/* ── LPAE 页表项格式 ─────────────────────────────────────────
 * Stage-2 三级页表: VTCR SL0=1 → L1 → L2 → L3
 * T0SZ=32 → IPA[31:0]: L1[1:0]=2bit, L2[8:0]=9bit, L3[8:0]=9bit
 * ─────────────────────────────────────────────────────────── */
#define LPAE_VALID       (1ULL << 0)
#define LPAE_TABLE       (1ULL << 1)   /* L1/L2: 指向下一级表       */
#define LPAE_PAGE        (3ULL << 0)   /* L3 page: bits[1:0]=11     */
#define LPAE_AF          (1ULL << 10)  /* Access Flag               */
#define LPAE_SH_IS       (3ULL << 8)   /* Inner Shareable           */
#define LPAE_MATTR_NORM  (0xFULL << 2) /* Normal WB cacheable       */
#define LPAE_MATTR_DEV   (0x1ULL << 2) /* Device-nGnRE              */
#define LPAE_XN          (1ULL << 54)  /* Execute-never             */
#define LPAE_S2AP_RW     (3ULL << 6)   /* Stage-2 AP: read/write    */
#define LPAE_S2AP_RO     (1ULL << 6)   /* Stage-2 AP: read-only     */

/* ── 页表层级尺寸 ─────────────────────────────────────────── */
/* T0SZ=32: IPA[31:30]=L1(4 entry), IPA[29:21]=L2(512 entry/L1) */
#define S2_L1_ENTRIES   4
#define S2_L2_ENTRIES   512
#define S2_L3_ENTRIES   512

/* ── VTCR_EL2 构造宏 ─────────────────────────────────────── */
#define VTCR_T0SZ(n)      ((n) & 0x3f)
#define VTCR_SL0(n)       (((n) & 0x3) << 6)
#define VTCR_IRGN0_WBWA   (1ULL << 8)
#define VTCR_ORGN0_WBWA   (1ULL << 10)
#define VTCR_SH0_IS       (3ULL << 12)
#define VTCR_TG0_4K       (0ULL << 14)
#define VTCR_PS_36BITS    (1ULL << 16)

/* ── VTTBR_EL2 VMID 编码 ────────────────────────────────── */
#define VTTBR_VMID_SHIFT  48

/* ── Stage-2 页表静态数组（BSS 中）──────────────────────── */
extern uint64_t s2_l1[S2_L1_ENTRIES]                __attribute__((aligned(4096)));
extern uint64_t s2_l2[S2_L1_ENTRIES][S2_L2_ENTRIES] __attribute__((aligned(4096)));
extern uint64_t s2_l3_ro[S2_L3_ENTRIES]             __attribute__((aligned(4096)));

/* ── API ────────────────────────────────────────────────── */

/*
 * stage2_init — 建立 Stage-2 identity map，写入 VTCR_EL2 / VTTBR_EL2
 * @mem_base: guest 物理内存起始 IPA
 * @mem_size: guest 物理内存大小
 */
void stage2_init(uint64_t mem_base, uint64_t mem_size);

/* 精细 4KB 权限控制（用于 Stage-2 权限故障测试）*/
void      stage2_set_ro(uint64_t ipa);       /* 设为只读 */
void      stage2_restore(uint64_t ipa);      /* 恢复读写 */
uint64_t *stage2_get_l3entry(uint64_t ipa);  /* 获取 L3 entry 指针 */

#endif /* AARCH64_STAGE2_H */
