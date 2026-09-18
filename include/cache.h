#ifndef CACHE_H
#define CACHE_H

#include "types.h"
#include "arch.h"

/*
 * 跨架构缓存操作抽象层
 * 支持数据缓存(D-cache)的清理、失效等操作
 */

/* ===== 通用接口定义 ===== */

/**
 * clean_dcache_range - 清理地址范围的数据缓存到内存
 * @addr: 起始地址
 * @size: 字节大小
 *
 * 将指定地址范围的数据缓存行写回到内存，但不从缓存中移除
 */
static inline void clean_dcache_range(const void *addr, size_t size);

/**
 * invalidate_dcache_range - 使地址范围的数据缓存失效
 * @addr: 起始地址
 * @size: 字节大小
 *
 * 使指定地址范围的数据缓存行失效，但不写回内存
 * 警告：未写回的脏数据会丢失！
 */
static inline void invalidate_dcache_range(const void *addr, size_t size);

/**
 * clean_and_invalidate_dcache_range - 清理并使地址范围的数据缓存失效
 * @addr: 起始地址
 * @size: 字节大小
 *
 * 将指定地址范围的数据缓存行写回到内存，然后从缓存中移除
 */
static inline void clean_and_invalidate_dcache_range(const void *addr, size_t size);

/**
 * sync_caches - 同步所有缓存操作
 *
 * 确保所有之前的缓存操作都已完成
 */
static inline void sync_caches(void);

/**
 * sync_icache_all - 让刚刚写入的指令可见（I-cache 同步）
 *
 * 写可执行内存（加载器打补丁、自改码、JIT）之后必须调用，否则 CPU 可能执行
 * 到陈旧的指令。各架构实现：
 *   aarch64  ic iallu + dsb ish + isb
 *   riscv64  fence.i；C906 这类核还要厂商私有的 icache.iall + sync.i
 *            （见 include/riscv64/rvplatform.h）
 *   x86_64   I-cache 与 D-cache 硬件一致，只需一条序列化屏障
 *
 * 注意：汇编里的等价序列（.S 的 exec/fork 返回路径）用的是同一套语义，
 * 但 C 接口在那里不可用，所以 .S 仍自行处理。
 */
static inline void sync_icache_all(void);

/**
 * get_cache_line_size - 获取数据缓存行大小
 *
 * 返回值: 缓存行大小（字节），通常为 32、64 或 128
 */
static inline size_t get_cache_line_size(void);

/**
 * init_cache - 初始化缓存子系统
 *
 * 检测并初始化缓存相关参数（如缓存行大小）
 */
static inline void init_cache(void);

/* ===== 架构特定底层操作 ===== */

#if ARCH_X86_64
    #include "x86_64/cache_impl.h"
#elif ARCH_AARCH64
    #include "aarch64/cache_impl.h"
#elif ARCH_RISCV64
    #include "riscv64/cache_impl.h"
#else
    #error "Unsupported architecture"
#endif

/* ===== 通用 Range 操作实现 ===== */

/*
 * ARCH_HAS_CUSTOM_DCACHE_RANGE：架构/平台自己实现下面三个 range 动词时定义。
 *
 * 现在**没有任何架构用它**（SG2002 曾用它把三个动词都换成"整 cache 刷"，
 * 那等于丢掉区间语义，已删除）。留着这个开关是为了"按 VA 的 CMO 在这块硬件上
 * 不可靠"这种情况 —— 真要用，请先在上板验证，并在实现处写清为什么。
 */
#ifndef ARCH_HAS_CUSTOM_DCACHE_RANGE

/**
 * clean_dcache_range - 清理地址范围的缓存
 */
static inline void
clean_dcache_range(const void *addr, size_t size)
{
    const unsigned char *p      = (const unsigned char *)addr;
    const unsigned char *end    = p + size;
    size_t                    cacheline_size = get_cache_line_size();

    sync_caches();  /* 确保之前的所有内存访问完成 */

    for (; p < end; p += cacheline_size)
        __clean_dcache_one(p);

    sync_caches();  /* 确保 clean 完成 */
}

/**
 * invalidate_dcache_range - 使地址范围的缓存失效
 */
static inline void
invalidate_dcache_range(const void *addr, size_t size)
{
    unsigned char       *p      = (unsigned char *)addr;
    const unsigned char *end    = p + size;
    size_t                off;
    size_t                cacheline_size = get_cache_line_size();

    sync_caches();  /* CPU 发出所有写入到范围 */

    /* 对齐到缓存行边界 */
    off = (unsigned long)p % cacheline_size;
    if (off) {
        p -= off;
        __clean_and_invalidate_dcache_one(p);
        p += cacheline_size;
    }

    off = (unsigned long)end % cacheline_size;
    if (off) {
        end -= off;
        __clean_and_invalidate_dcache_one(end);
    }

    for (; p < end; p += cacheline_size)
        __invalidate_dcache_one(p);

    sync_caches();  /* 确保失效完成 */
}

/**
 * clean_and_invalidate_dcache_range - 清理并使地址范围的缓存失效
 */
static inline void
clean_and_invalidate_dcache_range(const void *addr, size_t size)
{
    const unsigned char *p      = (const unsigned char *)addr;
    const unsigned char *end    = p + size;
    size_t                    cacheline_size = get_cache_line_size();

    sync_caches();

    for (; p < end; p += cacheline_size)
        __clean_and_invalidate_dcache_one(p);

    sync_caches();
}

#endif /* ARCH_HAS_CUSTOM_DCACHE_RANGE */

#endif /* CACHE_H */
