#ifndef PMM_H
#define PMM_H

#include "types.h"
#include "bitmap.h"
#include "task/mutex.h"
#include "platform_cfg.h"


/* ── 物理内存配置 ───────────────────────────────────────────────────── */

/* 运行时变量别名（从 platform_conf_scan() 提取，在 pmm_initialize() 前有效） */
#define PMM_RAM_BASE    g_mem_ram_base
#define PMM_RAM_SIZE    g_mem_ram_size

/* 位图静态缓冲区的最大字节数：支持最多 4GB RAM @ 4KB 页 */
#define PMM_BITMAP_MAX_BYTES  131072U

/*
 * include/pmm.h - 物理内存管理器
 *
 * 提供物理页面的分配和释放功能，独立于虚拟内存管理
 */

/* ── 物理内存管理器结构 ───────────────────────────────────────────── */

typedef struct {
    mutex_t  mutex;        /* 互斥锁                      */
    bitmap_t bitmap;       /* 页面分配位图                */
    uint64_t start_addr;   /* 物理内存起始地址            */
    uint64_t total_size;   /* 总内存大小                  */
    uint64_t page_size;    /* 页面大小（通常 4KB）        */
    uint64_t total_pages;  /* 总页面数                    */
    uint64_t free_pages;   /* 空闲页面数                  */
} pmm_t;

extern pmm_t *g_pmm;

/* ── PMM API ───────────────────────────────────────────────────── */

/**
 * pmm_init - 初始化物理内存管理器
 * @pmm:           物理内存管理器实例
 * @start_addr:    物理内存起始地址
 * @size:          内存大小（字节）
 * @bitmap_buffer: 位图缓冲区
 * @bitmap_size:   位图大小（字节）
 *
 * 使用前必须调用此函数初始化 PMM。
 */
void pmm_init(pmm_t *pmm,
              uint64_t start_addr,
              uint64_t size,
              uint8_t *bitmap_buffer,
              size_t bitmap_size);

/**
 * pmm_alloc_pages - 分配连续的物理页面
 * @pmm:        物理内存管理器
 * @page_count: 需要分配的页面数
 *
 * 返回：分配的物理地址，失败返回 0
 */
uint64_t pmm_alloc_pages(pmm_t *pmm, uint32_t page_count);

/**
 * pmm_free_pages - 释放物理页面
 * @pmm:        物理内存管理器
 * @paddr:      物理地址
 * @page_count: 页面数
 *
 * 释放之前分配的物理页面。
 */
void pmm_free_pages(pmm_t *pmm, uint64_t paddr, uint32_t page_count);

/**
 * pmm_mark_allocated - 标记内存区域为已分配
 * @pmm:        物理内存管理器
 * @start_addr: 起始地址
 * @end_addr:   结束地址
 *
 * 用于标记内核等保留区域，不会实际分配内存。
 */
void pmm_mark_allocated(pmm_t *pmm, uint64_t start_addr, uint64_t end_addr);

/**
 * pmm_get_free_pages - 获取空闲页面数
 * @pmm: 物理内存管理器
 *
 * 返回：空闲页面数
 */
static inline uint64_t pmm_get_free_pages(pmm_t *pmm)
{
    return pmm->free_pages;
}

/**
 * pmm_get_total_pages - 获取总页面数
 * @pmm: 物理内存管理器
 *
 * 返回：总页面数
 */
static inline uint64_t pmm_get_total_pages(pmm_t *pmm)
{
    return pmm->total_pages;
}

/**
 * pmm_mark_kernel_allocated - 标记内核内存区域为已分配
 * @pmm: 物理内存管理器
 *
 * 自动标记内核代码和数据段为已分配。
 */
void pmm_mark_kernel_allocated(pmm_t *pmm);

/* ── PMM 初始化和测试（架构特定）────────────────────────────────────── */

/**
 * pmm_initialize - 初始化物理内存管理器
 *
 * 必须在 kernel_main 中早期调用，在任何内存分配之前。
 * 会根据架构自动配置内存范围。
 */
void pmm_initialize(void);

/**
 * run_pmm_tests - 运行所有 PMM 测试
 *
 * 在 pmm_initialize() 之后调用。
 */
void run_pmm_tests(void);

#endif /* PMM_H */
