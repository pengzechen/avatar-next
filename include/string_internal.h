#ifndef __STRING_INTERNAL_H
#define __STRING_INTERNAL_H

/*
 * include/string_internal.h — 字符串/内存函数的**实现层**
 *
 * 为什么实现要和 string.h 分家：
 *
 *   公开 API（string.h 里的 `static inline memcpy`/`memset`）占用了这两个**名字**，
 *   而且是内部链接。lib/string.c 必须把同样的名字定义成**外部符号**，供编译器
 *   自动生成的调用使用（大结构体整体赋值、循环→memcpy 变换等，GCC 会自己发
 *   `bl memcpy`）。同一个翻译单元里同名标识符既有内部链接又有外部链接是未定义
 *   行为（C11 6.2.2p7），所以只能把"实现"和"公开名字"拆开：
 *
 *     string_internal.h  实现：memcpy_generic / memcpy_arch / memcpy_neon …
 *     string.h           公开 API：static inline 转发到 *_arch
 *     lib/string.c       外部符号：同样转发到 *_arch
 *
 *   这样"源码里写的 memcpy"和"编译器自己生成的 memcpy 调用"落到**同一份实现**。
 *   在此之前两者是分裂的：前者拿到内联的优化版本，后者落到 lib/string.c 里那个
 *   逐字节循环（实测 fork/clone 路径的 816 字节 trap frame 拷贝就是后者）。
 *
 * 架构约定 —— 每个架构在自己的 string_impl.h 里提供两个函数：
 *   memcpy_arch(dst, src, n)    本架构的 memcpy（可以是阈值分派，也可以是通用实现）
 *   memset_arch(dst, c, n)      同上
 * 名字刻意不叫 memcpy/memset：那样又会和公开 API 撞名，回到老问题。
 */

#include "types.h"
#include "arch.h"

/* ── 通用实现（所有架构都能用，也是各架构的兜底）──────────────────── */

/**
 * memcpy_generic - 通用 memcpy
 *
 * 为什么宽访问要求 src/dest **同时**对齐：这里用的是普通 GPR 的 8/4/2 字节
 * 访问，而不是 NEON 那种按字节访问的 ld1/st1。RISC-V（尤其 SG2002/C906 这类
 * 真实硬件）不保证支持非对齐访问，所以只能两边都对齐才敢加宽。
 *
 * 代价：src/dest 的**奇偶不同**时（相对偏移是奇数），无论推进多少字节两者都
 * 不可能同时对齐 —— 这种情况会退化成逐字节拷贝。这是该约束下的必然结果，不是
 * 缺陷；要再快就得做"两次对齐读取 + 移位拼合"，或者确认目标平台允许非对齐访问。
 * （AArch64 的 memcpy_neon 没有这个问题：ld1/st1 {v0.16b} 按字节访问，只需要
 * 对齐目的地址。）
 */
static inline void *
memcpy_generic(void *dest, const void *src, size_t n)
{
    size_t         i = 0;
    uint8_t       *d = (uint8_t *) dest;
    const uint8_t *s = (const uint8_t *) src;

    if (n < sizeof(uint64_t)) {
        while (i < n) {
            d[i] = s[i];
            i++;
        }
        return dest;
    }

    /* 逐字节拷贝直到两边都 2 字节对齐 */
    while (i < n && ((uintptr_t) (d + i) % 2 != 0 || (uintptr_t) (s + i) % 2 != 0)) {
        d[i] = s[i];
        i++;
    }

    /* 8 字节拷贝 */
    while ((n - i) >= sizeof(uint64_t) &&
           ((uintptr_t) (d + i) % sizeof(uint64_t) == 0) &&
           ((uintptr_t) (s + i) % sizeof(uint64_t) == 0)) {
        uint64_t word;
        __builtin_memcpy(&word, s + i, sizeof(word));
        __builtin_memcpy(d + i, &word, sizeof(word));
        i += sizeof(uint64_t);
    }

    /* 4 字节拷贝 */
    while ((n - i) >= sizeof(uint32_t) &&
           ((uintptr_t) (d + i) % sizeof(uint32_t) == 0) &&
           ((uintptr_t) (s + i) % sizeof(uint32_t) == 0)) {
        uint32_t word;
        __builtin_memcpy(&word, s + i, sizeof(word));
        __builtin_memcpy(d + i, &word, sizeof(word));
        i += sizeof(uint32_t);
    }

    /* 2 字节拷贝 */
    while ((n - i) >= sizeof(uint16_t) &&
           ((uintptr_t) (d + i) % sizeof(uint16_t) == 0) &&
           ((uintptr_t) (s + i) % sizeof(uint16_t) == 0)) {
        uint16_t word;
        __builtin_memcpy(&word, s + i, sizeof(word));
        __builtin_memcpy(d + i, &word, sizeof(word));
        i += sizeof(uint16_t);
    }

    /* 剩余逐字节拷贝 */
    while (i < n) {
        d[i] = s[i];
        i++;
    }

    return dest;
}

/**
 * memset_generic - 通用 memset（按字宽填充，同样要求目的地址对齐后才加宽）
 */
static inline void *
memset_generic(void *s, int c, size_t n)
{
    size_t   i = 0;
    uint8_t *d = (uint8_t *) s;
    uint8_t  b = (uint8_t) c;

    if (n < sizeof(uint64_t)) {
        while (i < n)
            d[i++] = b;
        return s;
    }

    /* 先把目的地址推到 2 字节边界 */
    while (i < n && ((uintptr_t) (d + i) % 2 != 0))
        d[i++] = b;

    /* 8 字节填充（源是常量，不存在"两边对齐"问题，只需目的对齐） */
    uint64_t word64 = 0x0101010101010101ULL * b;
    while ((n - i) >= sizeof(uint64_t) &&
           ((uintptr_t) (d + i) % sizeof(uint64_t) == 0)) {
        __builtin_memcpy(d + i, &word64, sizeof(word64));
        i += sizeof(uint64_t);
    }

    uint32_t word32 = 0x01010101U * b;
    while ((n - i) >= sizeof(uint32_t) &&
           ((uintptr_t) (d + i) % sizeof(uint32_t) == 0)) {
        __builtin_memcpy(d + i, &word32, sizeof(word32));
        i += sizeof(uint32_t);
    }

    uint16_t word16 = (uint16_t) (0x0101U * b);
    while ((n - i) >= sizeof(uint16_t) &&
           ((uintptr_t) (d + i) % sizeof(uint16_t) == 0)) {
        __builtin_memcpy(d + i, &word16, sizeof(word16));
        i += sizeof(uint16_t);
    }

    while (i < n)
        d[i++] = b;

    return s;
}

/**
 * memmove_generic - 通用 memmove（重叠时反向拷贝）
 */
static inline void *
memmove_generic(void *dest, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *) dest;
    const uint8_t *s = (const uint8_t *) src;

    if (d == s || n == 0)
        return dest;

    /* 目的在源之前（或不相交）：正向拷贝，可复用 memcpy 的加宽路径 */
    if (d < s)
        return memcpy_generic(dest, src, n);

    /* 目的在源之后：从尾部反向拷贝，避免自己覆盖自己 */
    while (n--)
        d[n] = s[n];

    return dest;
}

/* ── 架构实现 ──────────────────────────────────────────────────── */

#if ARCH_X86_64
    #include "x86_64/string_impl.h"
#elif ARCH_AARCH64
    #include "aarch64/string_impl.h"
#elif ARCH_RISCV64
    #include "riscv64/string_impl.h"
#else
    /* 未知架构：直接用通用实现 */
    static inline void *
    memcpy_arch(void *dest, const void *src, size_t n)
    {
        return memcpy_generic(dest, src, n);
    }

    static inline void *
    memset_arch(void *s, int c, size_t n)
    {
        return memset_generic(s, c, n);
    }
#endif

#endif /* __STRING_INTERNAL_H */
