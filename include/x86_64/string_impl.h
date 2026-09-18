#ifndef X86_64_STRING_IMPL_H
#define X86_64_STRING_IMPL_H

#include "types.h"

/*
 * x86_64 架构的字符串函数优化实现
 *
 * 目前转发到通用实现。要注意的是本架构内核编译带 `-mno-mmx -mno-sse`
 * （见 Makefile），所以要做 SSE2/AVX 版本，得先给这几个函数所在的翻译单元
 * 单独开浮点/向量开关，并按 aarch64 的做法把 FP 状态纳入上下文保存。
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

#endif /* X86_64_STRING_IMPL_H */
