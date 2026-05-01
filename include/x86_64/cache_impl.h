#ifndef X86_64_CACHE_IMPL_H
#define X86_64_CACHE_IMPL_H

#include "barrier.h"  /* 使用 barrier.h 的内存屏障 */

/*
 * x86_64 缓存操作底层实现
 * 架构特定的单个缓存行操作
 */

/* 全局缓存行大小 */
static size_t g_cache_line_size = 64;  /* x86_64 标准缓存行 */

/* ===== 底层缓存操作指令 ===== */

/**
 * __clean_dcache_one - 清理单个缓存行
 * @addr: 地址
 *
 * CLWB: 写回缓存行但保留在缓存中（如果可用）
 * 否则使用 CLFLUSH
 */
#ifdef __CLWB__
static inline void
__clean_dcache_one(const void *addr)
{
    asm volatile("clwb %0" : : "m"(*(const char *)addr) : "memory");
}
#else
static inline void
__clean_dcache_one(const void *addr)
{
    /* CLFLUSH: 写回并失效 */
    asm volatile("clflush %0" : : "m"(*(const char *)addr) : "memory");
}
#endif

/**
 * __invalidate_dcache_one - 使单个缓存行失效
 * @addr: 地址
 *
 * CLFLUSHOPT: 优化的缓存行失效（如果可用）
 * 否则使用 CLFLUSH
 */
#ifdef __CLFLUSHOPT__
static inline void
__invalidate_dcache_one(const void *addr)
{
    asm volatile("clflushopt %0" : : "m"(*(const char *)addr) : "memory");
}
#else
static inline void
__invalidate_dcache_one(const void *addr)
{
    asm volatile("clflush %0" : : "m"(*(const char *)addr) : "memory");
}
#endif

/**
 * __clean_and_invalidate_dcache_one - 清理并使单个缓存行失效
 * @addr: 地址
 *
 * CLFLUSH: 写回并使缓存行失效
 */
static inline void
__clean_and_invalidate_dcache_one(const void *addr)
{
    asm volatile("clflush %0" : : "m"(*(const char *)addr) : "memory");
}

/* ===== 缓存行大小管理 ===== */

/**
 * get_cache_line_size - 获取缓存行大小
 */
static inline size_t
get_cache_line_size(void)
{
    return g_cache_line_size;
}

/**
 * init_cache - 初始化缓存子系统
 */
static inline void
init_cache(void)
{
    /* x86_64 标准缓存行大小为 64 字节 */
    /* 可以通过 CPUID 指令检测，但 64 字节是通用标准 */
    g_cache_line_size = 64;
}

/**
 * sync_caches - 同步缓存操作
 *
 * 使用 barrier.h 的数据屏障
 */
static inline void
sync_caches(void)
{
    barrier_data();
}

#endif /* X86_64_CACHE_IMPL_H */
