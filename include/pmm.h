#ifndef PMM_H
#define PMM_H

#include "types.h"
#include "bitmap.h"
#include "spinlock.h"
#include "platform_cfg.h"

/* ── 物理内存配置 ───────────────────────────────────────────────────── */

/* 运行时变量别名（从 platform_conf_scan() 提取，在 pmm_initialize() 前有效） */
#define PMM_RAM_BASE g_mem_ram_base
#define PMM_RAM_SIZE g_mem_ram_size

/* 位图静态缓冲区的最大字节数：支持最多 4GB RAM @ 4KB 页 */
#define PMM_BITMAP_MAX_BYTES 131072U

/* 同时存在的保留区数量上限：内核镜像 + rootfs + 平台 reservemap */
#define PMM_MAX_RESERVED_RANGES 16U

/*
 * include/pmm.h - 物理内存管理器
 *
 * 提供物理页面的分配和释放功能，独立于虚拟内存管理
 */

/* ── 保留区 ───────────────────────────────────────────────────────── */

/*
 * 保留区和"已分配"在位图里同样是 1，区别在于保留区还会记在这张表里：
 * pmm_free_pages() 命中保留区时拒绝释放、且不清位。位图只有 1 bit，
 * 没有这张表的话，任何一次误 free 都能把内核/rootfs 的页面交还分配器。
 *
 * 区间语义：半开区间 [start, end)，单位是物理地址（页对齐）。
 */
typedef struct {
    uint64_t start;
    uint64_t end; /* 不含 */
    const char *tag;
} pmm_reserved_t;

/* ── 物理内存管理器结构 ───────────────────────────────────────────── */

typedef struct {
    spinlock_t lock;      /* 自旋锁（任务初始化前可用）  */
    bitmap_t bitmap;      /* 页面分配位图                */
    uint64_t start_addr;  /* 物理内存起始地址            */
    uint64_t total_size;  /* 总内存大小                  */
    uint64_t page_size;   /* 页面大小（通常 4KB）        */
    uint64_t total_pages; /* 总页面数                    */
    uint64_t free_pages;  /* 空闲页面数                  */

    /* 保留区（内核镜像、rootfs、平台 reservemap） */
    pmm_reserved_t reserved[PMM_MAX_RESERVED_RANGES];
    unsigned reserved_count;
    uint64_t reserved_pages; /* 保留区覆盖的页面数（重叠区间不重复计） */

    /*
     * next-fit 分配提示：下次从哪一位开始找。
     *
     * 没有它的话每次分配都从位图**开头**扫 —— 而低地址被内核与 rootfs
     * 保留区占着，于是每分配一页都要白扫几万位。加载 guest 内核映像要
     * 近万次分配，累计下来是几亿次位测试（详见 lib/bitmap.c 的说明）。
     */
    size_t alloc_hint;
} pmm_t;

/* 全局 PMM 单例（boot 时由 pmm_initialize() 初始化） */
extern pmm_t g_pmm;

/*
 * pmm_is_initialized - 全局 PMM 是否已完成 pmm_init()
 *
 * 静态对象初值为 0；pmm_init() 断言 size > 0 并设置 total_pages，
 * 所以 total_pages 非 0 是可靠的"已初始化"判据。
 */
static inline bool pmm_is_initialized(void)
{
    return g_pmm.total_pages != 0;
}

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
void pmm_init(pmm_t *pmm, uint64_t start_addr, uint64_t size,
              uint8_t *bitmap_buffer, size_t bitmap_size);

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
 *
 * 返回：true 成功；false 拒绝。拒绝的情况包括地址非法、命中保留区，
 * 以及区间里有一页本来就是空闲的（double free）。最后一种整笔拒绝、
 * 一个 bit 都不动 —— PMM 是最底层的分配器，宁可早报错，也不要让
 * free_pages 的账悄悄漂移。
 */
bool pmm_free_pages(pmm_t *pmm, uint64_t paddr, uint32_t page_count);

/**
 * pmm_reserve - 预留一段物理内存
 * @pmm:        物理内存管理器
 * @start_addr: 起始物理地址（含，内部向下页对齐）
 * @end_addr:   结束物理地址（不含，内部向上页对齐）
 * @tag:        保留区名字（必须是静态生命周期字符串，用于日志/排障）
 *
 * 位图里把这些页标成 1（分配器不会再发出去），并记录到保留区表：
 * 之后 pmm_free_pages() 对这段区间的释放请求会被拒绝，且不会清位。
 *
 * 返回：true 成功（空区间也算成功）；false 越界或表满。
 */
bool pmm_reserve(pmm_t *pmm, uint64_t start_addr, uint64_t end_addr,
                 const char *tag);

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

/* ── PMM 初始化和测试 ───────────────────────────────────────────── */

/**
 * pmm_initialize - 初始化物理内存管理器
 * @kernel_start_phys: 内核镜像起始物理地址（含）
 * @kernel_end_phys:   内核镜像结束物理地址（不含）
 *
 * 必须在 kernel_main 中早期调用，在任何内存分配之前。
 * 会根据架构自动配置内存范围。
 *
 * 内核镜像区间由调用者（boot 层）从链接符号换算成物理地址后传入：
 * PMM 只认物理地址，不认识 KERNEL_VMA / 高半区布局。
 */
void pmm_initialize(uint64_t kernel_start_phys, uint64_t kernel_end_phys);

/**
 * run_pmm_tests - 运行所有 PMM 测试
 *
 * 在 pmm_initialize() 之后调用。
 */
void run_pmm_tests(void);

#endif /* PMM_H */
