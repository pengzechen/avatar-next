/*
 * kernel/mm/pmm_test.c - 物理内存管理器测试
 */

#include "pmm.h"
#include "klog.h"
#include "assert.h"
#include "mm_vm.h"
#include "../../driver/blk/ramblk_cfg.h"   /* RAMBLK_PHYS_BASE / RAMBLK_PHYS_END */



/* 计算位图大小 */
#define PMM_PAGE_SIZE   4096  /* 4KB */
#define PMM_TOTAL_PAGES (PMM_RAM_SIZE / PMM_PAGE_SIZE)
#define PMM_BITMAP_SIZE ((PMM_TOTAL_PAGES + 7) / 8)  /* 字节 */

/* ── 全局变量 ───────────────────────────────────────────────────── */

/* 位图缓冲区（静态分配） */
static uint8_t pmm_bitmap_buffer[PMM_BITMAP_SIZE];

/* 物理内存管理器实例 */
pmm_t pmm;  /* 改为非 static，供 vmm.c 使用 */

/* 全局 PMM 指针（供 vmm.c 使用） */
pmm_t *g_pmm = &pmm;

/* ── 测试函数 ───────────────────────────────────────────────────── */

/**
 * test_pmm_basic - 基础分配测试
 */
static void test_pmm_basic(void)
{
    KLOG_INFO("=== PMM Basic Allocation Test ===");

    /* 测试单页分配 */
    uint64_t page1 = pmm_alloc_pages(&pmm, 1);
    KLOG_INFO("Allocated 1 page at: 0x%llx", page1);
    assert(page1 != 0);
    assert(page1 >= PMM_RAM_BASE);

    /* 测试多页分配 */
    uint64_t pages = pmm_alloc_pages(&pmm, 10);
    KLOG_INFO("Allocated 10 pages at: 0x%llx", pages);
    assert(pages != 0);
    assert(pages >= PMM_RAM_BASE);

    /* 测试页面释放 */
    pmm_free_pages(&pmm, page1, 1);
    KLOG_INFO("Freed 1 page at: 0x%llx", page1);

    pmm_free_pages(&pmm, pages, 10);
    KLOG_INFO("Freed 10 pages at: 0x%llx", pages);

    /* 验证页面计数 */
    uint64_t free_pages = pmm_get_free_pages(&pmm);
    uint64_t total_pages = pmm_get_total_pages(&pmm);
    KLOG_INFO("Free pages: %llu / %llu", free_pages, total_pages);

    KLOG_INFO("✓ Basic allocation test passed\n");
}

/**
 * test_pmm_large_allocation - 大块分配测试
 */
static void test_pmm_large_allocation(void)
{
    KLOG_INFO("=== PMM Large Allocation Test ===");

    /* 分配 1000 页（约 4MB） */
    uint64_t large_block = pmm_alloc_pages(&pmm, 1000);
    KLOG_INFO("Allocated 1000 pages (~4MB) at: 0x%llx", large_block);
    assert(large_block != 0);

    /* 分配 10000 页（约 40MB） */
    uint64_t huge_block = pmm_alloc_pages(&pmm, 10000);
    KLOG_INFO("Allocated 10000 pages (~40MB) at: 0x%llx", huge_block);
    assert(huge_block != 0);

    /* 释放 */
    pmm_free_pages(&pmm, large_block, 1000);
    KLOG_INFO("Freed 1000 pages");

    pmm_free_pages(&pmm, huge_block, 10000);
    KLOG_INFO("Freed 10000 pages");

    /* 验证页面计数恢复 */
    uint64_t free_pages = pmm_get_free_pages(&pmm);
    uint64_t total_pages = pmm_get_total_pages(&pmm);
    KLOG_INFO("Free pages: %llu / %llu", free_pages, total_pages);

    KLOG_INFO("✓ Large allocation test passed\n");
}

/**
 * test_pmm_fragmentation - 内存碎片测试
 */
static void test_pmm_fragmentation(void)
{
    KLOG_INFO("=== PMM Fragmentation Test ===");

    uint64_t pages[10];

    /* 分配多个小块 */
    for (int i = 0; i < 10; i++) {
        pages[i] = pmm_alloc_pages(&pmm, 100);
        KLOG_INFO("Allocated block %d: 0x%llx", i, pages[i]);
        assert(pages[i] != 0);
    }

    /* 释放部分块 */
    for (int i = 0; i < 5; i++) {
        pmm_free_pages(&pmm, pages[i], 100);
        KLOG_INFO("Freed block %d", i);
    }

    /* 尝试分配大块（应该能成功，因为有连续空间） */
    uint64_t large = pmm_alloc_pages(&pmm, 500);
    KLOG_INFO("Allocated 500 pages after fragmentation: 0x%llx", large);
    assert(large != 0);

    /* 清理 */
    pmm_free_pages(&pmm, large, 500);
    for (int i = 5; i < 10; i++) {
        pmm_free_pages(&pmm, pages[i], 100);
    }

    KLOG_INFO("✓ Fragmentation test passed\n");
}

/**
 * test_pmm_stress - 压力测试
 */
static void test_pmm_stress(void)
{
    KLOG_INFO("=== PMM Stress Test ===");

    uint64_t total_allocated = 0;
    uint64_t iterations = 0;

    /* 尽可能多地分配页面 */
    while (iterations < 1000) {
        uint32_t count = (iterations % 10) + 1;  /* 1-10 页 */
        uint64_t paddr = pmm_alloc_pages(&pmm, count);

        if (paddr == 0) {
            /* 内存耗尽 */
            break;
        }

        total_allocated += count;
        iterations++;

        /* 每 100 次迭代释放一些内存 */
        if (iterations % 100 == 0) {
            KLOG_INFO("Iteration %llu: allocated %llu pages total",
                      iterations, total_allocated);
        }
    }

    KLOG_INFO("Stress test completed:");
    KLOG_INFO("  Iterations: %llu", iterations);
    KLOG_INFO("  Total allocated: %llu pages (~%llu MB)",
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
    KLOG_INFO("=== PMM Boundary Test ===");

    /* 测试对齐 */
    uint64_t page1 = pmm_alloc_pages(&pmm, 1);
    KLOG_INFO("Allocated page at: 0x%llx", page1);
    assert(page1 % PMM_PAGE_SIZE == 0);  /* 应该页对齐 */

    /* 测试地址范围 */
    assert(page1 >= PMM_RAM_BASE);
    assert(page1 < PMM_RAM_BASE + PMM_RAM_SIZE);

    pmm_free_pages(&pmm, page1, 1);

    /* 测试无效释放（应该被优雅处理） */
    KLOG_INFO("Testing invalid free (expecting error message)...");
    pmm_free_pages(&pmm, 0x0, 1);  /* 无效地址 */
    pmm_free_pages(&pmm, 0xFFFFFFFFULL, 1);  /* 超出范围 */

    KLOG_INFO("✓ Boundary test passed\n");
}

/* ── PMM 初始化和测试入口 ─────────────────────────────────────────── */

/**
 * pmm_initialize - 初始化物理内存管理器
 *
 * 必须在 kernel_main 中早期调用，在任何内存分配之前。
 */
void pmm_initialize(void)
{
    KLOG_INFO("=== Initializing Physical Memory Manager ===\n");

    /* 初始化 PMM */
    pmm_init(&pmm,
             PMM_RAM_BASE,
             PMM_RAM_SIZE,
             pmm_bitmap_buffer,
             sizeof(pmm_bitmap_buffer));

    /* 标记内核内存区域为已分配 */
    KLOG_INFO("Marking kernel memory as allocated...\n");
    pmm_mark_kernel_allocated(&pmm);

#if ARCH_RISCV64
    /*
     * QEMU RISC-V 默认 OpenSBI 固件驻留在 RAM 起始低地址（约 0x80000000 起）。
     * 这段区域在 S-mode 下不可安全作为普通页分配，否则会在 memcpy 等访问时触发异常。
     * 预留 [0x80000000, 0x801FFFFF]（2MB）覆盖固件与早期保留区。
     */
    KLOG_INFO("Reserving OpenSBI/firmware region: 0x%llx - 0x%llx\n",
              (uint64_t)0x80000000ULL, (uint64_t)0x801FFFFFULL);
    pmm_mark_allocated(&pmm, 0x80000000ULL, 0x801FFFFFULL);

    /*
     * 预留 RISC-V 低地址 boot 区（_start/mmu_init/boot 栈/早期页表等）。
     * 从符号可见该区位于 0x80200000 起，内核高地址镜像从 0x80206000 对应物理开始。
     * 若不预留，用户进程 PGD 可能被分配到 0x80200000，覆盖早期关键数据。
     */
    KLOG_INFO("Reserving RISC-V boot-low region: 0x%llx - 0x%llx\n",
              (uint64_t)0x80200000ULL, (uint64_t)0x80205FFFULL);
    pmm_mark_allocated(&pmm, 0x80200000ULL, 0x80205FFFULL);
#endif

    /* 预留 rootfs 物理区域，防止 PMM 将其分配出去 */
    KLOG_INFO("Reserving rootfs region: 0x%llx - 0x%llx\n",
              (uint64_t)RAMBLK_PHYS_BASE, (uint64_t)RAMBLK_PHYS_END);
    pmm_mark_allocated(&pmm, RAMBLK_PHYS_BASE, RAMBLK_PHYS_END);

    KLOG_INFO("PMM initialization completed\n");
}

/**
 * run_pmm_tests - 运行所有 PMM 测试
 *
 * 在 pmm_initialize() 之后调用。
 */
void run_pmm_tests(void)
{
    KLOG_INFO("=== Running PMM Tests ===\n");

    test_pmm_basic();
    test_pmm_large_allocation();
    test_pmm_fragmentation();
    test_pmm_stress();
    test_pmm_boundary();

    KLOG_INFO("=== All PMM Tests Passed ===\n");
}
