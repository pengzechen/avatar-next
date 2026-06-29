/*
 * kernel/mm/riscv64/vmm.c — RISC-V Sv39 页表操作实现
 *
 * 提供 Sv39 三级页表（L2→L1→L0）的遍历、映射、复制等操作。
 */

#include "types.h"
#include "klog.h"
#include "pmm.h"
#include "mm_vm.h"
#include "string.h"
#include "riscv64/satp_utils.h"

/* ── 页表遍历 ───────────────────────────────────────────────────── */

uint64_t *
rv_walk_l0_pte(void *page_dir, uint64_t vaddr, bool alloc)
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

/* ── 权限转换 ───────────────────────────────────────────────────── */

uint64_t
rv_perm_to_flags(uint64_t perm)
{
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

/* ── PTE 查询 ───────────────────────────────────────────────────── */

uint64_t
mm_vm_get_pte(void *page_dir, uint64_t vaddr)
{
    uint64_t *pte = rv_walk_l0_pte(page_dir, vaddr, false);
    if (pte == NULL)
        return 0;
    return *pte;
}

/* ── 页表映射 ───────────────────────────────────────────────────── */

int32_t
mm_vm_map_pages(void *page_dir, uint64_t vaddr,
                uint64_t paddr, int32_t count, uint64_t perm)
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

/* ── 虚拟地址转物理地址 ─────────────────────────────────────────── */

uint64_t
mm_vm_get_paddr(void *page_dir, uint64_t vaddr)
{
    uint64_t *pte = rv_walk_l0_pte(page_dir, vaddr, false);
    if (pte == NULL || ((*pte & RV_PTE_V) == 0) || !rv_pte_is_leaf(*pte))
        return 0;
    return rv_pte_to_pa(*pte) + (vaddr & (RV_PAGE_SIZE - 1));
}

/* ── 用户地址空间复制（递归遍历页表树）──────────────────────────── */

static int
rv_copy_pt_recursive(uint64_t *src, uint64_t *dst, int level)
{
    uint32_t limit = (level == 2) ? RISCV64_KERNEL_L1_MMIO0_IDX
                                  : (uint32_t)RV_PT_ENTRIES;

    for (uint32_t i = 0; i < limit; i++) {
        uint64_t pte = src[i];
        if ((pte & RV_PTE_V) == 0)
            continue;

        if (rv_pte_is_leaf(pte)) {
            if (pte & RV_PTE_NOFREE) {
                dst[i] = pte;
                continue;
            }
            uint64_t src_pa = rv_pte_to_pa(pte);
            uint64_t dst_pa = pmm_alloc_pages(g_pmm, 1);
            if (dst_pa == 0)
                return -1;
            memcpy(rv_pa_to_kva(dst_pa), rv_pa_to_kva(src_pa), RV_PAGE_SIZE);
            dst[i] = (pte & ~(RV_PTE_PPN_MASK << 10)) |
                     ((dst_pa >> 12) << 10);
            continue;
        }

        uint64_t new_child = pmm_alloc_pages(g_pmm, 1);
        if (new_child == 0)
            return -1;
        memset(rv_pa_to_kva(new_child), 0, RV_PAGE_SIZE);
        dst[i] = rv_make_table_pte(new_child);

        if (rv_copy_pt_recursive(rv_next_level(pte),
                                 (uint64_t *)rv_pa_to_kva(new_child),
                                 level - 1) != 0)
            return -1;
    }
    return 0;
}

int32_t
mm_vm_copy_user_space(void *dst_pgd, void *src_pgd)
{
    return rv_copy_pt_recursive((uint64_t *)src_pgd,
                                (uint64_t *)dst_pgd, 2);
}

/* ── 数据拷贝到用户虚拟地址 ─────────────────────────────────────── */

void
mm_vm_copy_to_uva(void *pgd, uint64_t user_vaddr,
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
