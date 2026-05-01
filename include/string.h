#ifndef __STRING_H
#define __STRING_H

#include "types.h"
#include "arch.h"

/* ===== 通用字符串函数实现 (static inline) ===== */

static inline size_t
strlen(const char *buf)
{
    unsigned long len = 0;
    while (*buf++)
        ++len;
    return len;
}

static inline char *
strcat(char *dest, const char *src)
{
    char *p = dest;
    while (*p)
        ++p;
    while ((*p++ = *src++) != 0)
        ;
    return dest;
}

static inline char *
strcpy(char *dest, const char *src)
{
    char *p = dest;
    while ((*p++ = *src++) != 0)
        ;
    return dest;
}

static inline char *
strncpy(char *dest, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return dest;
}

static inline int
strncmp(const char *a, const char *b, size_t n)
{
    for (; n--; ++a, ++b)
        if (*a != *b || *a == '\0')
            return *a - *b;
    return 0;
}

static inline int
strcmp(const char *a, const char *b)
{
    return strncmp(a, b, SIZE_MAX);
}

static inline char *
strchr(const char *s, int c)
{
    while (*s != (char) c)
        if (*s++ == '\0')
            return NULL;
    return (char *) s;
}

static inline int
memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *a = s1, *b = s2;
    int                  ret = 0;
    while (n--) {
        ret = *a - *b;
        if (ret)
            break;
        ++a, ++b;
    }
    return ret;
}

static inline char *
strstr(const char *s1, const char *s2)
{
    size_t l1, l2;
    l2 = strlen(s2);
    if (!l2)
        return (char *) s1;
    l1 = strlen(s1);
    while (l1 >= l2) {
        l1--;
        if (!memcmp(s1, s2, l2))
            return (char *) s1;
        s1++;
    }
    return NULL;
}

static inline void *
memset(void *s, int c, size_t n)
{
    size_t i;
    char  *a = s;
    for (i = 0; i < n; ++i)
        a[i] = c;
    return s;
}

static inline void *
memmove(void *dest, const void *src, size_t n)
{
    const unsigned char *s = src;
    unsigned char       *d = dest;
    if (d <= s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n, s += n;
        while (n--)
            *--d = *--s;
    }
    return dest;
}

static inline void *
memchr(const void *s, int c, size_t n)
{
    const unsigned char *str = s, chr = (unsigned char) c;
    while (n--)
        if (*str++ == chr)
            return (void *) (str - 1);
    return NULL;
}

static inline long
atol(const char *ptr)
{
    long        acc = 0;
    const char *s   = ptr;
    int         neg, c;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-') {
        neg = 1;
        s++;
    } else {
        neg = 0;
        if (*s == '+')
            s++;
    }
    while (*s) {
        if (*s < '0' || *s > '9')
            break;
        c   = *s - '0';
        acc = acc * 10 + c;
        s++;
    }
    if (neg)
        acc = -acc;
    return acc;
}

/* ===== memcpy 架构优化实现 ===== */

/* 通用 memcpy 实现 */
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

/* 根据架构选择优化的 memcpy 实现 */
#if defined(ARCH_X86_64)
    #include "x86_64/string_impl.h"
#elif defined(ARCH_AARCH64)
    #include "aarch64/string_impl.h"
#elif defined(ARCH_RISCV64)
    #include "riscv64/string_impl.h"
#else
    /* 使用通用实现 */
    static inline void *
    memcpy(void *dest, const void *src, size_t n)
    {
        return memcpy_generic(dest, src, n);
    }
#endif

#endif /* __STRING_H */
