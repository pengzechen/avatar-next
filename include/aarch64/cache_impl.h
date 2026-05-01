#ifndef AARCH64_CACHE_IMPL_H
#define AARCH64_CACHE_IMPL_H

#include "barrier.h"  /* 使用 barrier.h 的内存屏障 */

/*
 * AArch64 缓存操作底层实现
 * 架构特定的单个缓存行操作
 */

/* AArch64 系统寄存器定义 */
#define CTR_EL0_DMINLINE_SHIFT  16
#define CTR_EL0_DMINLINE_MASK   0xF
#define CTR_EL0_ILINE_SHIFT     0
#define CTR_EL0_ILINE_MASK      0xF

/* 缓存行大小相关常量 */
#define CACHE_LINE_WORD_SIZE    4      /* WORD = 4 bytes */
#define MIN_CACHELINE_SIZE      16     /* 最小缓存行 */
#define MAX_CACHELINE_SIZE      2048   /* 最大缓存行 */

/* 全局缓存行大小 */
static size_t g_cache_line_size = 64;  /* 默认 64 字节 */

/* ===== 底层缓存操作指令 ===== */

/**
 * __clean_dcache_one - 清理单个缓存行
 * @addr: 缓存行对齐的地址
 */
static inline void
__clean_dcache_one(const void *addr)
{
    asm volatile("dc cvac, %0" : : "r"(addr) : "memory");
}

/**
 * __invalidate_dcache_one - 使单个缓存行失效
 * @addr: 缓存行对齐的地址
 */
static inline void
__invalidate_dcache_one(const void *addr)
{
    asm volatile("dc ivac, %0" : : "r"(addr) : "memory");
}

/**
 * __clean_and_invalidate_dcache_one - 清理并使单个缓存行失效
 * @addr: 缓存行对齐的地址
 */
static inline void
__clean_and_invalidate_dcache_one(const void *addr)
{
    asm volatile("dc civac, %0" : : "r"(addr) : "memory");
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
    uint64_t ctr_el0;
    uint32_t dminline;
    size_t   cache_size;

    /* 读取 CTR_EL0 寄存器 */
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr_el0));

    /* 提取 DminLine 字段 (bits [19:16]) */
    dminline = (ctr_el0 >> CTR_EL0_DMINLINE_SHIFT) & CTR_EL0_DMINLINE_MASK;

    /* 计算缓存行大小: WORD_SIZE * 2^DminLine */
    cache_size = CACHE_LINE_WORD_SIZE << dminline;

    /* 验证缓存行大小的合理性 */
    if (cache_size < MIN_CACHELINE_SIZE || cache_size > MAX_CACHELINE_SIZE) {
        /* 无效的缓存行大小，使用默认值 */
        cache_size = 64;
    }

    g_cache_line_size = cache_size;
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

#endif /* AARCH64_CACHE_IMPL_H */
