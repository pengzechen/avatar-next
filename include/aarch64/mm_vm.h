#ifndef AARCH64_MM_VM_H
#define AARCH64_MM_VM_H

#include "types.h"

/* AArch64 现有 VM 实现导出符号 */
extern int32_t memory_create_map(void *page_dir, uint64_t vaddr, uint64_t paddr,
                                 int32_t count, uint64_t perm);
extern uint64_t memory_get_paddr(void *page_dir, uint64_t vaddr);

extern int32_t memory_copy_uvm_4level(void *dst_pgd, void *src_pgd);

/* copydata_to_uvm: 将 src_paddr 处的数据拷贝到 pgd 对应用户地址空间的 user_vaddr */
extern void copydata_to_uvm(void *page_dir, uint64_t vaddr, uint64_t paddr, uint64_t size);



static inline int32_t mm_vm_map_pages(void *page_dir, uint64_t vaddr,
                                      uint64_t paddr, int32_t count,
                                      uint64_t perm)
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

#endif /* AARCH64_MM_VM_H */
