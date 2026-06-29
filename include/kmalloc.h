#ifndef KMALLOC_H
#define KMALLOC_H

/*
 * include/kmalloc.h — 内核内存分配接口
 *
 * 所有函数返回内核虚拟地址（高半区），内部通过 PMM 分配物理页并转换。
 * 当前实现为页粒度分配，将来可在此基础上叠加 slab。
 */

#include "types.h"

/*
 * kalloc_pages — 分配 n 个连续物理页，返回已清零的内核虚拟地址
 * 失败返回 NULL。
 */
void *kalloc_pages(uint32_t pages);

/*
 * kfree_pages — 释放 kalloc_pages 分配的内存
 * @addr:  kalloc_pages 返回的虚拟地址
 * @pages: 分配时的页数
 */
void kfree_pages(void *addr, uint32_t pages);

/*
 * kmalloc — 分配至少 size 字节的内核内存（页对齐，已清零）
 * 失败返回 NULL。
 */
void *kmalloc(size_t size);

/*
 * kfree — 释放 kmalloc 分配的内存
 * @ptr:  kmalloc 返回的指针
 * @size: 分配时的 size
 */
void kfree(void *ptr, size_t size);

#endif /* KMALLOC_H */
