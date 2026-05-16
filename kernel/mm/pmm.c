/*
 * kernel/mm/pmm.c - 物理内存管理器实现
 *
 * 参考：ref/Avatar/kernel/mem/pmm.c
 */

#include "pmm.h"
#include "klog.h"
#include "assert.h"
#include "string.h"
#include "mm_vm.h"  /* 提供 PAGE_SIZE */
#include "../../driver/blk/ramblk_cfg.h" /* RAMBLK_PHYS_BASE / RAMBLK_PHYS_END */

/* 全局 PMM 状态 — 位图缓冲区使用编译期最大值（PMM_BITMAP_MAX_BYTES = 128KB = 4GB/4KB/8bit）*/
static uint8_t g_pmm_bitmap_buffer[PMM_BITMAP_MAX_BYTES];
pmm_t pmm;
pmm_t *g_pmm = &pmm;

/* ── PMM 初始化 ───────────────────────────────────────────────────── */

/*
 * pmm_init - 初始化物理内存管理器
 * @pmm:           物理内存管理器实例
 * @start_addr:    物理内存起始地址
 * @size:          内存大小（字节）
 * @bitmap_buffer: 位图缓冲区
 * @bitmap_size:   位图大小（字节）
 */
void pmm_init(pmm_t *pmm,
              uint64_t start_addr,
              uint64_t size,
              uint8_t *bitmap_buffer,
              size_t bitmap_size)
{
    assert(pmm != NULL);
    assert(bitmap_buffer != NULL);
    assert(size > 0);

    /* 初始化基本信息 */
    pmm->start_addr  = start_addr;
    pmm->total_size  = size;
    pmm->page_size   = PAGE_SIZE;
    pmm->total_pages = size / PAGE_SIZE;
    pmm->free_pages  = pmm->total_pages;

    /*
     * bitmap_init 的 size 单位是 bit，bitmap_size 传入单位是 byte。
     * 这里必须做 byte -> bit 转换，否则 PMM 只会管理 1/8 的页面。
     */
    bitmap_init(&pmm->bitmap, bitmap_buffer, bitmap_size * 8);
    assert(pmm->bitmap.size >= pmm->total_pages);

    /* 初始化互斥锁 */
    mutex_init(&pmm->mutex);

    KLOG_INFO("PMM initialized:\n");
    KLOG_INFO("  start_addr  = 0x%llx\n", start_addr);
    KLOG_INFO("  total_size = 0x%llx (%llu MB)\n",
              size, size / (1024 * 1024));
    KLOG_INFO("  page_size  = 0x%llx (%u KB)\n",
              pmm->page_size, pmm->page_size / 1024);
    KLOG_INFO("  total_pages = %llu\n", pmm->total_pages);
    KLOG_INFO("  free_pages  = %llu\n", pmm->free_pages);
}

/* ── 页面分配 ───────────────────────────────────────────────────── */

/*
 * pmm_alloc_pages - 分配连续的物理页面
 * @pmm:        物理内存管理器
 * @page_count: 需要分配的页面数
 *
 * 返回：分配的物理地址，失败返回 0
 */
uint64_t pmm_alloc_pages(pmm_t *pmm, uint32_t page_count)
{
    assert(pmm != NULL);
    assert(page_count > 0);

    uint64_t paddr = 0;

    mutex_lock(&pmm->mutex);

    /* 查找连续的空闲页面 */
    size_t page_index = bitmap_find_contiguous_free(&pmm->bitmap, page_count);
    if (page_index != (size_t)-1) {
        /* 标记页面为已分配 */
        bitmap_set_range(&pmm->bitmap, page_index, page_count);

        /* 计算物理地址 */
        paddr = pmm->start_addr + page_index * pmm->page_size;

        /* 更新空闲页面计数 */
        pmm->free_pages -= page_count;

        // KLOG_DEBUG("PMM: allocated %u pages at 0x%llx (index %zu)\n",
        //            page_count, paddr, page_index);
    } else {
        KLOG_ERROR("PMM: failed to allocate %u pages\n", page_count);
    }

    mutex_unlock(&pmm->mutex);

    return paddr;
}

/* ── 页面释放 ───────────────────────────────────────────────────── */

/*
 * pmm_free_pages - 释放物理页面
 * @pmm:        物理内存管理器
 * @paddr:      物理地址
 * @page_count: 页面数
 */
void pmm_free_pages(pmm_t *pmm, uint64_t paddr, uint32_t page_count)
{
    assert(pmm != NULL);
    assert(page_count > 0);

    if ((paddr & (pmm->page_size - 1)) != 0) {
        KLOG_ERROR("PMM: free address is not page-aligned: 0x%llx\n", paddr);
        return;
    }

    /* 检查地址范围 */
    if (paddr < pmm->start_addr || paddr >= pmm->start_addr + pmm->total_size) {
        KLOG_ERROR("PMM: invalid free address: 0x%llx\n", paddr);
        return;
    }

    mutex_lock(&pmm->mutex);

    /* 计算页面索引 */
    uint64_t page_index = (paddr - pmm->start_addr) / pmm->page_size;

    /* 检查范围 */
    if (page_index + page_count <= pmm->total_pages) {
        uint64_t freed = 0;

        /* 仅在 bit 为 1 时清除并更新计数，防止 double free 污染统计 */
        for (uint64_t i = 0; i < page_count; i++) {
            uint64_t idx = page_index + i;
            if (bitmap_test(&pmm->bitmap, idx)) {
                bitmap_clear(&pmm->bitmap, idx);
                freed++;
            }
        }

        pmm->free_pages += freed;

        if (freed != page_count) {
            KLOG_WARN("PMM: partial free detected: requested=%u, actually_freed=%llu, paddr=0x%llx\n",
                      page_count, freed, paddr);
        }

        // KLOG_DEBUG("PMM: freed %u pages at 0x%llx (index %llu)\n",
        //            page_count, paddr, page_index);
    } else {
        KLOG_ERROR("PMM: invalid free request: paddr=0x%llx, count=%u\n",
                   paddr, page_count);
    }

    mutex_unlock(&pmm->mutex);
}

/* ── 内存标记 ───────────────────────────────────────────────────── */

/*
 * pmm_mark_allocated - 标记内存区域为已分配
 * @pmm:        物理内存管理器
 * @start_addr: 起始地址
 * @end_addr:   结束地址
 *
 * 用于标记内核等保留区域，不会实际分配内存。
 */
void pmm_mark_allocated(pmm_t *pmm, uint64_t start_addr, uint64_t end_addr)
{
    assert(pmm != NULL);
    assert(start_addr <= end_addr);

    /* 检查地址范围是否在PMM管理的内存内 */
    if (start_addr < pmm->start_addr || end_addr >= pmm->start_addr + pmm->total_size) {
        KLOG_ERROR("PMM: invalid address range for marking\n");
        KLOG_ERROR("  requested: 0x%llx - 0x%llx\n", start_addr, end_addr);
        KLOG_ERROR("  PMM range: 0x%llx - 0x%llx\n", pmm->start_addr, pmm->start_addr + pmm->total_size);
        return;
    }

    /* 计算页面范围 */
    uint64_t start_page = (start_addr - pmm->start_addr) / pmm->page_size;
    uint64_t end_page   = (end_addr - pmm->start_addr) / pmm->page_size;
    uint64_t free_before, free_after, newly_marked = 0;

    KLOG_INFO("PMM: marking range 0x%llx - 0x%llx as allocated\n", start_addr, end_addr);
    KLOG_INFO("  page indices: %llu - %llu (total: %llu pages)\n",
              start_page, end_page, end_page - start_page + 1);

    /* 先锁定，读取 free_pages */
    mutex_lock(&pmm->mutex);
    free_before = pmm->free_pages;

    /* 遍历所有页，仅在 0->1 转换时更新 free_pages，保持计数与位图一致 */
    for (uint64_t i = start_page; i <= end_page; i++) {
        if (!bitmap_test(&pmm->bitmap, i)) {
            bitmap_set(&pmm->bitmap, i);
            pmm->free_pages--;
            newly_marked++;
        }
    }

    free_after = pmm->free_pages;
    mutex_unlock(&pmm->mutex);

    /* 在锁外输出日志 */
    KLOG_INFO("  newly_marked: %llu pages, free_pages before: %llu, after: %llu\n",
              newly_marked, free_before, free_after);
}

/*
 * pmm_mark_kernel_allocated - 标记内核内存区域为已分配
 * @pmm: 物理内存管理器
 *
 * 自动标记内核代码和数据段为已分配。
 * 注意：需要在链接器脚本中定义 __kernel_start 和 __kernel_end 符号。
 */
void pmm_mark_kernel_allocated(pmm_t *pmm)
{
    /* 外部符号（在链接器脚本中定义） */
    extern char __kernel_start[];
    extern char __kernel_end[];

    uint64_t start = (uint64_t)__kernel_start;
    uint64_t end   = ALIGN_UP((uint64_t)__kernel_end, PAGE_SIZE);

    KLOG_INFO("PMM: __kernel_start = 0x%llx, __kernel_end = 0x%llx\n", start, end);
    KLOG_INFO("PMM: KERNEL_VMA = 0x%llx\n", KERNEL_VMA);
    KLOG_INFO("PMM: PMM start_addr = 0x%llx, total_size = 0x%llx\n",
              pmm->start_addr, pmm->total_size);

    /*
     * 检查地址是否在合理的内核虚拟地址范围内
     * AArch64: 0xffff000000000000 +
     * 如果地址 < KERNEL_VMA，说明可能是：
     * 1. 已经是物理地址
     * 2. 符号地址异常
     *
     * 判断方法：如果地址在 PMM 管理的物理内存范围内，则认为是物理地址
     */
    uint64_t start_phys, end_phys;

    if (start >= KERNEL_VMA) {
        uint64_t cand_start = virt_to_phys(start);
        uint64_t cand_end = virt_to_phys(end);
        if (cand_start >= pmm->start_addr && cand_end <= pmm->start_addr + pmm->total_size) {
            start_phys = cand_start;
            end_phys = cand_end;
            KLOG_WARN("PMM: kernel symbols are high-half virtual addresses\n");
            KLOG_INFO("  virtual range: 0x%llx - 0x%llx\n", start, end);
            KLOG_INFO("  physical range: 0x%llx - 0x%llx\n", start_phys, end_phys);
        } else {
            KLOG_WARN("PMM: virtual->physical conversion out of PMM range, fallback to raw values\n");
            start_phys = start;
            end_phys = end;
            KLOG_INFO("  fallback range: 0x%llx - 0x%llx\n", start_phys, end_phys);
        }
    } else if (start >= pmm->start_addr && start < pmm->start_addr + pmm->total_size) {
        /* 地址在 PMM 物理内存范围内，认为已经是物理地址 */
        start_phys = start;
        end_phys   = end;
        KLOG_WARN("PMM: kernel symbols are already physical addresses\n");
        KLOG_INFO("  physical range: 0x%llx - 0x%llx\n", start_phys, end_phys);
    } else {
        /* 异常情况，按原值处理并记录日志 */
        KLOG_WARN("PMM: unusual kernel symbol range, using raw addresses\n");
        start_phys = start;
        end_phys   = end;
        KLOG_INFO("  raw range: 0x%llx - 0x%llx\n", start_phys, end_phys);
    }


    pmm_mark_allocated(pmm, start_phys, end_phys);
}

/*
 * pmm_initialize - 初始化物理内存管理器
 *
 * 必须在 kernel_main 中早期调用，在任何内存分配之前。
 */
void pmm_initialize(void)
{
    KLOG_INFO("=== Initializing Physical Memory Manager ===\n");

    /* 初始化 PMM */
    pmm_init(g_pmm,
             PMM_RAM_BASE,
             PMM_RAM_SIZE,
             g_pmm_bitmap_buffer,
             sizeof(g_pmm_bitmap_buffer));

    /* 标记内核内存区域为已分配 */
    KLOG_INFO("Marking kernel memory as allocated...\n");
    pmm_mark_kernel_allocated(g_pmm);

    /* 运行时保留区（由 platform_conf_scan() 从 Lua 读取） */
    for (int i = 0; i < g_pmm_resv_count; i++) {
        KLOG_INFO("Reserving PMM extra region(%s): 0x%llx - 0x%llx\n",
                  g_pmm_reserves[i].tag,
                  (uint64_t)g_pmm_reserves[i].start,
                  (uint64_t)g_pmm_reserves[i].end);
        pmm_mark_allocated(g_pmm,
                           (uint64_t)g_pmm_reserves[i].start,
                           (uint64_t)g_pmm_reserves[i].end);
    }

    /* 预留 rootfs 物理区域，防止 PMM 将其分配出去 */
    KLOG_INFO("Reserving rootfs region: 0x%llx - 0x%llx\n",
              (uint64_t)RAMBLK_PHYS_BASE, (uint64_t)RAMBLK_PHYS_END);
    pmm_mark_allocated(g_pmm, RAMBLK_PHYS_BASE, RAMBLK_PHYS_END);

    KLOG_INFO("PMM initialization completed\n");
    KLOG_INFO("  g_pmm = %p\n", g_pmm);
}
