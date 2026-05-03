#ifndef RISCV64_MM_VM_H
#define RISCV64_MM_VM_H

#include "types.h"

/*
 * RISC-V 用户 VM 尚未实现：
 * - map/copy 返回失败
 * - get_paddr 返回 0（未映射）
 */
static inline int32_t mm_vm_map_pages(void *page_dir, uint64_t vaddr,
                                      uint64_t paddr, int32_t count,
                                      uint64_t perm)
{
    (void)page_dir;
    (void)vaddr;
    (void)paddr;
    (void)count;
    (void)perm;
    return -1;
}

static inline uint64_t mm_vm_get_paddr(void *page_dir, uint64_t vaddr)
{
    (void)page_dir;
    (void)vaddr;
    return 0;
}

static inline int32_t mm_vm_copy_user_space(void *dst_pgd, void *src_pgd)
{
    (void)dst_pgd;
    (void)src_pgd;
    return -1;
}

static inline void mm_vm_copy_to_uva(void *pgd, uint64_t user_vaddr,
                                     uint64_t src_paddr, uint64_t size)
{
    (void)pgd;
    (void)user_vaddr;
    (void)src_paddr;
    (void)size;
}

#endif /* RISCV64_MM_VM_H */
