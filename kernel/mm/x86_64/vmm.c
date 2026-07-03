/*
 * kernel/mm/x86_64/vmm.c — x86_64 4 级页表操作实现
 *
 * 提供 PML4 → PDPT → PD → PT 四级页表的遍历、映射、复制等操作。
 */

#include "klog.h"
#include "mm_vm.h"
#include "pmm.h"
#include "string.h"
#include "types.h"

/* ── 页表遍历 ───────────────────────────────────────────────────── */

uint64_t *x86_walk_pt(void *page_dir, uint64_t vaddr, bool alloc) {
  uint64_t *pml4 = (uint64_t *)page_dir;
  uint64_t idx_pml4 = GET_PML4_INDEX(vaddr);
  uint64_t idx_pdpt = GET_PDPT_INDEX(vaddr);
  uint64_t idx_pd = GET_PD_INDEX(vaddr);
  uint64_t idx_pt = GET_PT_INDEX(vaddr);

  /* Level 4: PML4 → PDPT */
  if ((pml4[idx_pml4] & PTE_PRESENT) == 0) {
    if (!alloc)
      return NULL;
    uint64_t pa = pmm_alloc_pages(g_pmm, 1);
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
    uint64_t pa = pmm_alloc_pages(g_pmm, 1);
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
    uint64_t pa = pmm_alloc_pages(g_pmm, 1);
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

uint64_t x86_perm_to_flags(uint64_t perm) {
  if (perm == 1) {
    return PTE_PRESENT | PTE_WRITABLE | PTE_GLOBAL | PTE_NX | PTE_ACCESSED |
           PTE_DIRTY;
  }
  if (perm == 2) {
    return PTE_PRESENT | PTE_WRITABLE | PTE_PCD | PTE_PWT | PTE_NX |
           PTE_ACCESSED | PTE_DIRTY;
  }
  return PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_ACCESSED | PTE_DIRTY;
}

/* ── 页表映射 ───────────────────────────────────────────────────── */

int32_t mm_vm_map_pages(void *page_dir, uint64_t vaddr, uint64_t paddr,
                        int32_t count, uint64_t perm) {
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

uint64_t mm_vm_get_paddr(void *page_dir, uint64_t vaddr) {
  uint64_t *pte = x86_walk_pt(page_dir, vaddr, false);
  if (pte == NULL || (*pte & PTE_PRESENT) == 0)
    return 0;
  return x86_pte_to_pa(*pte) + GET_PAGE_OFFSET(vaddr);
}

/* ── 用户地址空间复制（递归遍历页表树）──────────────────────────── */

static int x86_copy_pt_recursive(uint64_t *src, uint64_t *dst, int level) {
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
      if (pte & PTE_NOFREE) {
        dst[i] = pte;
        continue;
      }
      uint64_t src_pa = x86_pte_to_pa(pte);
      uint64_t dst_pa = pmm_alloc_pages(g_pmm, 1);
      if (dst_pa == 0)
        return -1;
      memcpy(x86_pa_to_kva(dst_pa), x86_pa_to_kva(src_pa), PAGE_SIZE);
      dst[i] = (pte & ~PTE_ADDR_MASK) | (dst_pa & PTE_ADDR_MASK);
      continue;
    }

    uint64_t new_child = pmm_alloc_pages(g_pmm, 1);
    if (new_child == 0)
      return -1;
    memset(x86_pa_to_kva(new_child), 0, PAGE_SIZE);
    dst[i] = x86_make_table_pte(new_child);

    if (x86_copy_pt_recursive(x86_next_level(pte),
                              (uint64_t *)x86_pa_to_kva(new_child),
                              level - 1) != 0)
      return -1;
  }
  return 0;
}

int32_t mm_vm_copy_user_space(void *dst_pgd, void *src_pgd) {
  return x86_copy_pt_recursive((uint64_t *)src_pgd, (uint64_t *)dst_pgd, 4);
}

/* ── 数据拷贝到用户虚拟地址 ─────────────────────────────────────── */

void mm_vm_copy_to_uva(void *pgd, uint64_t user_vaddr, uint64_t src_paddr,
                       uint64_t size) {
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
