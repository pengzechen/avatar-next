#ifndef AARCH64_STRING_IMPL_H
#define AARCH64_STRING_IMPL_H

#include "types.h"
#include "aarch64/cpu.h"

/*
 * AArch64 架构的字符串函数优化实现
 * 使用 NEON 指令集优化
 */

/**
 * memcpy - ARM NEON 优化版本
 * @dest: 目标地址
 * @src: 源地址
 * @n: 拷贝字节数
 *
 * 使用 NEON 128-bit 寄存器进行批量拷贝
 *
 * 注意：使用前必须调用 aarch64_enable_neon() 启用 NEON
 */
static inline void *
memcpy_neon(void *dest, const void *src, size_t n)
{
    size_t         i = 0;
    uint8_t       *d = (uint8_t *) dest;
    const uint8_t *s = (const uint8_t *) src;

    /* 1. 对齐拷贝至 16 字节边界 */
    while (i < n && ((uint64_t) (d + i) % 16 != 0 || (uint64_t) (s + i) % 16 != 0)) {
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

/* AArch64 使用通用实现（不使用 NEON，因为 -mgeneral-regs-only） */
static inline void *
memcpy(void *dest, const void *src, size_t n)
{
    return memcpy_generic(dest, src, n);
}

#endif /* AARCH64_STRING_IMPL_H */
