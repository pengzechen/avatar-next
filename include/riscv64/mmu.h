#ifndef RISCV64_MMU_H
#define RISCV64_MMU_H

#include "pte.h"
#include "types.h"

/* ── PTE 工具函数(定义在 pte.h；这里保留兼容包装)──────────────── */

static inline uint64_t rv_pte_to_pa(uint64_t pte)
{
    return RV_PTE_TO_PA(pte);
}

static inline uint64_t rv_make_table_pte(uint64_t pa)
{
    return RV_PTE_FROM_PA(pa, RV_PTE_V);
}

static inline bool rv_pte_is_leaf(uint64_t pte)
{
    return RV_PTE_IS_LEAF(pte);
}

/* ── MMU 初始化(在 mmu.S 中实现)───────────────────────────────── */

extern void mmu_init(void);
extern void mmu_disable(void);

#endif /* RISCV64_MMU_H */
