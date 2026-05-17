/*
 * lib/rust_glue.c — Rust FFI 胶水层
 *
 * 为 Rust no_std 代码提供内核 C 函数的非内联包装。
 * 所有被 Rust FFI 调用的函数必须是普通 extern C 函数（非 static inline/宏）。
 *
 * 导出接口：
 *   kernel_alloc(size)     — 通过 PMM 分配内存，返回内核虚拟地址
 *   kernel_free(ptr, size) — 释放 kernel_alloc 分配的内存
 *
 * klog_putchar / platform_panic 已有 C linkage，Rust 可直接 FFI 调用，无需包装。
 */

#include "pmm.h"
#include "mm_vm.h"
#include "types.h"

/*
 * kernel_alloc - 分配至少 size 字节的内存
 *
 * 返回：内核虚拟地址指针，失败返回 NULL
 *
 * 用 PMM 的页粒度分配（最小 4KB），向上取整到页边界。
 * Rust GlobalAllocator 传来的 Layout.size() 可能 < 4KB，这里统一按页处理。
 */
void *kernel_alloc(size_t size)
{
    if (size == 0 || g_pmm == NULL)
        return NULL;

    uint64_t page_size = g_pmm->page_size;
    uint32_t pages = (uint32_t)((size + page_size - 1) / page_size);
    uint64_t paddr = pmm_alloc_pages(g_pmm, pages);
    if (paddr == 0)
        return NULL;

    /* PMM 返回物理地址，转换为内核可访问的虚拟地址 */
    return phys_to_virt(paddr);
}

/*
 * kernel_free - 释放 kernel_alloc 分配的内存
 *
 * @ptr:  kernel_alloc 返回的虚拟地址
 * @size: 原始请求大小（字节，与 alloc 时相同）
 */
void kernel_free(void *ptr, size_t size)
{
    if (ptr == NULL || g_pmm == NULL || size == 0)
        return;

    uint64_t page_size = g_pmm->page_size;
    uint32_t pages = (uint32_t)((size + page_size - 1) / page_size);
    /* 虚拟地址转回物理地址交给 PMM */
    pmm_free_pages(g_pmm, virt_to_phys(ptr), pages);
}
