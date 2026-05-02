
#ifndef VMM_H
#define VMM_H

#include "types.h"
#include "mmu.h"  /* for pte_t */

/* ── VMM API ───────────────────────────────────────────────────── */

/**
 * kalloc_pages - 分配物理页面并返回虚拟地址
 * @pages: 页面数
 *
 * 返回：虚拟地址，失败返回 NULL
 */
void *kalloc_pages(uint32_t pages);

/**
 * kfree_pages - 释放虚拟地址对应的物理页面
 * @addr: 虚拟地址
 * @pages: 页面数
 */
void kfree_pages(void *addr, uint32_t pages);

/**
 * copydata_to_uvm - 将数据从物理地址拷贝到用户虚拟地址空间
 * @page_dir: 用户页表基址（虚拟地址）
 * @vaddr: 目标虚拟地址
 * @paddr: 源物理地址
 * @size: 拷贝大小（字节）
 *
 * 注意：目标虚拟地址必须在页表中已映射
 */
void copydata_to_uvm(pte_t *page_dir, uint64_t vaddr, uint64_t paddr, uint64_t size);

/* ── VMM 测试函数 ───────────────────────────────────────────── */

/**
 * kmem_test - 运行 VMM 模块的所有测试
 *
 * 测试包括：
 * - 物理内存分配/释放
 * - 页表查找和创建
 * - 内存映射
 * - UVM 分配和释放
 * - 数据拷贝
 */
void kmem_test(void);

#endif