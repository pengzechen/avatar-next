#ifndef __STRING_H
#define __STRING_H

#include "types.h"
#include "arch.h"

/*
 * 实现层（memcpy_generic / memcpy_arch / memcpy_neon …）在这里。
 * 与公开 API 分开的原因见 string_internal.h 顶部：lib/string.c 要用同样的
 * 名字定义外部符号，实现不能占用公开名字。
 */
#include "string_internal.h"

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
    return memset_arch(s, c, n);
}

static inline void *
memmove(void *dest, const void *src, size_t n)
{
    return memmove_generic(dest, src, n);
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

/* ===== memcpy ===== */

/*
 * 公开 API 一律转发到实现层（string_internal.h 里的 *_arch）。
 * lib/string.c 用同样的 *_arch 定义外部符号，所以源码里的调用和编译器自动
 * 生成的调用（大结构体赋值、循环→memcpy 变换）走的是同一份实现。
 */
static inline void *
memcpy(void *dest, const void *src, size_t n)
{
    return memcpy_arch(dest, src, n);
}

#endif /* __STRING_H */
