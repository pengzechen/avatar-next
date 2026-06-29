/*
 * kernel/mm/kmalloc.c — 内核内存分配器
 *
 * 在 PMM 之上提供返回虚拟地址的分配接口。
 */

#include "kmalloc.h"
#include "pmm.h"
#include "mm_vm.h"
#include "klog.h"
#include "string.h"

void *kalloc_pages(uint32_t pages)
{
    uint64_t paddr = pmm_alloc_pages(g_pmm, pages);
    if (paddr == 0)
        return NULL;

    void *vaddr = phys_to_virt(paddr);
    memset(vaddr, 0, (uint64_t)pages * PAGE_SIZE);
    return vaddr;
}

void kfree_pages(void *addr, uint32_t pages)
{
    if (addr == NULL)
        return;

    uint64_t paddr = virt_to_phys(addr);

    extern char __kernel_start[];
    extern char __kernel_end[];

    uint64_t ks = (uint64_t)__kernel_start;
    uint64_t ke = (uint64_t)__kernel_end;
    if (ks >= KERNEL_VMA)
        ks = virt_to_phys(ks);
    if (ke >= KERNEL_VMA)
        ke = virt_to_phys(ke);

    if (paddr >= ks && paddr <= ke) {
        KLOG_WARN("kfree_pages: attempt to free kernel memory at 0x%llx\n",
                  paddr);
        return;
    }

    pmm_free_pages(g_pmm, paddr, pages);
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;
    uint32_t pages = (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    return kalloc_pages(pages);
}

void kfree(void *ptr, size_t size)
{
    if (ptr == NULL || size == 0)
        return;
    uint32_t pages = (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    kfree_pages(ptr, pages);
}
