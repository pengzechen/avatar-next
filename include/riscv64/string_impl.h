#ifndef RISCV64_STRING_IMPL_H
#define RISCV64_STRING_IMPL_H

#include "types.h"

/*
 * RISC-V 64 位架构的字符串函数优化实现
 *
 * 目前转发到通用实现。要做 V 扩展优化时，在这里加 memcpy_vector()/memset_vector()
 * 并让下面两个函数按阈值分派即可（对照 include/aarch64/string_impl.h）。
 *
 * 注意：通用实现用普通 GPR 做 8/4/2 字节访问，只有在 src/dest **同时**对齐时
 * 才加宽 —— SG2002/C906 这类真实硬件不保证支持非对齐访问，不能想当然放宽。
 */
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

#endif /* RISCV64_STRING_IMPL_H */
