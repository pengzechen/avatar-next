/*
 * 字符串函数实现
 *
 * 所有字符串函数已在 include/string.h 中以 static inline 形式实现，
 * 架构优化版本由各架构的 string_impl.h 提供。
 *
 * 此文件提供非内联版本，供编译器生成的外部引用使用（如数组初始化）。
 */

#include "types.h"

/*
 * memset - 填充内存
 *
 * 当编译器需要 memset 作为外部符号时（如数组初始化），
 * 使用此非内联版本。
 *
 * 注意：不包含 string.h 以避免与 static inline 版本冲突
 */
void *memset(void *s, int c, size_t n)
{
    size_t i;
    char  *a = s;
    for (i = 0; i < n; ++i)
        a[i] = (char)c;
    return s;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    char       *d = dst;
    const char *s = src;
    size_t      i;
    for (i = 0; i < n; ++i)
        d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    char       *d = dst;
    const char *s = src;
    size_t      i;
    if (d < s || d >= s + n) {
        for (i = 0; i < n; ++i)
            d[i] = s[i];
    } else {
        for (i = n; i > 0; --i)
            d[i - 1] = s[i - 1];
    }
    return dst;
}
