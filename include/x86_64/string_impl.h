#ifndef X86_64_STRING_IMPL_H
#define X86_64_STRING_IMPL_H

/*
 * x86_64 架构的字符串函数优化实现
 * 使用 SSE/AVX 指令集优化
 */

/* 使用通用实现 */
static inline void *
memcpy(void *dest, const void *src, size_t n)
{
    return memcpy_generic(dest, src, n);
}

/* TODO: 可以添加 SSE2/AVX2 优化版本
 * static inline void *
 * memcpy_sse2(void *dest, const void *src, size_t n)
 * {
 *     ...
 * }
 */

#endif /* X86_64_STRING_IMPL_H */
