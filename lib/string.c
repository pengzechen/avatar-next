/*
 * lib/string.c — 字符串/内存函数的**外部符号**版本
 *
 * 绝大多数调用点是 include/string.h 里的 static inline 版本（会被内联，
 * 或者退化成调用点自己的副本）。但编译器**自己生成**的调用 —— 大结构体整体
 * 赋值、循环→memcpy 变换等 —— 会直接发一个对本文件这些符号的引用：
 *
 *     0xc0c: bl memcpy
 *
 * 所以这里必须提供外部符号，而且必须和 inline 版本走**同一份实现**
 * （string_internal.h 里的 *_arch），否则就会出现"源码里写的 memcpy 是 NEON，
 * 编译器生成的 memcpy 是逐字节循环"这种分裂 —— fork/clone 拷贝 816 字节
 * trap frame 正好踩过这个坑。
 *
 * 注意：本文件包含的是实现层头，不是 string.h —— 后者的 static inline
 * memcpy/memset 与本文件要定义的外部符号同名，同一翻译单元里内部链接与外部
 * 链接同名是未定义行为（C11 6.2.2p7）。详见 include/string_internal.h 顶部。
 */

#include "string_internal.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    return memcpy_arch(dst, src, n);
}

void *memset(void *s, int c, size_t n)
{
    return memset_arch(s, c, n);
}

void *memmove(void *dst, const void *src, size_t n)
{
    return memmove_generic(dst, src, n);
}
