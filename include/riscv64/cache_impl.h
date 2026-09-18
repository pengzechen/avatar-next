#ifndef RISCV64_CACHE_IMPL_H
#define RISCV64_CACHE_IMPL_H

#include "types.h"
#include "barrier.h"
#include "riscv64/rvplatform.h"   /* 本平台实际可用的 CMO 原语 */

/*
 * RISC-V 64 的 cache 操作实现
 *
 * 本文件只做**动词 → 原语**的映射，具体指令由 rvplatform.h 按平台给出
 * （Zicbom / T-Head 私有 / 无 CMO 三种情况）。这样平台差异不会渗进通用动词：
 * 以前这里用 `#if PLATFORM_SG2002` 把三个 range 动词全部换成了"整 cache 刷"，
 * 于是 clean_dcache_range() 在 SG2002 上既丢了区间语义、也没法按需清理。
 */

/* 缓存行大小（= CMO 粒度）。目前是常量：Zicbom 规范上应从设备树
 * `riscv,cbom-block-size` 读，本内核还没有设备树解析。 */
static size_t g_cache_line_size = RV_CMO_BLOCK_SIZE;

/* ===== 底层单行操作 ===== */

/**
 * __clean_dcache_one - 清理单个缓存行（写回内存）
 */
static inline void
__clean_dcache_one(const void *addr)
{
    rv_cmo_clean((void *) addr);
}

/**
 * __invalidate_dcache_one - 使单个缓存行失效（不写回，脏数据会丢）
 */
static inline void
__invalidate_dcache_one(const void *addr)
{
    rv_cmo_invalidate((void *) addr);
}

/**
 * __clean_and_invalidate_dcache_one - 写回并使单个缓存行失效
 *
 * T-Head 上用一条 `dcache.civa`：原来的写法是 cva 之后再 iva，
 * 两条指令之间别的 agent 可能把该行弄脏，语义上不等价。
 */
static inline void
__clean_and_invalidate_dcache_one(const void *addr)
{
    rv_cmo_clean_invalidate((void *) addr);
}

/* ===== 缓存行大小管理 ===== */

static inline size_t
get_cache_line_size(void)
{
    return g_cache_line_size;
}

static inline void
init_cache(void)
{
    g_cache_line_size = rv_cmo_block_size();
}

/**
 * sync_caches - 等待之前的 cache 操作完成
 *
 * 设备/CMO 之后的完成屏障；没有 CMO 的平台上这就是唯一的动作。
 */
static inline void
sync_caches(void)
{
    rv_cmo_sync();
}

/**
 * sync_icache_all - 让刚写入的指令对所有核可见
 *
 * RISC-V 上 `fence.i` 是标准做法；但 C906 这类核上 fence.i 不足以保证
 * I-cache 可见（厂商私有 icache.iall + sync.i 才是），由 rvplatform 层处理。
 */
static inline void
sync_icache_all(void)
{
    rv_cmo_icache_sync();
}

#endif /* RISCV64_CACHE_IMPL_H */
