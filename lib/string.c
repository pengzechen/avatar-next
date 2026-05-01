/*
 * 字符串函数实现
 *
 * 这些函数为编译器选择不内联 static inline 函数时提供后备实现
 */

#include "types.h"

/* strlen 实现 */
size_t
strlen(const char *buf)
{
    size_t len = 0;
    while (*buf++)
        ++len;
    return len;
}

/* memcpy 通用实现 */
static inline void *
memcpy_generic(void *dest, const void *src, size_t n)
{
    size_t         i = 0;
    uint8_t       *d = (uint8_t *) dest;
    const uint8_t *s = (const uint8_t *) src;

    /* 逐字节拷贝直到对齐 */
    while (i < n && ((uint64_t) (d + i) % 2 != 0 || (uint64_t) (s + i) % 2 != 0)) {
        d[i] = s[i];
        i++;
    }

    /* 8 字节拷贝 */
    while (i + 7 < n && ((uint64_t) (d + i) % 8 == 0) && ((uint64_t) (s + i) % 8 == 0)) {
        *((uint64_t *) (d + i)) = *((uint64_t *) (s + i));
        i += 8;
    }

    /* 4 字节拷贝 */
    while (i + 3 < n && ((uint64_t) (d + i) % 4 == 0) && ((uint64_t) (s + i) % 4 == 0)) {
        *((uint32_t *) (d + i)) = *((uint32_t *) (s + i));
        i += 4;
    }

    /* 2 字节拷贝 */
    while (i + 1 < n && ((uint64_t) (d + i) % 2 == 0) && ((uint64_t) (s + i) % 2 == 0)) {
        *((uint16_t *) (d + i)) = *((uint16_t *) (s + i));
        i += 2;
    }

    /* 剩余逐字节拷贝 */
    while (i < n) {
        d[i] = s[i];
        i++;
    }

    return dest;
}

/* memcpy 实现 */
void *
memcpy(void *dest, const void *src, size_t n)
{
    return memcpy_generic(dest, src, n);
}

/* memset 实现 */
void *
memset(void *s, int c, size_t n)
{
    size_t i;
    char  *a = s;
    for (i = 0; i < n; ++i)
        a[i] = c;
    return s;
}

/* memcmp 实现 */
int
memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *a = s1, *b = s2;
    int                  ret = 0;
    while (n--) {
        ret = *a - *b;
        if (ret)
            break;
        ++a;
        ++b;
    }
    return ret;
}
