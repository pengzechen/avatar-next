#ifndef AARCH64_STRING_IMPL_H
#define AARCH64_STRING_IMPL_H

#include "types.h"
#include "aarch64/cpu.h"

/*
 * AArch64 架构的字符串函数优化实现
 * 使用 NEON 指令集优化
 */

/**
 * memcpy_neon - ARM NEON 优化版本
 * @dest: 目标地址
 * @src: 源地址
 * @n: 拷贝字节数
 *
 * 使用 NEON 128-bit 寄存器进行批量拷贝。
 *
 * 只把**目的地址**对齐到 16 字节就够：ld1/st1 的 {v0.16b} 变体是按字节
 * 访问的，架构上不要求 16 字节对齐（这点与 ldr q/ldp q 不同）。原来的
 * 版本要求 src/dest 两边同时 16 字节对齐，一旦两者奇偶不同（比如网络
 * 缓冲区拷进对齐的堆块），对齐循环永不退出，整个拷贝退化成逐字节。
 *
 * 注意：使用前必须调用 aarch64_enable_neon() 启用 NEON（boot/aarch64/boot.S
 * 在装向量表之前就打开了 CPACR.FPEN，正常路径无需再关心）。
 */
static inline void *
memcpy_neon(void *dest, const void *src, size_t n)
{
    size_t         i = 0;
    uint8_t       *d = (uint8_t *) dest;
    const uint8_t *s = (const uint8_t *) src;

    /* 1. 先把目的地址推到 16 字节边界（最多 15 字节） */
    while (i < n && ((uint64_t) (d + i) % 16 != 0)) {
        d[i] = s[i];
        i++;
    }

    /* 2. NEON 128-bit 拷贝 */
    for (; i + 15 < n; i += 16) {
        asm volatile(
            "ld1 {v0.16b}, [%[src]]\n"    /* 加载 16 字节到 NEON v0 */
            "st1 {v0.16b}, [%[dest]]\n"   /* 存储 16 字节到 dest */
            :
            : [src] "r"(s + i), [dest] "r"(d + i)
            : "v0", "memory");
    }

    /* 3. 剩余不足 16 字节拷贝 */
    for (; i < n; i++) {
        d[i] = s[i];
    }

    return dest;
}

/*
 * 分派阈值：大于 128 字节走 NEON，否则走通用实现。
 * （docs/STRING.md 一直写着这个策略，但代码里直到打开 FP/SIMD 才接上。）
 *
 * NEON 版本会写 v0 —— caller-saved，且内核现在有陷阱帧（exception.S 无条件
 * 保存 q0-q31）与任务切换帧（switch.S 保存 d8-d15）两层保护，用户态与内核态
 * 的 FP 状态都不会因此丢失。详见 docs/arch/aarch64/FP_SIMD_CONTEXT.md。
 */
#define MEMCPY_NEON_THRESHOLD 128u

static inline void *
memcpy(void *dest, const void *src, size_t n)
{
    if (n > MEMCPY_NEON_THRESHOLD)
        return memcpy_neon(dest, src, n);
    return memcpy_generic(dest, src, n);
}

#endif /* AARCH64_STRING_IMPL_H */
