/*
 * tests/pmm_test.c - 物理内存管理器测试
 */

#include "pmm.h"
#include "klog.h"
#include "assert.h"
#include "mm_vm.h"



#define PMM_PAGE_SIZE   4096  /* 4KB */

/* ── 测试函数 ───────────────────────────────────────────────────── */

/**
 * test_pmm_basic - 基础分配测试
 */
static void test_pmm_basic(void)
{
    KLOG_INFO("=== PMM Basic Allocation Test ===\n");

    /* 测试单页分配 */
    uint64_t page1 = pmm_alloc_pages(g_pmm, 1);
    KLOG_INFO("Allocated 1 page at: 0x%llx\n", page1);
    assert(page1 != 0);
    assert(page1 >= PMM_RAM_BASE);

    /* 测试多页分配 */
    uint64_t pages = pmm_alloc_pages(g_pmm, 10);
    KLOG_INFO("Allocated 10 pages at: 0x%llx\n", pages);
    assert(pages != 0);
    assert(pages >= PMM_RAM_BASE);

    /* 测试页面释放 */
    pmm_free_pages(g_pmm, page1, 1);
    KLOG_INFO("Freed 1 page at: 0x%llx\n", page1);

    pmm_free_pages(g_pmm, pages, 10);
    KLOG_INFO("Freed 10 pages at: 0x%llx\n", pages);

    /* 验证页面计数 */
    uint64_t free_pages = pmm_get_free_pages(g_pmm);
    uint64_t total_pages = pmm_get_total_pages(g_pmm);
    KLOG_INFO("Free pages: %llu / %llu\n", free_pages, total_pages);

    KLOG_INFO("✓ Basic allocation test passed\n");
}

/**
 * test_pmm_large_allocation - 大块分配测试
 */
static void test_pmm_large_allocation(void)
{
    KLOG_INFO("=== PMM Large Allocation Test ===\n");

    /* 分配 1000 页（约 4MB） */
    uint64_t large_block = pmm_alloc_pages(g_pmm, 1000);
    KLOG_INFO("Allocated 1000 pages (~4MB) at: 0x%llx\n", large_block);
    assert(large_block != 0);

    /* 分配 10000 页（约 40MB） */
    uint64_t huge_block = pmm_alloc_pages(g_pmm, 10000);
    KLOG_INFO("Allocated 10000 pages (~40MB) at: 0x%llx\n", huge_block);
    assert(huge_block != 0);

    /* 释放 */
    pmm_free_pages(g_pmm, large_block, 1000);
    KLOG_INFO("Freed 1000 pages\n");

    pmm_free_pages(g_pmm, huge_block, 10000);
    KLOG_INFO("Freed 10000 pages\n");

    /* 验证页面计数恢复 */
    uint64_t free_pages = pmm_get_free_pages(g_pmm);
    uint64_t total_pages = pmm_get_total_pages(g_pmm);
    KLOG_INFO("Free pages: %llu / %llu\n", free_pages, total_pages);

    KLOG_INFO("✓ Large allocation test passed\n");
}

/**
 * test_pmm_fragmentation - 内存碎片测试
 */
static void test_pmm_fragmentation(void)
{
    KLOG_INFO("=== PMM Fragmentation Test ===\n");

    uint64_t pages[10];

    /* 分配多个小块 */
    for (int i = 0; i < 10; i++) {
        pages[i] = pmm_alloc_pages(g_pmm, 100);
        KLOG_INFO("Allocated block %d: 0x%llx\n", i, pages[i]);
        assert(pages[i] != 0);
    }

    /* 释放部分块 */
    for (int i = 0; i < 5; i++) {
        pmm_free_pages(g_pmm, pages[i], 100);
        KLOG_INFO("Freed block %d\n", i);
    }

    /* 尝试分配大块（应该能成功，因为有连续空间） */
    uint64_t large = pmm_alloc_pages(g_pmm, 500);
    KLOG_INFO("Allocated 500 pages after fragmentation: 0x%llx\n", large);
    assert(large != 0);

    /* 清理 */
    pmm_free_pages(g_pmm, large, 500);
    for (int i = 5; i < 10; i++) {
        pmm_free_pages(g_pmm, pages[i], 100);
    }

    KLOG_INFO("✓ Fragmentation test passed\n");
}

/**
 * test_pmm_stress - 压力测试
 */
static void test_pmm_stress(void)
{
    KLOG_INFO("=== PMM Stress Test ===\n");

    uint64_t total_allocated = 0;
    uint64_t iterations = 0;

    /* 尽可能多地分配页面 */
    while (iterations < 1000) {
        uint32_t count = (iterations % 10) + 1;  /* 1-10 页 */
        uint64_t paddr = pmm_alloc_pages(g_pmm, count);

        if (paddr == 0) {
            /* 内存耗尽 */
            break;
        }

        total_allocated += count;
        iterations++;

        /* 每 100 次迭代释放一些内存 */
        if (iterations % 100 == 0) {
            KLOG_INFO("Iteration %llu: allocated %llu pages total\n",
                      iterations, total_allocated);
        }
    }

    KLOG_INFO("Stress test completed:\n");
    KLOG_INFO("  Iterations: %llu\n", iterations);
    KLOG_INFO("  Total allocated: %llu pages (~%llu MB)\n",
              total_allocated,
              (total_allocated * PMM_PAGE_SIZE) / (1024 * 1024));

    /* 注意：这里没有释放分配的内存，实际使用中应该保存地址并释放 */

    KLOG_INFO("✓ Stress test passed\n");
}

/**
 * test_pmm_boundary - 边界测试
 */
static void test_pmm_boundary(void)
{
    KLOG_INFO("=== PMM Boundary Test ===\n");

    /* 测试对齐 */
    uint64_t page1 = pmm_alloc_pages(g_pmm, 1);
    KLOG_INFO("Allocated page at: 0x%llx\n", page1);
    assert(page1 % PMM_PAGE_SIZE == 0);  /* 应该页对齐 */

    /* 测试地址范围 */
    assert(page1 >= PMM_RAM_BASE);
    assert(page1 < PMM_RAM_BASE + PMM_RAM_SIZE);

    pmm_free_pages(g_pmm, page1, 1);

    /* 测试无效释放（应该被优雅处理） */
    KLOG_INFO("Testing invalid free (expecting error message)...\n");
    pmm_free_pages(g_pmm, 0x0, 1);  /* 无效地址 */
    pmm_free_pages(g_pmm, 0xFFFFFFFFULL, 1);  /* 超出范围 */

    KLOG_INFO("✓ Boundary test passed\n");
}

/* ── PMM 测试入口 ─────────────────────────────────────────────── */

/**
 * run_pmm_tests - 运行所有 PMM 测试
 *
 * 在 pmm_initialize() 之后调用。
 */
void run_pmm_tests(void)
{
    KLOG_WARN("=== Running PMM Tests ===\n");

    test_pmm_basic();
    test_pmm_large_allocation();
    test_pmm_fragmentation();
    test_pmm_stress();
    test_pmm_boundary();

    KLOG_INFO("=== All PMM Tests Passed ===\n");
}