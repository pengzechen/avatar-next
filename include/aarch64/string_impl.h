#ifndef AARCH64_STRING_IMPL_H
#define AARCH64_STRING_IMPL_H

#include "types.h"
#include "aarch64/cpu.h"

/*
 * AArch64 架构的字符串函数优化实现（NEON）
 *
 * 本文件只提供**具名实现**（memcpy_neon / memset_neon）与架构分派
 * （memcpy_arch / memset_arch）。公开的 memcpy/memset 名字在 string.h，
 * 外部符号在 lib/string.c —— 两者都转发到这里，见 string_internal.h 顶部。
 *
 * 前置条件：CPACR.FPEN 已打开。boot/aarch64/boot.S 的 init_el2_vhe 在
 * 装向量表之前就打开了，早于任何 C 代码，所以正常路径无需关心。
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

/**
 * memset_neon - ARM NEON 优化版本
 * @s: 目标地址
 * @c: 填充字节
 * @n: 填充字节数
 *
 * dup 把字节广播到 16 个 lane，再用 st1 一次写 16 字节。
 * 与 memcpy_neon 同理，st1 {v0.16b} 按字节访问，只需目的地址对齐。
 */
static inline void *
memset_neon(void *s, int c, size_t n)
{
    size_t   i = 0;
    uint8_t *d = (uint8_t *) s;
    unsigned byte = (unsigned) (uint8_t) c;

    /* 1. 先把目的地址推到 16 字节边界 */
    while (i < n && ((uint64_t) (d + i) % 16 != 0))
        d[i++] = (uint8_t) byte;

    /* 2. NEON 128-bit 填充 */
    for (; i + 15 < n; i += 16) {
        asm volatile(
            "dup v0.16b, %w[b]\n"        /* 字节广播到 16 个 lane */
            "st1 {v0.16b}, [%[dst]]\n"
            :
            : [b] "r"(byte), [dst] "r"(d + i)
            : "v0", "memory");
    }

    /* 3. 剩余不足 16 字节 */
    for (; i < n; i++)
        d[i] = (uint8_t) byte;

    return s;
}

/*
 * 分派阈值：大于 128 字节走 NEON，否则走通用实现。
 * （小块用 NEON 要付对齐 + 分支的开销，不划算。）
 *
 * NEON 版本会写 v0 —— caller-saved，且内核有陷阱帧（exception.S 无条件保存
 * q0-q31）与任务切换帧（switch.S 保存 d8-d15）两层保护，用户态与内核态的 FP
 * 状态都不会因此丢失。详见 docs/arch/aarch64/FP_SIMD_CONTEXT.md。
 */
#define MEMCPY_NEON_THRESHOLD 128u
#define MEMSET_NEON_THRESHOLD 128u

static inline void *
memcpy_arch(void *dest, const void *src, size_t n)
{
    if (n > MEMCPY_NEON_THRESHOLD)
        return memcpy_neon(dest, src, n);
    return memcpy_generic(dest, src, n);
}

static inline void *
memset_arch(void *s, int c, size_t n)
{
    if (n > MEMSET_NEON_THRESHOLD)
        return memset_neon(s, c, n);
    return memset_generic(s, c, n);
}

#endif /* AARCH64_STRING_IMPL_H */
