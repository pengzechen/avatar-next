/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/mm/pmm.c - 物理内存管理器实现

   参考：ref/Avatar/kernel/mem/pmm.c
*/

#include "pmm.h"
#include "klog.h"
#include "assert.h"
#include "string.h"
#include "page.h"
#include "platform_cfg.h" /* g_mem_rootfs_base / g_mem_rootfs_size */

/* 全局 PMM 状态 — 位图缓冲区使用编译期最大值(PMM_BITMAP_MAX_BYTES = 128KB = 4GB/4KB/8bit) */
static uint8_t g_pmm_bitmap_buffer[PMM_BITMAP_MAX_BYTES];
pmm_t g_pmm;

/* ── PMM 初始化 ───────────────────────────────────────────────────── */

/*
   pmm_init - 初始化物理内存管理器
   @pmm:           物理内存管理器实例
   @start_addr:    物理内存起始地址
   @size:          内存大小(字节)
   @bitmap_buffer: 位图缓冲区
   @bitmap_size:   位图大小(字节)
*/
void pmm_init(pmm_t *pmm, uint64_t start_addr, uint64_t size, uint8_t *bitmap_buffer, size_t bitmap_size)
{
    assert(pmm != NULL);
    assert(bitmap_buffer != NULL);
    assert(size > 0);

    /* 初始化基本信息 */
    pmm->start_addr = start_addr;
    pmm->total_size = size;
    pmm->page_size = PAGE_SIZE;
    pmm->total_pages = size / PAGE_SIZE;
    pmm->free_pages = pmm->total_pages;
    pmm->reserved_count = 0;
    pmm->reserved_pages = 0;
    pmm->alloc_hint = 0;

    /*
       bitmap_init 的 size 单位是 bit。位图缓冲区按 PMM_BITMAP_MAX_BYTES
       给(最多 4GB RAM), 但逻辑容量必须是 total_pages —— 多出来的
       尾部位对 bitmap_find_* 来说全是"空闲", next-fit 一旦越过 total_pages
       就会把 RAM 之外的页当成可分配的页交出去。
    */
    assert(bitmap_size >= (pmm->total_pages + 7) / 8);
    bitmap_init(&pmm->bitmap, bitmap_buffer, pmm->total_pages);

    /* 初始化自旋锁 */
    spinlock_init(&pmm->lock);

    /* 原来是六行结构体 dump —— 合并成一行：这是操作者真正要的那条内存事实 */
    KLOG_INFO("PMM: base=0x%llx size=%lluMB page=%uKB pages=%llu free=%llu\n", start_addr, size / (1024 * 1024),
              pmm->page_size / 1024, pmm->total_pages, pmm->free_pages);
}

/* ── 页面分配 ───────────────────────────────────────────────────── */

/*
   pmm_alloc_pages - 分配连续的物理页面
   @pmm:        物理内存管理器
   @page_count: 需要分配的页面数

   返回：分配的物理地址, 失败返回 0
*/
uint64_t pmm_alloc_pages(pmm_t *pmm, uint32_t page_count)
{
    assert(pmm != NULL);
    assert(page_count > 0);

    uint64_t paddr = 0;

    spin_lock(&pmm->lock);

    /*
       查找连续的空闲页面：先从上一次的落点找(next-fit), 失败再回绕。
       位图的逻辑容量在 pmm_init() 里就是 total_pages, 所以查找结果天然
       落在 [0, total_pages) 内, 不需要再按 total_pages 复核。
    */
    size_t page_index = bitmap_find_contiguous_free_from(&pmm->bitmap, page_count, pmm->alloc_hint);
    if (page_index == (size_t)-1 && pmm->alloc_hint != 0)
        page_index = bitmap_find_contiguous_free(&pmm->bitmap, page_count);
    if (page_index != (size_t)-1) {
        /* 标记页面为已分配 */
        bitmap_set_range(&pmm->bitmap, page_index, page_count);

        /* 计算物理地址 */
        paddr = pmm->start_addr + page_index * pmm->page_size;

        /* 更新空闲页面计数与 next-fit 提示 */
        pmm->free_pages -= page_count;
        pmm->alloc_hint = page_index + page_count;
    }

    spin_unlock(&pmm->lock);

    /*
       日志放在解锁之后：klog 会持 g_klog_lock(关中断)把整条消息逐字符
       轮询写 UART, 在 pmm->lock —— 全内核最热的锁 —— 里做这件事, 等于让
       每个分配者多等一次串口输出。paddr == 0 即分配失败。
    */
    if (paddr == 0)
        KLOG_ERROR("PMM: failed to allocate %u pages\n", page_count);

    return paddr;
}

/* ── 页面释放 ───────────────────────────────────────────────────── */

/*
   pmm_free_pages - 释放物理页面
   @pmm:        物理内存管理器
   @paddr:      物理地址
   @page_count: 页面数
*/
bool pmm_free_pages(pmm_t *pmm, uint64_t paddr, uint32_t page_count)
{
    assert(pmm != NULL);
    assert(page_count > 0);

    if ((paddr & (pmm->page_size - 1)) != 0) {
        KLOG_ERROR("PMM: free address is not page-aligned: 0x%llx\n", paddr);
        return false;
    }

    /* 检查地址范围 */
    if (paddr < pmm->start_addr || paddr >= pmm->start_addr + pmm->total_size) {
        KLOG_ERROR("PMM: invalid free address: 0x%llx\n", paddr);
        return false;
    }

    uint64_t page_index = (paddr - pmm->start_addr) / pmm->page_size;
    if (page_index >= pmm->total_pages || page_count > pmm->total_pages - page_index) {
        KLOG_ERROR("PMM: invalid free request: paddr=0x%llx count=%u\n", paddr, page_count);
        return false;
    }

    uint64_t free_end = paddr + (uint64_t)page_count * pmm->page_size;
    const char *reserved_tag = NULL;
    bool double_free = false;
    uint64_t double_free_paddr = 0;

    spin_lock(&pmm->lock);

    /*
       保留区检查必须在清位之前：位图里"保留"和"已分配"都是 1, 单看位图
       分不出来。误 free 内核/rootfs 页面时如果照常清位, 就等于把内核代码
       重新交给了分配器 —— 这里拒绝整笔释放, 并且一个 bit 都不动。
    */
    for (unsigned i = 0; i < pmm->reserved_count; i++) {
        if (paddr < pmm->reserved[i].end && free_end > pmm->reserved[i].start) {
            reserved_tag = pmm->reserved[i].tag;
            break;
        }
    }

    if (reserved_tag == NULL) {
        /*
           整笔检查：只要有一页本来就是空闲的, 就是 double free。此时
           一个 bit 都不清 —— 旧实现会"释放一半, 把另一半当成已释放",
           调用方拿不到任何错误, free_pages 的账从此漂移。
        */
        for (uint32_t i = 0; i < page_count; i++) {
            if (!bitmap_test(&pmm->bitmap, page_index + i)) {
                double_free = true;
                double_free_paddr = pmm->start_addr + (page_index + i) * pmm->page_size;
                break;
            }
        }

        if (!double_free) {
            for (uint32_t i = 0; i < page_count; i++)
                bitmap_clear(&pmm->bitmap, page_index + i);
            pmm->free_pages += page_count;
        }
    }

    spin_unlock(&pmm->lock);

    /* 日志一律在解锁之后打(理由同 pmm_alloc_pages) */
    if (reserved_tag != NULL) {
        KLOG_ERROR("PMM: refusing to free reserved region '%s': paddr=0x%llx count=%u\n", reserved_tag, paddr,
                   page_count);
        return false;
    }
    if (double_free) {
        KLOG_ERROR("PMM: double free: paddr=0x%llx count=%u (page 0x%llx already free); refusing whole range\n", paddr,
                   page_count, double_free_paddr);
        return false;
    }
    return true;
}

/* ── 保留区 ─────────────────────────────────────────────────────── */

/*
   pmm_reserve - 预留一段物理内存
   @pmm:        物理内存管理器
   @start_addr: 起始物理地址(含)
   @end_addr:   结束物理地址(不含)
   @tag:        保留区名字(静态生命周期)

   位图标 1, 并记录到保留区表。之后 pmm_free_pages() 命中这张表会拒绝释放
   且不清位 —— 位图本身区分不了"已分配"和"保留"。
*/
bool pmm_reserve(pmm_t *pmm, uint64_t start_addr, uint64_t end_addr, const char *tag)
{
    assert(pmm != NULL);
    assert(tag != NULL);

    /* 空区间：没有东西要预留, 不算失败(rootfs 大小为 0 的配置会走到) */
    if (end_addr <= start_addr)
        return true;

    /* 半开区间向外取整：调用方给的首尾不必页对齐 */
    start_addr = ALIGN_DOWN(start_addr, pmm->page_size);
    end_addr = ALIGN_UP(end_addr, pmm->page_size);

    if (start_addr < pmm->start_addr || end_addr > pmm->start_addr + pmm->total_size) {
        KLOG_ERROR("PMM: reserve '%s' out of range: 0x%llx - 0x%llx\n", tag, start_addr, end_addr);
        KLOG_ERROR("  PMM range: 0x%llx - 0x%llx\n", pmm->start_addr, pmm->start_addr + pmm->total_size);
        return false;
    }

    uint64_t start_page = (start_addr - pmm->start_addr) / pmm->page_size;
    uint64_t end_page = (end_addr - pmm->start_addr) / pmm->page_size;
    uint64_t newly_marked = 0;

    spin_lock(&pmm->lock);

    if (pmm->reserved_count >= PMM_MAX_RESERVED_RANGES) {
        spin_unlock(&pmm->lock);
        KLOG_ERROR("PMM: reserved range table full, cannot reserve '%s'\n", tag);
        return false;
    }

    pmm_reserved_t *resv = &pmm->reserved[pmm->reserved_count++];
    resv->start = start_addr;
    resv->end = end_addr;
    resv->tag = tag;

    /* 仅统计 0->1 的位：区间重叠时 reserved_pages 不会重复计数 */
    for (uint64_t i = start_page; i < end_page; i++) {
        if (!bitmap_test(&pmm->bitmap, i)) {
            bitmap_set(&pmm->bitmap, i);
            pmm->free_pages--;
            newly_marked++;
        }
    }
    pmm->reserved_pages += newly_marked;

    spin_unlock(&pmm->lock);

    KLOG_MM("PMM: reserved '%s': 0x%llx - 0x%llx (%llu pages, %llu new)\n", tag, start_addr, end_addr,
            end_page - start_page, newly_marked);

    return true;
}

/*
   pmm_initialize - 初始化物理内存管理器
   @kernel_start_phys: 内核镜像起始物理地址(含)
   @kernel_end_phys:   内核镜像结束物理地址(不含)

   必须在 kernel_main 中早期调用, 在任何内存分配之前。

   内核镜像区间由 boot 层(kernel/main.c)从链接符号换算成物理地址后传入。
   PMM 只认物理地址 —— 高半区/虚拟地址布局的知识不在这里。
*/
void pmm_initialize(uint64_t kernel_start_phys, uint64_t kernel_end_phys)
{
    KLOG_MM("=== Initializing Physical Memory Manager ===\n");

    /* 初始化 PMM */
    pmm_init(&g_pmm, PMM_RAM_BASE, PMM_RAM_SIZE, g_pmm_bitmap_buffer, sizeof(g_pmm_bitmap_buffer));

    /* 预留内核镜像：失败等于允许分配器把内核代码发出去, 不能继续启动 */
    KLOG_MM("Reserving kernel image...\n");
    if (!pmm_reserve(&g_pmm, kernel_start_phys, kernel_end_phys, "kernel-image")) {
        KLOG_ERROR("PMM: failed to reserve kernel image (0x%llx - 0x%llx) — refusing to boot\n", kernel_start_phys,
                   kernel_end_phys);
        assert_always(0);
    }

    /*
       运行时保留区(由 platform_conf_scan() 从静态平台配置读取)。
       platform.conf 里的 stop 是闭区间, 这里转换成半开区间 [start, stop+1)。
    */
    for (int i = 0; i < g_pmm_resv_count; i++) {
        uint64_t start = (uint64_t)g_pmm_reserves[i].start;
        uint64_t end = (uint64_t)g_pmm_reserves[i].end + 1;

        KLOG_MM("Reserving PMM extra region(%s): 0x%llx - 0x%llx\n", g_pmm_reserves[i].tag, start, end);
        if (!pmm_reserve(&g_pmm, start, end, g_pmm_reserves[i].tag))
            KLOG_ERROR("PMM: failed to reserve platform region '%s'\n", g_pmm_reserves[i].tag);
    }

    /* 预留 rootfs 物理区域, 防止 PMM 将其分配出去（平台内存布局，与具体块驱动无关） */
    uintptr_t rootfs_start = g_mem_rootfs_base;
    uintptr_t rootfs_end = g_mem_rootfs_base + g_mem_rootfs_size;
    KLOG_MM("Reserving rootfs region: 0x%llx - 0x%llx\n", (uint64_t)rootfs_start, (uint64_t)rootfs_end);
    if (!pmm_reserve(&g_pmm, rootfs_start, rootfs_end, "rootfs"))
        KLOG_ERROR("PMM: failed to reserve rootfs region\n");

    /* 一行说清最终账目：内核/rootfs/平台保留一共占了多少, 还剩多少 */
    KLOG_INFO("PMM: %u reserved regions, %llu pages reserved, %llu pages free\n", g_pmm.reserved_count,
              g_pmm.reserved_pages, g_pmm.free_pages);

    KLOG_MM("PMM initialization completed\n");
    KLOG_MM("  g_pmm = %p\n", &g_pmm);
}
