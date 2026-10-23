/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   driver/dma/dma_heap.h — 堆后端(内存从哪来)

   这一层故意只有两个函数：它是 P0 与 P1 之间的接缝。

     现在(P0)：从全局 PMM 要一段物理连续页。
     将来(P1)：从启动时预留的 CMA 区里切 —— 换掉 dma_heap.c 一处即可,
                 dma_buf.c 及以上(含 NPU 作业描述符, DRM shim)一行不动。

   为什么一定要能换：`pmm_alloc_pages` 是单一全局位图的 next-fit 扫描
   (kernel/mm/pmm.c), 系统跑一会儿就碎片化, 几十 MB 的连续请求会随机失败 ——
   而失败点是"模型加载到一半", 极难归因。预留区保证「能分配就一定连续」。
*/
#ifndef DRIVER_DMA_HEAP_H
#define DRIVER_DMA_HEAP_H

#include "types.h"

/*
   申请一块物理连续的缓冲区。

   @size:      请求字节数(内部向上取整到页)
   @paddr_out: [out] 物理基址(页对齐)
   @kva_out:   [out] 内核虚拟地址(phys_to_virt 直接映射)
   @size_out:  [out] 实际大小(页对齐后的字节数)

   返回 0 成功；-1 失败(内存不足 / PMM 未初始化 / 请求为 0)。

   成功返回的缓冲区已清零, 语义上等同 dma_alloc_coherent。
*/
int dma_heap_alloc(size_t size, uint64_t *paddr_out, void **kva_out, size_t *size_out);

/* 归还 dma_heap_alloc 拿到的缓冲区。@size 必须是 size_out 那个值。 */
void dma_heap_free(uint64_t paddr, size_t size);

#endif /* DRIVER_DMA_HEAP_H */
