#ifndef X86_64_MM_VM_H
#define X86_64_MM_VM_H

/*
 * x86_64 页表操作接口
 *
 * 本头文件由 mm_vm.h 包含，phys_to_virt/virt_to_phys 已在包含前定义。
 * 实现位于 kernel/mm/x86_64/vmm.c。
 */

#include "mmu.h"

/* ── 需要 phys_to_virt 的工具函数 ──────────────────────────────── */

static inline void *x86_pa_to_kva(uint64_t pa)
{
    return phys_to_virt(pa);
}

static inline uint64_t *x86_next_level(uint64_t pte)
{
    return (uint64_t *)x86_pa_to_kva(x86_pte_to_pa(pte));
}

/* ── 页表操作（实现在 kernel/mm/x86_64/vmm.c）──────────────────── */

extern uint64_t *x86_walk_pt(void *page_dir, uint64_t vaddr, bool alloc);
extern uint64_t  x86_perm_to_flags(uint64_t perm);
extern int32_t   mm_vm_map_pages(void *page_dir, uint64_t vaddr,
                                 uint64_t paddr, int32_t count, uint64_t perm);
extern uint64_t  mm_vm_get_paddr(void *page_dir, uint64_t vaddr);
extern int32_t   mm_vm_copy_user_space(void *dst_pgd, void *src_pgd);
extern void      mm_vm_copy_to_uva(void *pgd, uint64_t user_vaddr,
                                   uint64_t src_paddr, uint64_t size);

#endif /* X86_64_MM_VM_H */
