#ifndef RISCV64_MMU_H
#define RISCV64_MMU_H

#include "types.h"

/* ── 页表常量 ───────────────────────────────────────────────────── */

#define RV_PAGE_SIZE       4096ULL
#define RV_PT_ENTRIES      512ULL

/* ── Sv39 PTE 标志位 ───────────────────────────────────────────── */

#define RV_PTE_V           (1ULL << 0)   /* Valid */
#define RV_PTE_R           (1ULL << 1)   /* Read */
#define RV_PTE_W           (1ULL << 2)   /* Write */
#define RV_PTE_X           (1ULL << 3)   /* Execute */
#define RV_PTE_U           (1ULL << 4)   /* User */
#define RV_PTE_A           (1ULL << 6)   /* Accessed */
#define RV_PTE_D           (1ULL << 7)   /* Dirty */
#define RV_PTE_NOFREE      (1ULL << 8)   /* RSW: leaf maps external memory */
#define RV_PTE_PPN_MASK    ((1ULL << 44) - 1ULL)

/* ── 平台特定 PTE 内存属性 ─────────────────────────────────────── */

#if defined(PLATFORM_SG2002)
#define RV_PTE_ATTR_NORMAL   ((7ULL << 60))
#define RV_PTE_ATTR_IOREMAP  ((1ULL << 63) | (1ULL << 60))
#else
#define RV_PTE_ATTR_NORMAL   0ULL
#define RV_PTE_ATTR_IOREMAP  0ULL
#endif

/* ── PTE 工具函数（纯位运算，无外部依赖）──────────────────────── */

static inline uint64_t rv_pte_to_pa(uint64_t pte)
{
    return ((pte >> 10) & RV_PTE_PPN_MASK) << 12;
}

static inline uint64_t rv_make_table_pte(uint64_t pa)
{
    return ((pa >> 12) << 10) | RV_PTE_V;
}

static inline bool rv_pte_is_leaf(uint64_t pte)
{
    return (pte & (RV_PTE_R | RV_PTE_W | RV_PTE_X)) != 0;
}

/* ── MMU 初始化（在 mmu.S 中实现）───────────────────────────────── */

extern void mmu_init(void);
extern void mmu_disable(void);

#endif /* RISCV64_MMU_H */
