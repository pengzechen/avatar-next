/*
   driver/dma/dma_heap.c — 堆后端实现(P0：全局 PMM)

   见 dma_heap.h 里为什么这一层只有两个函数。
*/
#include "dma_heap.h"

#include "klog.h"
#include "mm_vm.h"
#include "pmm.h"
#include "string.h"
#include "types.h"

#define DMA_HEAP_PAGE_SIZE 4096U

int dma_heap_alloc(size_t size, uint64_t *paddr_out, void **kva_out, size_t *size_out)
{
    if (size == 0U || !paddr_out || !kva_out || !size_out)
        return -1;

    if (!pmm_is_initialized()) {
        KLOG_ERROR("[dma_heap] g_pmm not initialized\n");
        return -1;
    }

    size_t pages = (size + DMA_HEAP_PAGE_SIZE - 1U) / DMA_HEAP_PAGE_SIZE;
    size_t bytes = pages * DMA_HEAP_PAGE_SIZE;

    /*
       pmm_alloc_pages 返回的是物理连续的一段 —— 这正是无 IOMMU 的前提,
       也是这里不能用 kalloc_pages 之外任何"逐页拼"分配器的原因。
    */
    uint64_t pa = pmm_alloc_pages(&g_pmm, (uint32_t)pages);
    if (pa == 0U) {
        KLOG_ERROR("[dma_heap] contiguous alloc failed: size=%zu pages=%zu\n", size, pages);
        return -1;
    }

    void *va = phys_to_virt(pa);
    /* DMA coherent 语义：设备看到的是干净的零, 不会把上一次的残留读出去 */
    memset(va, 0, bytes);

    *paddr_out = pa;
    *kva_out = va;
    *size_out = bytes;

    KLOG_DEBUG("[dma_heap] alloc size=%zu bytes=%zu pa=0x%llx\n", size, bytes, (unsigned long long)pa);
    return 0;
}

void dma_heap_free(uint64_t paddr, size_t size)
{
    if (paddr == 0U || size == 0U)
        return;

    if (!pmm_is_initialized())
        return;

    pmm_free_pages(&g_pmm, paddr, (uint32_t)(size / DMA_HEAP_PAGE_SIZE));
}
