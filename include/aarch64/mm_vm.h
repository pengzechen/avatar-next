#ifndef AARCH64_MM_VM_H
#define AARCH64_MM_VM_H

#include "types.h"
#include "mmu.h"

/* AArch64 现有 VM 实现导出符号 */
extern int32_t memory_create_map(void *page_dir, uint64_t vaddr, uint64_t paddr,
                                 int32_t count, uint64_t perm);
extern uint64_t memory_get_paddr(void *page_dir, uint64_t vaddr);
extern void memory_free_page(void *page_dir, uint64_t addr);

extern int32_t memory_copy_uvm_4level(void *dst_pgd, void *src_pgd);
extern uint64_t memory_get_pte_raw(void *page_dir, uint64_t vaddr);
extern void memory_set_pte_nofree(void *page_dir, uint64_t vaddr);
extern void memory_set_pte_keep(void *page_dir, uint64_t vaddr);

/* copydata_to_uvm: 将 src_paddr 处的数据拷贝到 pgd 对应用户地址空间的 user_vaddr */
extern void copydata_to_uvm(void *page_dir, uint64_t vaddr, uint64_t paddr,
                            uint64_t size);

static inline int32_t mm_vm_map_pages(void *page_dir, uint64_t vaddr,
                                      uint64_t paddr, int32_t count,
                                      vm_perm_t perm)
{
    return memory_create_map(page_dir, vaddr, paddr, count, perm);
}

static inline uint64_t mm_vm_get_paddr(void *page_dir, uint64_t vaddr)
{
    return memory_get_paddr(page_dir, vaddr);
}

static inline int32_t mm_vm_copy_user_space(void *dst_pgd, void *src_pgd)
{
    return memory_copy_uvm_4level(dst_pgd, src_pgd);
}

static inline void mm_vm_copy_to_uva(void *pgd, uint64_t user_vaddr,
                                     uint64_t src_paddr, uint64_t size)
{
    copydata_to_uvm(pgd, user_vaddr, src_paddr, size);
}

/*
 * 把一页标成"设备缓冲"：unmap 时只拆映射，不释放页也不碰引用计数。
 * 用于把 dma_buf / ION 的缓冲区映射进用户态 —— 页归句柄所有，不归映射。
 * 语义与 NOFREE 的区别见 mmu.h 里 PTE_KEEP 的说明。
 */
static inline void mm_vm_set_pte_keep(void *pgd, uint64_t vaddr)
{
    memory_set_pte_keep(pgd, vaddr);
}

/* ── 用户地址空间生命周期钩子（实现在 kernel/mm/aarch64/vmm.c）────── */

/* AArch64 用 TTBR0/TTBR1 硬件分割内核/用户空间，无需复制内核映射。 */
extern void mm_vm_init_user_pgd(void *pgd, uint64_t pgd_phys);
extern void mm_vm_destroy_user_space(uint64_t pgd_phys);
extern int mm_vm_unmap_page(void *pgd, uint64_t vaddr);
extern void mm_vm_flush_tlb_user(void);

#endif /* AARCH64_MM_VM_H */
