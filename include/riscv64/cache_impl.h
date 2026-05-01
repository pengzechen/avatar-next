#ifndef RISCV64_CACHE_IMPL_H
#define RISCV64_CACHE_IMPL_H

#include "barrier.h"  /* 使用 barrier.h 的内存屏障 */

/*
 * RISC-V 64位缓存操作底层实现
 * 架构特定的单个缓存行操作
 */

/* 全局缓存行大小 */
static size_t g_cache_line_size = 64;  /* RISC-V 标准缓存行 */

/* ===== 底层缓存操作指令 ===== */

/**
 * __clean_dcache_one - 清理单个缓存行
 * @addr: 地址
 *
 * CBO.clean: 写回缓存行到内存（需要 Zicbom 扩展）
 */
#ifdef __riscv_zicbom
static inline void
__clean_dcache_one(const void *addr)
{
    asm volatile("cbo.clean %0" : : "r"(addr) : "memory");
}
#else
/* 没有缓存块操作扩展时的替代实现 */
static inline void
__clean_dcache_one(const void *addr)
{
    /* 使用 fence 确保之前的写入完成 */
    asm volatile("fence ow, ow" ::: "memory");
    (void)addr;  /* 避免未使用警告 */
}
#endif

/**
 * __invalidate_dcache_one - 使单个缓存行失效
 * @addr: 地址
 *
 * CBO.inval: 使缓存行失效（需要 Zicbom 扩展）
 */
#ifdef __riscv_zicbom
static inline void
__invalidate_dcache_one(const void *addr)
{
    asm volatile("cbo.inval %0" : : "r"(addr) : "memory");
}
#else
static inline void
__invalidate_dcache_one(const void *addr)
{
    (void)addr;
}
#endif

/**
 * __clean_and_invalidate_dcache_one - 清理并使单个缓存行失效
 * @addr: 地址
 *
 * CBO.flush: 写回并使缓存行失效（需要 Zicbom 扩展）
 */
#ifdef __riscv_zicbom
static inline void
__flush_dcache_one(const void *addr)
{
    asm volatile("cbo.flush %0" : : "r"(addr) : "memory");
}
#else
static inline void
__clean_and_invalidate_dcache_one(const void *addr)
{
    __clean_dcache_one(addr);
    __invalidate_dcache_one(addr);
}
#endif

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
    /* RISC-V 标准缓存行大小为 64 字节 */
    /* 可以通过 misa 寄存器或其他方式检测 */
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

#endif /* RISCV64_CACHE_IMPL_H */
