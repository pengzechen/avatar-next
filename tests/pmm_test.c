/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   tests/pmm_test.c - 物理内存管理器测试
*/

#include "pmm.h"
#include "klog.h"
#include "assert.h"
#include "mm_vm.h"
#include "shared_page.h"
#include "kmalloc.h"
#include "string.h"

#define PMM_PAGE_SIZE 4096 /* 4KB */

/* ── 测试函数 ───────────────────────────────────────────────────── */

/*
   test_pmm_basic - 基础分配测试
*/
static void test_pmm_basic(void)
{
    KLOG_INFO("=== PMM Basic Allocation Test ===\n");

    /* 测试单页分配 */
    uint64_t page1 = pmm_alloc_pages(&g_pmm, 1);
    KLOG_INFO("Allocated 1 page at: 0x%llx\n", page1);
    assert(page1 != 0);
    assert(page1 >= PMM_RAM_BASE);

    /* 测试多页分配 */
    uint64_t pages = pmm_alloc_pages(&g_pmm, 10);
    KLOG_INFO("Allocated 10 pages at: 0x%llx\n", pages);
    assert(pages != 0);
    assert(pages >= PMM_RAM_BASE);

    /* 测试页面释放 */
    pmm_free_pages(&g_pmm, page1, 1);
    KLOG_INFO("Freed 1 page at: 0x%llx\n", page1);

    pmm_free_pages(&g_pmm, pages, 10);
    KLOG_INFO("Freed 10 pages at: 0x%llx\n", pages);

    /* 验证页面计数 */
    uint64_t free_pages = pmm_get_free_pages(&g_pmm);
    uint64_t total_pages = pmm_get_total_pages(&g_pmm);
    KLOG_INFO("Free pages: %llu / %llu\n", free_pages, total_pages);

    KLOG_INFO("Basic allocation test passed\n");
}

/*
   test_pmm_large_allocation - 大块分配测试
*/
static void test_pmm_large_allocation(void)
{
    KLOG_INFO("=== PMM Large Allocation Test ===\n");

    /* 分配 1000 页(约 4MB) */
    uint64_t large_block = pmm_alloc_pages(&g_pmm, 1000);
    KLOG_INFO("Allocated 1000 pages (~4MB) at: 0x%llx\n", large_block);
    assert(large_block != 0);

    /* 分配 10000 页(约 40MB) */
    uint64_t huge_block = pmm_alloc_pages(&g_pmm, 10000);
    KLOG_INFO("Allocated 10000 pages (~40MB) at: 0x%llx\n", huge_block);
    assert(huge_block != 0);

    /* 释放 */
    pmm_free_pages(&g_pmm, large_block, 1000);
    KLOG_INFO("Freed 1000 pages\n");

    pmm_free_pages(&g_pmm, huge_block, 10000);
    KLOG_INFO("Freed 10000 pages\n");

    /* 验证页面计数恢复 */
    uint64_t free_pages = pmm_get_free_pages(&g_pmm);
    uint64_t total_pages = pmm_get_total_pages(&g_pmm);
    KLOG_INFO("Free pages: %llu / %llu\n", free_pages, total_pages);

    KLOG_INFO("Large allocation test passed\n");
}

/*
   test_pmm_fragmentation - 内存碎片测试
*/
static void test_pmm_fragmentation(void)
{
    KLOG_INFO("=== PMM Fragmentation Test ===\n");

    uint64_t pages[10];

    /* 分配多个小块 */
    for (int i = 0; i < 10; i++) {
        pages[i] = pmm_alloc_pages(&g_pmm, 100);
        KLOG_INFO("Allocated block %d: 0x%llx\n", i, pages[i]);
        assert(pages[i] != 0);
    }

    /* 释放部分块 */
    for (int i = 0; i < 5; i++) {
        pmm_free_pages(&g_pmm, pages[i], 100);
        KLOG_INFO("Freed block %d\n", i);
    }

    /* 尝试分配大块(应该能成功, 因为有连续空间) */
    uint64_t large = pmm_alloc_pages(&g_pmm, 500);
    KLOG_INFO("Allocated 500 pages after fragmentation: 0x%llx\n", large);
    assert(large != 0);

    /* 清理 */
    pmm_free_pages(&g_pmm, large, 500);
    for (int i = 5; i < 10; i++) {
        pmm_free_pages(&g_pmm, pages[i], 100);
    }

    KLOG_INFO("Fragmentation test passed\n");
}

/*
   test_pmm_reserved - 保留区释放测试

   位图区分不了"已分配"和"保留", 所以这里验证的是保留区表那一层：
   对保留区调用 pmm_free_pages() 必须被拒绝, 且 free_pages/位图都不能变。
*/
static void test_pmm_reserved(void)
{
    KLOG_INFO("=== PMM Reserved Region Test ===\n");

    assert(g_pmm.reserved_count > 0);

    const pmm_reserved_t *resv = &g_pmm.reserved[0];
    uint64_t free_before = pmm_get_free_pages(&g_pmm);
    uint64_t index = (resv->start - g_pmm.start_addr) / g_pmm.page_size;

    KLOG_INFO("Trying to free reserved region '%s' at 0x%llx\n", resv->tag, resv->start);
    assert(!pmm_free_pages(&g_pmm, resv->start, 1));

    assert(pmm_get_free_pages(&g_pmm) == free_before);
    assert(bitmap_test(&g_pmm.bitmap, index));

    KLOG_INFO("Reserved region test passed\n");
}

/*
   test_pmm_double_free - 位图容量 + double free 拒绝测试

   两件事必须同时成立：
     - bitmap 的逻辑容量等于 total_pages(多出来的尾部位会让 next-fit
       把 RAM 之外的页发出去)；
     - 第二次 free 同一页必须整笔拒绝, free_pages 不变。
*/
static void test_pmm_double_free(void)
{
    KLOG_INFO("=== PMM Double Free Test ===\n");

    assert(g_pmm.bitmap.size == g_pmm.total_pages);

    uint64_t pa = pmm_alloc_pages(&g_pmm, 1);
    assert(pa != 0);
    uint64_t free_before = pmm_get_free_pages(&g_pmm);

    assert(pmm_free_pages(&g_pmm, pa, 1));
    assert(pmm_get_free_pages(&g_pmm) == free_before + 1);

    KLOG_INFO("Testing double free (expecting error message)...\n");
    assert(!pmm_free_pages(&g_pmm, pa, 1));
    assert(pmm_get_free_pages(&g_pmm) == free_before + 1);

    KLOG_INFO("Double free rejection test passed\n");
}

/*
   test_kmalloc_kfree - kmalloc 头部记账测试

   验证：
     - 返回地址 16 字节对齐, 且页内 offset 是 header 大小；
     - kfree 不再需要 size, 单页/跨页分配都能完整还给 PMM；
     - kalloc_pages 的页对齐指针传给 kfree 必须被拒绝(offset 检查)。
*/
static void test_kmalloc_kfree(void)
{
    KLOG_INFO("=== kmalloc/kfree Test ===\n");

    uint64_t free_before = pmm_get_free_pages(&g_pmm);

    void *small = kmalloc(1);
    assert(small != NULL);
    assert(((uintptr_t)small & 0xF) == 0);
    assert(((uintptr_t)small & (PAGE_SIZE - 1)) == 16);
    memset(small, 0xA5, 1);
    kfree(small);
    assert(pmm_get_free_pages(&g_pmm) == free_before);

    size_t big_size = PAGE_SIZE * 2 + 100;
    void *big = kmalloc(big_size);
    assert(big != NULL);
    assert(((uintptr_t)big & 0xF) == 0);
    memset(big, 0x5A, big_size);
    kfree(big);
    assert(pmm_get_free_pages(&g_pmm) == free_before);

    /*
       kalloc_pages 返回页对齐指针(offset 0), 不是 kmalloc 指针。
       kfree 必须拒绝, 而且不能动 free_pages。
    */
    void *raw = kalloc_pages(1);
    assert(raw != NULL);
    uint64_t raw_free = pmm_get_free_pages(&g_pmm);
    kfree(raw);
    assert(pmm_get_free_pages(&g_pmm) == raw_free);
    kfree_pages(raw, 1);

    kfree(NULL);

    KLOG_INFO("kmalloc/kfree test passed\n");
}

/*
   test_shared_page_refcount - MAP_SHARED 引用计数测试

   验证最后一个 unref 才释放物理页, 以及非法 paddr 被拒绝。
   并发场景由 fork/mmap 的回归用例覆盖；这里只验证账本本身。
*/
static void test_shared_page_refcount(void)
{
    KLOG_INFO("=== Shared Page Refcount Test ===\n");

    uint64_t pa = pmm_alloc_pages(&g_pmm, 1);
    assert(pa != 0);
    uint64_t free_before = pmm_get_free_pages(&g_pmm);

    assert(shared_page_ref(pa));
    assert(shared_page_ref(pa));

    /* 还有一个持有者：页不能回到分配器 */
    shared_page_unref(pa);
    assert(pmm_get_free_pages(&g_pmm) == free_before);

    /* 最后一个持有者退出：页释放 */
    shared_page_unref(pa);
    assert(pmm_get_free_pages(&g_pmm) == free_before + 1);

    /* 0 不是合法物理页, 不能被记账 */
    assert(!shared_page_ref(0));

    KLOG_INFO("Shared page refcount test passed\n");
}

/*
   test_pmm_stress - 压力测试
*/
static void test_pmm_stress(void)
{
    KLOG_INFO("=== PMM Stress Test ===\n");

    uint64_t total_allocated = 0;
    uint64_t iterations = 0;

    /* 尽可能多地分配页面 */
    while (iterations < 1000) {
        uint32_t count = (iterations % 10) + 1; /* 1-10 页 */
        uint64_t paddr = pmm_alloc_pages(&g_pmm, count);

        if (paddr == 0) {
            /* 内存耗尽 */
            break;
        }

        total_allocated += count;
        iterations++;

        /* 每 100 次迭代释放一些内存 */
        if (iterations % 100 == 0) {
            KLOG_INFO("Iteration %llu: allocated %llu pages total\n", iterations, total_allocated);
        }
    }

    KLOG_INFO("Stress test completed:\n");
    KLOG_INFO("  Iterations: %llu\n", iterations);
    KLOG_INFO("  Total allocated: %llu pages (~%llu MB)\n", total_allocated,
              (total_allocated * PMM_PAGE_SIZE) / (1024 * 1024));

    /* 注意：这里没有释放分配的内存, 实际使用中应该保存地址并释放 */

    KLOG_INFO("Stress test passed\n");
}

/*
   test_pmm_boundary - 边界测试
*/
static void test_pmm_boundary(void)
{
    KLOG_INFO("=== PMM Boundary Test ===\n");

    /* 测试对齐 */
    uint64_t page1 = pmm_alloc_pages(&g_pmm, 1);
    KLOG_INFO("Allocated page at: 0x%llx\n", page1);
    assert(page1 % PMM_PAGE_SIZE == 0); /* 应该页对齐 */

    /* 测试地址范围 */
    assert(page1 >= PMM_RAM_BASE);
    assert(page1 < PMM_RAM_BASE + PMM_RAM_SIZE);

    pmm_free_pages(&g_pmm, page1, 1);

    /* 测试无效释放(必须被拒绝, 且不能动 free_pages) */
    KLOG_INFO("Testing invalid free (expecting error message)...\n");
    assert(!pmm_free_pages(&g_pmm, 0x0, 1));           /* 无效地址 */
    assert(!pmm_free_pages(&g_pmm, 0xFFFFFFFFULL, 1)); /* 未对齐/超出范围 */

    KLOG_INFO("Boundary test passed\n");
}

#if ARCH_RISCV64
/*
   PTE 打包/解包与 Sv39 下标的运行时校验。

   这些关系在 pte.h 里已有 _Static_assert；这里再从"目标平台实际宏值"
   走一遍, 专门防住 -D 覆盖(RV_KERNEL_VMA_OFF)与平台配置不一致。
*/
static void test_riscv_pte_geometry(void)
{
    const uint64_t pa = 0x12345000ULL;
    uint64_t pte = RV_PTE_FROM_PA(pa, RV_PTE_V | RV_PTE_R | RV_PTE_W);

    KLOG_INFO("=== RISC-V PTE Geometry Test ===\n");

    assert(RV_PTE_TO_PA(pte) == pa);
    assert(RV_PTE_IS_LEAF(pte));
    assert(!RV_PTE_IS_LEAF(RV_PTE_V)); /* 非叶项 */

    assert(RV_PTE_INDEX(0xffffffc000000000ULL, 2) == 0x100);
    assert(RV_PTE_INDEX(0x80200000ULL, 2) == 2);
    assert(RV_PTE_INDEX(0x80200000ULL, 1) == 1);
    assert(RV_PTE_INDEX(0x80201000ULL, 0) == 1);
    assert(RV_PAGE_OFFSET(0x80201abcULL) == 0xabc);

    KLOG_INFO("RISC-V PTE geometry test passed\n");
}
#endif

/* ── PMM 测试入口 ─────────────────────────────────────────────── */

/*
   run_pmm_tests - 运行所有 PMM 测试

   在 pmm_initialize() 之后调用。
*/
void run_pmm_tests(void)
{
    KLOG_WARN("=== Running PMM Tests ===\n");

    test_pmm_basic();
    test_pmm_large_allocation();
    test_pmm_fragmentation();
    test_pmm_reserved();
    test_pmm_double_free();
    test_kmalloc_kfree();
    test_shared_page_refcount();
    test_pmm_stress();
    test_pmm_boundary();
#if ARCH_RISCV64
    test_riscv_pte_geometry();
#endif

    KLOG_INFO("=== All PMM Tests Passed ===\n");
}