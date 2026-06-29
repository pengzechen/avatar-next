/*
 * lib/rust_glue.c — Rust FFI 胶水层
 *
 * 为 Rust no_std 代码提供内核 C 函数的非内联包装。
 *
 * 导出接口：
 *   kernel_alloc(size)     — 分配内存，返回内核虚拟地址
 *   kernel_free(ptr, size) — 释放 kernel_alloc 分配的内存
 */

#include "kmalloc.h"
#include "types.h"

void *kernel_alloc(size_t size)
{
    return kmalloc(size);
}

void kernel_free(void *ptr, size_t size)
{
    kfree(ptr, size);
}
