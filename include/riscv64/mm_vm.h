#ifndef RISCV64_MM_VM_H
#define RISCV64_MM_VM_H

#include "types.h"
#include "pmm.h"
#include "string.h"

#define RV_PAGE_SIZE       4096ULL
#define RV_PT_ENTRIES      512ULL

/* Sv39 PTE flags */
#define RV_PTE_V           (1ULL << 0)
#define RV_PTE_R           (1ULL << 1)
#define RV_PTE_W           (1ULL << 2)
#define RV_PTE_X           (1ULL << 3)
#define RV_PTE_U           (1ULL << 4)
#define RV_PTE_A           (1ULL << 6)
#define RV_PTE_D           (1ULL << 7)
#define RV_PTE_PPN_MASK    ((1ULL << 44) - 1ULL)

#if defined(PLATFORM_SG2002)
/* SG2002/CVITEK T-Head PTE memory attribute bits. */
#define RV_PTE_ATTR_NORMAL   ((7ULL << 60))                 /* CACHE | BUF | SHARE */
#define RV_PTE_ATTR_IOREMAP  ((1ULL << 63) | (1ULL << 60))  /* SO | SHARE */
#else
#define RV_PTE_ATTR_NORMAL   0ULL
#define RV_PTE_ATTR_IOREMAP  0ULL
#endif

/* 当前 RISC-V 内核运行在高地址（KERNEL_VMA 偏移），需通过 phys_to_virt 将物理地址转为内核可访问虚拟地址 */
static inline void *rv_pa_to_kva(uint64_t pa)
{
    return phys_to_virt(pa);
}

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

static inline uint64_t *rv_next_level(uint64_t pte)
{
    return (uint64_t *)rv_pa_to_kva(rv_pte_to_pa(pte));
}

/* walk 到 L0 PTE，alloc=true 时按需分配中间页表 */
static inline uint64_t *rv_walk_l0_pte(void *page_dir, uint64_t vaddr, bool alloc)
{
    uint64_t *l2 = (uint64_t *)page_dir;
    uint64_t idx2 = (vaddr >> 30) & 0x1FFULL;
    uint64_t idx1 = (vaddr >> 21) & 0x1FFULL;
    uint64_t idx0 = (vaddr >> 12) & 0x1FFULL;

    if ((l2[idx2] & RV_PTE_V) == 0) {
        if (!alloc) return NULL;
        uint64_t pa = pmm_alloc_pages(g_pmm, 1);
        if (pa == 0) return NULL;
        memset(rv_pa_to_kva(pa), 0, RV_PAGE_SIZE);
        l2[idx2] = rv_make_table_pte(pa);
    }
    if (rv_pte_is_leaf(l2[idx2])) return NULL;

    uint64_t *l1 = rv_next_level(l2[idx2]);
    if ((l1[idx1] & RV_PTE_V) == 0) {
        if (!alloc) return NULL;
        uint64_t pa = pmm_alloc_pages(g_pmm, 1);
        if (pa == 0) return NULL;
        memset(rv_pa_to_kva(pa), 0, RV_PAGE_SIZE);
        l1[idx1] = rv_make_table_pte(pa);
    }
    if (rv_pte_is_leaf(l1[idx1])) return NULL;

    uint64_t *l0 = rv_next_level(l1[idx1]);
    return &l0[idx0];
}

static inline uint64_t rv_perm_to_flags(uint64_t perm)
{
    /* 保持与现有 AArch64 语义一致：
     * perm=0: 普通用户页（RWX + U）
     * perm=1: 内核普通页（RWX）
     * perm=2: 设备页（RW, no X）
     */
    if (perm == 1) {
        return RV_PTE_R | RV_PTE_W | RV_PTE_X | RV_PTE_A | RV_PTE_D |
               RV_PTE_ATTR_NORMAL;
    }
    if (perm == 2) {
        return RV_PTE_R | RV_PTE_W | RV_PTE_A | RV_PTE_D |
               RV_PTE_ATTR_IOREMAP;
    }
    return RV_PTE_R | RV_PTE_W | RV_PTE_X | RV_PTE_U | RV_PTE_A | RV_PTE_D |
           RV_PTE_ATTR_NORMAL;
}

static inline uint64_t mm_vm_get_pte(void *page_dir, uint64_t vaddr)
{
    uint64_t *pte = rv_walk_l0_pte(page_dir, vaddr, false);
    if (pte == NULL)
        return 0;
    return *pte;
}

static inline int32_t mm_vm_map_pages(void *page_dir, uint64_t vaddr,
                                      uint64_t paddr, int32_t count,
                                      uint64_t perm)
{
    uint64_t pa = paddr;
    uint64_t flags = rv_perm_to_flags(perm);

    for (int32_t i = 0; i < count; i++) {
        uint64_t *pte = rv_walk_l0_pte(page_dir, vaddr, true);
        if (pte == NULL) return -1;
        if ((*pte & RV_PTE_V) != 0) return -1;
        *pte = ((pa >> 12) << 10) | flags | RV_PTE_V;
        vaddr += RV_PAGE_SIZE;
        pa += RV_PAGE_SIZE;
    }

    return 0;
}

static inline uint64_t mm_vm_get_paddr(void *page_dir, uint64_t vaddr)
{
    uint64_t *pte = rv_walk_l0_pte(page_dir, vaddr, false);
    if (pte == NULL || ((*pte & RV_PTE_V) == 0) || !rv_pte_is_leaf(*pte))
        return 0;
    return rv_pte_to_pa(*pte) + (vaddr & (RV_PAGE_SIZE - 1));
}

static inline int32_t mm_vm_copy_user_space(void *dst_pgd, void *src_pgd)
{
    /* 最小实现：线性扫描常用用户区 [0x1000, 0x80000000) 按页复制 */
    for (uint64_t va = 0x1000ULL; va < 0x80000000ULL; va += RV_PAGE_SIZE) {
        uint64_t src_pa = mm_vm_get_paddr(src_pgd, va);
        if (src_pa == 0)
            continue;

        uint64_t dst_pa = pmm_alloc_pages(g_pmm, 1);
        if (dst_pa == 0)
            return -1;

        memcpy(rv_pa_to_kva(dst_pa), rv_pa_to_kva(src_pa & ~(RV_PAGE_SIZE - 1)), RV_PAGE_SIZE);
        if (mm_vm_map_pages(dst_pgd, va, dst_pa, 1, 0) != 0) {
            pmm_free_pages(g_pmm, dst_pa, 1);
            return -1;
        }
    }
    return 0;
}

static inline void mm_vm_copy_to_uva(void *pgd, uint64_t user_vaddr,
                                     uint64_t src_paddr, uint64_t size)
{
    uint64_t src = src_paddr;
    uint64_t left = size;

    while (left > 0) {
        uint64_t dst_pa = mm_vm_get_paddr(pgd, user_vaddr);
        if (dst_pa == 0)
            return;

        uint64_t page_off = user_vaddr & (RV_PAGE_SIZE - 1);
        uint64_t chunk = RV_PAGE_SIZE - page_off;
        if (chunk > left)
            chunk = left;

        memcpy(rv_pa_to_kva(dst_pa), rv_pa_to_kva(src), chunk);

        src += chunk;
        user_vaddr += chunk;
        left -= chunk;
    }
}

#endif /* RISCV64_MM_VM_H */
