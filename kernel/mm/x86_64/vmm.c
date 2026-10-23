/*
   kernel/mm/x86_64/vmm.c — x86_64 4 级页表操作实现

   提供 PML4 → PDPT → PD → PT 四级页表的遍历, 映射, 复制等操作。
*/

#include "klog.h"
#include "mm_vm.h"
#include "pmm.h"
#include "string.h"
#include "types.h"
#include "shared_page.h"

/* ── 页表遍历 ───────────────────────────────────────────────────── */

uint64_t *x86_walk_pt(void *page_dir, uint64_t vaddr, bool alloc)
{
    uint64_t *pml4 = (uint64_t *)page_dir;
    uint64_t idx_pml4 = GET_PML4_INDEX(vaddr);
    uint64_t idx_pdpt = GET_PDPT_INDEX(vaddr);
    uint64_t idx_pd = GET_PD_INDEX(vaddr);
    uint64_t idx_pt = GET_PT_INDEX(vaddr);

    /* Level 4: PML4 → PDPT */
    if ((pml4[idx_pml4] & PTE_PRESENT) == 0) {
        if (!alloc)
            return NULL;
        uint64_t pa = pmm_alloc_pages(&g_pmm, 1);
        if (pa == 0)
            return NULL;
        memset(x86_pa_to_kva(pa), 0, PAGE_SIZE);
        pml4[idx_pml4] = x86_make_table_pte(pa);
    }
    if (x86_pte_is_leaf(pml4[idx_pml4], 3))
        return NULL;

    /* Level 3: PDPT → PD */
    uint64_t *pdpt = x86_next_level(pml4[idx_pml4]);
    if ((pdpt[idx_pdpt] & PTE_PRESENT) == 0) {
        if (!alloc)
            return NULL;
        uint64_t pa = pmm_alloc_pages(&g_pmm, 1);
        if (pa == 0)
            return NULL;
        memset(x86_pa_to_kva(pa), 0, PAGE_SIZE);
        pdpt[idx_pdpt] = x86_make_table_pte(pa);
    }
    if (x86_pte_is_leaf(pdpt[idx_pdpt], 2))
        return NULL;

    /* Level 2: PD → PT */
    uint64_t *pd = x86_next_level(pdpt[idx_pdpt]);
    if ((pd[idx_pd] & PTE_PRESENT) == 0) {
        if (!alloc)
            return NULL;
        uint64_t pa = pmm_alloc_pages(&g_pmm, 1);
        if (pa == 0)
            return NULL;
        memset(x86_pa_to_kva(pa), 0, PAGE_SIZE);
        pd[idx_pd] = x86_make_table_pte(pa);
    }
    if (x86_pte_is_leaf(pd[idx_pd], 1))
        return NULL;

    /* Level 1: PT → Page */
    uint64_t *pt = x86_next_level(pd[idx_pd]);
    return &pt[idx_pt];
}

/* ── 权限转换 ───────────────────────────────────────────────────── */

uint64_t x86_perm_to_flags(vm_perm_t perm)
{
    if (perm == VM_PERM_KERNEL) {
        return PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL | PTE_NX | PTE_ACCESSED | PTE_DIRTY;
    }
    if (perm == VM_PERM_KERNEL_MMIO) {
        return PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT | PTE_NX | PTE_ACCESSED | PTE_DIRTY;
    }
    return PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_ACCESSED | PTE_DIRTY;
}

/* ── 页表映射 ───────────────────────────────────────────────────── */

int32_t mm_vm_map_pages(void *page_dir, uint64_t vaddr, uint64_t paddr, int32_t count, vm_perm_t perm)
{
    uint64_t va = vaddr;
    uint64_t pa = paddr;
    uint64_t flags = x86_perm_to_flags(perm);

    for (int32_t i = 0; i < count; i++) {
        uint64_t *pte = x86_walk_pt(page_dir, va, true);
        if (pte == NULL)
            return -1;
        if ((*pte & PTE_PRESENT) != 0)
            return -1;
        *pte = (pa & PTE_ADDR_MASK) | flags;
        va += PAGE_SIZE;
        pa += PAGE_SIZE;
    }

    return 0;
}

/* ── 虚拟地址转物理地址 ─────────────────────────────────────────── */

uint64_t mm_vm_get_paddr(void *page_dir, uint64_t vaddr)
{
    uint64_t *pte = x86_walk_pt(page_dir, vaddr, false);
    if (pte == NULL || (*pte & PTE_PRESENT) == 0)
        return 0;
    return x86_pte_to_pa(*pte) + GET_PAGE_OFFSET(vaddr);
}

/* ── 用户地址空间复制(递归遍历页表树)──────────────────────────── */

static int x86_copy_pt_recursive(uint64_t *src, uint64_t *dst, int level)
{
    uint32_t limit = (level == 4) ? X86_PML4_KERNEL_START : 512;

    for (uint32_t i = 0; i < limit; i++) {
        uint64_t pte = src[i];
        if ((pte & PTE_PRESENT) == 0)
            continue;

        if (level > 1 && (pte & PTE_HUGE)) {
            dst[i] = pte;
            continue;
        }

        if (level == 1) {
            /* KEEP(设备缓冲)同样原样复制, 但不加引用 —— 页归句柄所有 */
            if (pte & (PTE_NOFREE | PTE_KEEP)) {
                dst[i] = pte;
                if ((pte & PTE_NOFREE) && !shared_page_ref(x86_pte_to_pa(pte)))
                    return -1;
                continue;
            }
            uint64_t src_pa = x86_pte_to_pa(pte);
            uint64_t dst_pa = pmm_alloc_pages(&g_pmm, 1);
            if (dst_pa == 0)
                return -1;
            memcpy(x86_pa_to_kva(dst_pa), x86_pa_to_kva(src_pa), PAGE_SIZE);
            dst[i] = (pte & ~PTE_ADDR_MASK) | (dst_pa & PTE_ADDR_MASK);
            continue;
        }

        uint64_t new_child = pmm_alloc_pages(&g_pmm, 1);
        if (new_child == 0)
            return -1;
        memset(x86_pa_to_kva(new_child), 0, PAGE_SIZE);
        dst[i] = x86_make_table_pte(new_child);

        if (x86_copy_pt_recursive(x86_next_level(pte), (uint64_t *)x86_pa_to_kva(new_child), level - 1) != 0)
            return -1;
    }
    return 0;
}

int32_t mm_vm_copy_user_space(void *dst_pgd, void *src_pgd)
{
    return x86_copy_pt_recursive((uint64_t *)src_pgd, (uint64_t *)dst_pgd, 4);
}

/* ── 数据拷贝到用户虚拟地址 ─────────────────────────────────────── */

void mm_vm_copy_to_uva(void *pgd, uint64_t user_vaddr, uint64_t src_paddr, uint64_t size)
{
    uint64_t src = src_paddr;
    uint64_t dst_va = user_vaddr;
    uint64_t left = size;

    while (left > 0) {
        uint64_t dst_pa = mm_vm_get_paddr(pgd, dst_va);
        if (dst_pa == 0)
            return;

        uint64_t page_offset = GET_PAGE_OFFSET(dst_va);
        uint64_t copy_size = PAGE_SIZE - page_offset;
        if (copy_size > left)
            copy_size = left;

        memcpy(x86_pa_to_kva(dst_pa), x86_pa_to_kva(src), copy_size);

        src += copy_size;
        dst_va += copy_size;
        left -= copy_size;
    }
}

/* ── 用户地址空间生命周期(架构无关层经 mm_vm.h 调用)────────────── */

extern uint64_t g_kernel_pgd_phys;

static void x86_destroy_table(uint64_t *table, int level)
{
    for (uint32_t i = 0; i < 512U; i++) {
        uint64_t pte = table[i];
        if ((pte & PTE_PRESENT) == 0)
            continue;

        if (level == 1 || (level > 1 && (pte & PTE_HUGE))) {
            if ((pte & PTE_KEEP) != 0) {
                /* 设备缓冲：页归 dma_buf/ION 句柄所有, 只拆表 */
            } else if ((pte & PTE_NOFREE) == 0) {
                pmm_free_pages(&g_pmm, x86_pte_to_pa(pte), 1);
            } else {
                shared_page_unref(x86_pte_to_pa(pte));
            }
            table[i] = 0;
            continue;
        }

        uint64_t child_pa = x86_pte_to_pa(pte);
        x86_destroy_table((uint64_t *)phys_to_virt(child_pa), level - 1);
        pmm_free_pages(&g_pmm, child_pa, 1);
        table[i] = 0;
    }
}

void mm_vm_init_user_pgd(void *pgd, uint64_t pgd_phys)
{
    if (g_kernel_pgd_phys == 0) {
        KLOG_ERROR("[vm_user] g_kernel_pgd_phys is not initialized\n");
        return;
    }

    uint64_t *kernel_pml4 = (uint64_t *)phys_to_virt(g_kernel_pgd_phys);
    x86_copy_kernel_mappings((uint64_t *)pgd, kernel_pml4);
    KLOG_DEBUG("[vm_user] x86_64 copied kernel PML4[%u..%u] to user PGD=0x%llx\n", X86_PML4_KERNEL_START,
               X86_PML4_ENTRIES - 1U, pgd_phys);
}

void mm_vm_destroy_user_space(uint64_t pgd_phys)
{
    if (pgd_phys == 0)
        return;

    uint64_t *pml4 = (uint64_t *)phys_to_virt(pgd_phys);
    for (uint32_t i = 0; i < X86_PML4_KERNEL_START; i++) {
        uint64_t pte = pml4[i];
        if ((pte & PTE_PRESENT) == 0)
            continue;
        uint64_t child_pa = x86_pte_to_pa(pte);
        x86_destroy_table((uint64_t *)phys_to_virt(child_pa), 3);
        pmm_free_pages(&g_pmm, child_pa, 1);
        pml4[i] = 0;
    }

    pmm_free_pages(&g_pmm, pgd_phys, 1);
}

int mm_vm_unmap_page(void *pgd, uint64_t vaddr)
{
    uint64_t *pte = x86_walk_pt(pgd, vaddr, false);
    if (!pte || ((*pte & PTE_PRESENT) == 0))
        return 0;

    if ((*pte & PTE_KEEP) != 0) {
        /* 设备缓冲：页归 dma_buf/ION 句柄所有, 只拆映射 */
    } else if ((*pte & PTE_NOFREE) == 0) {
        pmm_free_pages(&g_pmm, x86_pte_to_pa(*pte), 1);
    } else {
        shared_page_unref(x86_pte_to_pa(*pte));
    }
    *pte = 0;
    flush_tlb_single(vaddr);
    return 1;
}

void mm_vm_flush_tlb_user(void)
{
    /* mm_vm_unmap_page() 已经逐页 invlpg, 不需要额外的全刷。 */
}
