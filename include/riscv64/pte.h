/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef RISCV64_PTE_H
#define RISCV64_PTE_H

/*
   include/riscv64/pte.h — RISC-V Sv39/Sv39x4 页表项与几何的唯一定义

   本文件同时被 C 和 .S 使用, 所以只能放 #define, 不能放 static inline,
   结构体或任何需要 C 语法的东西。

   历史问题：同一套 PTE 位/PPN 打包/Sv39 下标曾在 mmu.h, vmm.c, gstage.c,
   hext_run.c, mmu.S 各写一遍。任何一处改错(尤其是 PPN 右移 2 位)
   都只在运行期以"翻译到错误的物理页"这种形式暴露。
*/

#include "page.h" /* PAGE_SIZE / PAGE_SHIFT / PAGE_MASK */

/*
   下面这些常量会被 mmu.S / boot.S 直接展开, 而 GNU as 不认识 C 的
   `UL` 后缀。统一写成定宽十六进制字面量：C 里按值自动选类型,
   汇编里 gas 当作 64 位常量处理。函数式宏(RV_PTE_TO_PA 等)
   只会在 C 里展开, 可以继续用移位。
*/

/* ── PTE 标志位 ───────────────────────────────────────────────── */

#define RV_PTE_V      0x0000000000000001 /* Valid */
#define RV_PTE_R      0x0000000000000002 /* Read */
#define RV_PTE_W      0x0000000000000004 /* Write */
#define RV_PTE_X      0x0000000000000008 /* Execute */
#define RV_PTE_U      0x0000000000000010 /* User */
#define RV_PTE_A      0x0000000000000040 /* Accessed */
#define RV_PTE_D      0x0000000000000080 /* Dirty */
#define RV_PTE_NOFREE 0x0000000000000100 /* RSW: shared page, don't free on unmap */
#define RV_PTE_KEEP   0x0000000000000200 /* RSW: device buffer, unmap only */

/* ── PTE PPN 编码 ─────────────────────────────────────────────── */

/*
   Sv39/Sv39x4 的 PPN 在 pte[53:10], 不是 pte[47:12]。
   因此 PTE → PA 必须先右移 10, 再左移 12；不能像 ARM LPAE 那样
   直接 `pte & ~0xFFF`(那得到的是 pa >> 2)。
*/
#define RV_PTE_PPN_SHIFT 10
#define RV_PTE_PPN_MASK  0x00000FFFFFFFFFFF

#define RV_PTE_TO_PA(e) ((((e) >> RV_PTE_PPN_SHIFT) & RV_PTE_PPN_MASK) << PAGE_SHIFT)

#define RV_PTE_FROM_PA(pa, flags) ((((pa) >> PAGE_SHIFT) << RV_PTE_PPN_SHIFT) | (flags))

#define RV_PTE_IS_LEAF(e) (((e) & (RV_PTE_R | RV_PTE_W | RV_PTE_X)) != 0)

/* 1GiB 内核叶子(mmu.S 使用)：V|R|W|X|A|D */
#define RV_PTE_LEAF_RWX 0x00000000000000CF

/* ── Sv39 页表几何 ───────────────────────────────────────────── */

#define RV_PT_ENTRIES       (PAGE_SIZE / 8) /* 512 */
#define RV_PAGE_OFFSET(v)   ((v) & (PAGE_SIZE - 1))
#define RV_PAGE_OFFSET_MASK (PAGE_SIZE - 1)

/* htval 保存的是出错 GPA >> 2(低两位丢失, 需从 stval 取页内偏移) */
#define RV_GSTAGE_HTVAL_SHIFT 2

/* VPN[level]：level 0=4KiB, 1=2MiB, 2=1GiB */
#define RV_VPN(v, level)       (((v) >> (PAGE_SHIFT + 9 * (level))) & (RV_PT_ENTRIES - 1))
#define RV_PTE_INDEX(v, level) RV_VPN(v, level)

#define RV_LEVEL_SHIFT(level) (PAGE_SHIFT + 9 * (level))
#define RV_LEVEL_SIZE(level)  (1UL << RV_LEVEL_SHIFT(level))
#define RV_LEVEL_MASK(level)  (RV_LEVEL_SIZE(level) - 1)

/* ── satp / hgatp 模式与字段 ─────────────────────────────────── */

/* 模式 8 位于 bit63, 两种寄存器编码相同 */
#define RV_SATP_MODE_SV39    0x8000000000000000
#define RV_HGATP_MODE_SV39X4 0x8000000000000000

#define RV_SATP_MODE_SV39_NUM 8
#define RV_SATP_MODE_SV48_NUM 9
#define RV_SATP_MODE_SV57_NUM 10

#define RV_SATP_PPN_MASK    0x00000FFFFFFFFFFF
#define RV_HGATP_PPN_MASK   0x00000FFFFFFFFFFF
#define RV_HGATP_VMID_SHIFT 44

/* ── 平台 PTE 内存属性(SG2002 / CVITEK)─────────────────────── */

#if defined(PLATFORM_SG2002)
/* 7<<60 = CACHE|BUF|SHARE；bit63|bit60 = SO|SHARE */
#define RV_PTE_ATTR_NORMAL  0x7000000000000000
#define RV_PTE_ATTR_IOREMAP 0x9000000000000000
#else
#define RV_PTE_ATTR_NORMAL  0
#define RV_PTE_ATTR_IOREMAP 0
#endif

#ifndef __ASSEMBLER__
_Static_assert(RV_PT_ENTRIES == 512, "Sv39 每级固定 512 项");
_Static_assert(RV_LEVEL_SIZE(0) == PAGE_SIZE, "level 0 必须是 4KiB");
_Static_assert(RV_LEVEL_SIZE(1) == 2UL * 1024 * 1024, "level 1 必须是 2MiB");
_Static_assert(RV_LEVEL_SIZE(2) == 1UL << 30, "level 2 必须是 1GiB");
_Static_assert(RV_PTE_TO_PA(RV_PTE_FROM_PA(0x12345000ULL, 0)) == 0x12345000ULL, "PTE PPN 打包/解包必须可逆");
#endif

#endif /* RISCV64_PTE_H */
