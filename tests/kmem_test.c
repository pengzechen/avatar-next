/*
 * tests/kmem_test.c — AArch64 内核内存管理测试
 *
 * 从 kernel/mm/aarch64/vmm.c 中分离出的测试函数。
 * 仅在 AArch64 架构下编译。
 */

#include "arch.h"

#if ARCH_AARCH64

#include "types.h"
#include "klog.h"
#include "assert.h"
#include "pmm.h"
#include "mm_vm.h"
#include "aarch64/mmu.h"
#include "string.h"

extern pte_t *create_uvm(void);
extern pte_t *find_pte(pte_t *page_dir, uint64_t vaddr, int32_t alloc);
extern int32_t memory_create_map(void *page_dir, uint64_t vaddr, uint64_t paddr,
                                 int32_t count, uint64_t perm);
extern uint64_t memory_get_paddr(void *page_dir, uint64_t vaddr);
extern void memory_free_page(void *page_dir, uint64_t addr);
extern void destory_4level(pte_t *page_dir);
extern void destroy_uvm_4level(pte_t *page_dir);
extern int32_t memory_copy_uvm_4level(void *dst_pgd, void *src_pgd);
extern void copydata_to_uvm(void *page_dir, uint64_t vaddr, uint64_t paddr, uint64_t size);
extern uint64_t memory_alloc_page(void *page_dir, uint64_t vaddr, uint64_t size, int32_t perm);

extern char __heap_flag[];

static int32_t
get_available_page_count(void)
{
    uint64_t free_count = pmm_get_free_pages(g_pmm);

    KLOG_INFO("get_available_page_count: g_pmm = 0x%llx\n", (uint64_t)g_pmm);
    KLOG_INFO("  g_pmm->free_pages = %llu\n", g_pmm->free_pages);
    KLOG_INFO("  g_pmm->total_pages = %llu\n", g_pmm->total_pages);
    KLOG_INFO("  returning: %llu\n", free_count);

    return (int32_t)free_count;
}

void assert_bitmap_state(uint64_t addr, uint8_t expected_state)
{
    size_t page_index = (addr - g_pmm->start_addr) / g_pmm->page_size;
    uint8_t current_state = bitmap_test(&g_pmm->bitmap, page_index);
    assert(current_state == expected_state);
}

void test_alloc_free()
{
    uint64_t addr;

    KLOG_INFO("test_alloc_free: Starting...\n");

    KLOG_INFO("test_alloc_free: Allocating 1 page...\n");
    addr = pmm_alloc_pages(g_pmm, 1);
    KLOG_INFO("test_alloc_free: Allocated 1 page at address: 0x%llx\n", addr);
    assert(addr != 0);

    assert_bitmap_state(addr, 1);

    pmm_free_pages(g_pmm, addr, 1);
    KLOG_INFO("Freed 1 page at address: 0x%llx\n", addr);

    assert_bitmap_state(addr, 0);

    addr = pmm_alloc_pages(g_pmm, 4);
    KLOG_INFO("Allocated 4 pages starting at address: 0x%llx\n", addr);
    assert(addr != 0);

    for (int32_t i = 0; i < 4; i++) {
        assert_bitmap_state(addr + i * PAGE_SIZE, 1);
    }

    pmm_free_pages(g_pmm, addr, 4);
    KLOG_INFO("Freed 4 pages starting at address: 0x%llx\n", addr);

    for (int32_t i = 0; i < 4; i++) {
        assert_bitmap_state(addr + i * PAGE_SIZE, 0);
    }
}

void test_find_free_page()
{
    uint64_t addr;

    addr = pmm_alloc_pages(g_pmm, 1);
    KLOG_INFO("Allocated 1 page at address: 0x%llx\n", addr);
    assert(addr != 0);

    uint64_t free_page = bitmap_find_first_free(&g_pmm->bitmap);
    KLOG_INFO("First free page is at index: %lu\n", free_page);
    assert(free_page != (uint64_t)-1);

    assert_bitmap_state(free_page * g_pmm->page_size + g_pmm->start_addr, 0);

    pmm_free_pages(g_pmm, addr, 1);
}

void test_find_contiguous_free_pages()
{
    uint64_t addr;

    addr = pmm_alloc_pages(g_pmm, 4);
    KLOG_INFO("Allocated 4 pages at address: 0x%llx\n", addr);
    assert(addr != 0);

    uint64_t free_page = bitmap_find_contiguous_free(&g_pmm->bitmap, 4);
    KLOG_INFO("Found contiguous 4 free pages at index: %lu\n", free_page);
    assert(free_page != (size_t)-1);

    for (int32_t i = 0; i < 4; i++) {
        assert_bitmap_state((free_page + i) * g_pmm->page_size + g_pmm->start_addr, 0);
    }

    pmm_free_pages(g_pmm, addr, 4);
}

void test_create_uvm_find_pte()
{
    uint64_t total_nums = get_available_page_count();
    KLOG_INFO("test start total nums: %d\n", total_nums);

    pte_t *page_dir = create_uvm();
    assert(page_dir != (pte_t *)0);
    KLOG_INFO("Page directory created at: %llx\n", page_dir);

    uint64_t vaddr = 0x54567890;
    pte_t *pte = find_pte(page_dir, vaddr, 1);
    assert(pte != NULL);
    KLOG_INFO("Page table entry for vaddr 0x%llx: 0x%llx\n", vaddr, pte);

    pte_t *test = find_pte(page_dir, vaddr, 1);
    assert(pte == test);

    destory_4level(page_dir);

    uint64_t end_total_nums = get_available_page_count();
    KLOG_INFO("test end total nums: %d\n", end_total_nums);
    assert(total_nums == end_total_nums);
}

void test_memory_create_map()
{
    uint64_t total_nums = get_available_page_count();
    KLOG_INFO("test start total nums: %d\n", total_nums);

    pte_t *page_dir = create_uvm();
    assert(page_dir != (pte_t *)0);
    KLOG_INFO("Page directory created at: %llx\n", (unsigned long)page_dir);

    uint64_t vaddr = 0x1000;
    uint64_t paddr = pmm_alloc_pages(g_pmm, 3);
    int32_t count = 3;

    int32_t result = memory_create_map(page_dir, vaddr, paddr, count, 0x0);
    assert(result == 0);

    uint64_t mapped_paddr = memory_get_paddr(page_dir, vaddr);
    assert(mapped_paddr == paddr);

    uint64_t vaddr2 = vaddr + PAGE_SIZE;
    uint64_t mapped_paddr2 = memory_get_paddr(page_dir, vaddr2);
    assert(mapped_paddr2 == paddr + PAGE_SIZE);

    uint64_t vaddr3 = vaddr + PAGE_SIZE * 2;
    uint64_t mapped_paddr3 = memory_get_paddr(page_dir, vaddr3);
    assert(mapped_paddr3 == paddr + PAGE_SIZE * 2);

    KLOG_INFO("test free page: \n");
    memory_free_page(page_dir, vaddr);
    memory_free_page(page_dir, vaddr2);
    memory_free_page(page_dir, vaddr3);

    destory_4level(page_dir);

    uint64_t end_total_nums = get_available_page_count();
    KLOG_INFO("test end total nums: %d\n", end_total_nums);
    assert(total_nums == end_total_nums);
}

void test_uvm_alloc_free()
{
    uint64_t total_nums = get_available_page_count();
    KLOG_INFO("test start total nums: %d\n", total_nums);

    pte_t *page_dir = create_uvm();
    assert(page_dir != (pte_t *)0);
    KLOG_INFO("Page directory created at: %llx\n", (unsigned long)page_dir);

    memory_alloc_page(page_dir, 0x1000, 34, 0);
    memory_free_page(page_dir, 0x1000);

    destory_4level(page_dir);

    uint64_t end_total_nums = get_available_page_count();
    KLOG_INFO("test end total nums: %d\n", end_total_nums);
    assert(total_nums == end_total_nums);
}

void test_copydata_to_uvm()
{
    uint64_t total_nums = get_available_page_count();
    KLOG_INFO("test start total nums: %d\n", total_nums);

    pte_t *page_dir = create_uvm();
    assert(page_dir != (pte_t *)0);
    KLOG_INFO("Page directory created at: %llx\n", (unsigned long)page_dir);

    char data[156] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};
    uint64_t paddr = pmm_alloc_pages(g_pmm, 1);
    memcpy((void *)paddr, data, 156);

    memory_alloc_page(page_dir, 0x1000, 156, 0);
    copydata_to_uvm(page_dir, 0x1000, paddr, 156);

    uint64_t paddr_to_check = memory_get_paddr(page_dir, 0x1000);
    if (memcmp((const void *)paddr, (const void *)paddr_to_check, 156) == 0) {
        KLOG_INFO("data ok\n");
    }

    memory_free_page(page_dir, 0x1000);
    pmm_free_pages(g_pmm, paddr, 1);

    destory_4level(page_dir);

    uint64_t end_total_nums = get_available_page_count();
    KLOG_INFO("test end total nums: %d\n", end_total_nums);
    assert(total_nums == end_total_nums);
}

void test_memory_copy_uvm_4level()
{
    uint64_t total_nums = get_available_page_count();
    KLOG_INFO("test start total nums: %d\n", total_nums);

    pte_t *src_pgd = create_uvm();
    pte_t *dst_pgd = phys_to_virt(pmm_alloc_pages(g_pmm, 1));
    memset(dst_pgd, 0, PAGE_SIZE);

    const char *data = "1234567890abcdefghijklmnooqrstuvwxyz"
                       "1234567890abcdefghijklmnooqrstuvwxyz"
                       "1234567890abcdefghijklmnooqrstuvwxyz"
                       "1234567890abcdefghijklmnooqrstuvwxyz"
                       "1234567890abcdefghijklmnooqrstuvwxyz"
                       "1234567890abcdefghijklmnooqrstuvwxyz"
                       "1234567890abcdefghijklmnooqrstuvwxyz"
                       "1234567890abcdefghijklmnooqrstuvwxyz"
                       "1234567890abcdefghijklmnooqrstuvwxyz"
                       "1234567890abcdefghijklmnooqrstuvwxyz";
    size_t data_len = strlen(data);

    uint64_t src_phys = pmm_alloc_pages(g_pmm, 1);
    memcpy(phys_to_virt(src_phys), data, data_len);

    memory_alloc_page(src_pgd, 0x1000, PAGE_SIZE, 0);
    copydata_to_uvm(src_pgd, 0x1000, src_phys, data_len);

    int32_t result = memory_copy_uvm_4level(dst_pgd, src_pgd);
    assert(result == 0);

    uint64_t dst_phys = memory_get_paddr(dst_pgd, 0x1000);
    KLOG_INFO("src: %llx, dest: %llx\n", src_phys, dst_phys);
    assert(memcmp(phys_to_virt(src_phys), phys_to_virt(dst_phys), data_len) == 0);

    memory_free_page(src_pgd, 0x1000);
    memory_free_page(dst_pgd, 0x1000);

    destroy_uvm_4level(src_pgd);
    destroy_uvm_4level(dst_pgd);

    pmm_free_pages(g_pmm, src_phys, 1);

    uint64_t end_total_nums = get_available_page_count();
    KLOG_INFO("test end total nums: %d\n", end_total_nums);
    assert(total_nums == end_total_nums);
}

void kmem_test()
{
    KLOG_INFO("\n=========copy uvm to uvm tests: =========\n");
    test_memory_copy_uvm_4level();
}

#else /* !ARCH_AARCH64 */

void kmem_test(void)
{
}

#endif /* ARCH_AARCH64 */
