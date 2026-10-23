/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/mm/kmalloc.c — 内核内存分配器

   在 PMM 之上提供返回虚拟地址的分配接口。

   现在 kmalloc 在每笔分配的第一页开头放一个 16 字节 header(magic +
   页数 + 请求大小), 返回 base + 16。这样 kfree(ptr) 不再依赖调用方
   传对 size：传小会泄漏, 传大会释放相邻页。

   返回地址从"页对齐"改成"16 字节对齐"：当前调用者只要求自然对齐；
   需要页对齐/DMA 的场景继续用 kalloc_pages()。
*/

#include "kmalloc.h"
#include "klog.h"
#include "mm_vm.h" /* PAGE_SIZE + phys_to_virt/virt_to_phys */
#include "pmm.h"
#include "string.h"

#define KMALLOC_MAGIC 0x4B4D414C4C4F4331ULL /* "KMALLOC1" */

typedef struct {
    uint64_t magic;
    uint32_t pages; /* 总页数(含 header) */
    uint32_t size;  /* 用户请求的字节数 */
} kmalloc_hdr_t;

_Static_assert(sizeof(kmalloc_hdr_t) == 16, "kmalloc header must stay 16 bytes");

void *kalloc_pages(uint32_t pages)
{
    uint64_t paddr = pmm_alloc_pages(&g_pmm, pages);
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

    /*
       内核/rootfs/平台保留区的保护在 pmm_free_pages() 内部做(那里对所有
       调用者生效)。这里以前有一份单独判断内核范围的副本, 已经删掉。
    */
    pmm_free_pages(&g_pmm, virt_to_phys(addr), pages);
}

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;

    if (size > (size_t)UINT32_MAX - sizeof(kmalloc_hdr_t))
        return NULL;

    size_t total = size + sizeof(kmalloc_hdr_t);
    uint32_t pages = (uint32_t)((total + PAGE_SIZE - 1) / PAGE_SIZE);

    kmalloc_hdr_t *h = kalloc_pages(pages);
    if (!h)
        return NULL;

    h->magic = KMALLOC_MAGIC;
    h->pages = pages;
    h->size = (uint32_t)size;

    return (uint8_t *)h + sizeof(*h);
}

void kfree(void *ptr)
{
    if (ptr == NULL)
        return;

    /*
       kmalloc 指针固定落在页内 offset 16。这个检查把 kalloc_pages 的
       页对齐指针(offset 0)以及任意野指针挡在外面, 避免把 header
       之前的 16 字节当成账本。
    */
    if (((uintptr_t)ptr & (PAGE_SIZE - 1)) != sizeof(kmalloc_hdr_t)) {
        KLOG_ERROR("kfree: not a kmalloc pointer: 0x%llx\n", (unsigned long long)(uintptr_t)ptr);
        return;
    }

    kmalloc_hdr_t *h = (kmalloc_hdr_t *)((uint8_t *)ptr - sizeof(*h));
    if (h->magic != KMALLOC_MAGIC) {
        KLOG_ERROR("kfree: bad magic at 0x%llx\n", (unsigned long long)(uintptr_t)ptr);
        return;
    }

    uint32_t pages = h->pages;
    size_t size = h->size;
    if (pages == 0 || size > (size_t)pages * PAGE_SIZE - sizeof(kmalloc_hdr_t)) {
        KLOG_ERROR("kfree: corrupt header at 0x%llx (pages=%u size=%llu)\n", (unsigned long long)(uintptr_t)ptr, pages,
                   (unsigned long long)size);
        return;
    }

    /*
       先毒化 magic 再释放。页如果还没被复用, 第二次 kfree 会在这里
       被挡住；如果已经被复用成新的 kmalloc, 跨复用的 double free
       仍然无法完全识别 —— 那需要 slab/元数据表, 后面再做。
    */
    h->magic = 0;
    kfree_pages(h, pages);
}
