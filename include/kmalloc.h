#ifndef KMALLOC_H
#define KMALLOC_H

/*
   include/kmalloc.h — 内核内存分配接口

   所有函数返回内核虚拟地址(高半区), 内部通过 PMM 分配物理页并转换。
   当前实现为页粒度分配, 将来可在此基础上叠加 slab。
*/

#include "types.h"

/*
   kalloc_pages — 分配 n 个连续物理页, 返回已清零的, 页对齐的内核虚拟地址
   失败返回 NULL。
*/
void *kalloc_pages(uint32_t pages);

/*
   kfree_pages — 释放 kalloc_pages 分配的内存
   @addr:  kalloc_pages 返回的虚拟地址
   @pages: 分配时的页数
*/
void kfree_pages(void *addr, uint32_t pages);

/*
   kmalloc — 分配至少 size 字节的内核内存(16 字节对齐, 已清零)

   返回地址由页内 16 字节 header 管理, 调用者不要回退/改写它。
   需要页对齐(DMA, 页表等)的调用者请用 kalloc_pages()。
   失败返回 NULL。
*/
void *kmalloc(size_t size);

/*
   kfree — 释放 kmalloc 分配的内存
   @ptr: kmalloc 原样返回的指针

   不需要再传 size：页数记录在分配头里。ptr 不是 kmalloc 返回的指针
   (包括 kalloc_pages 的页对齐指针)会被拒绝并打日志。
*/
void kfree(void *ptr);

#endif /* KMALLOC_H */
