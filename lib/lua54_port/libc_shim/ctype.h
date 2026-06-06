/* lib/lua54_port/libc_shim/ctype.h — ASCII ctype functions for Lua */
#ifndef _COMPAT_CTYPE_H
#define _COMPAT_CTYPE_H

static inline __attribute__((unused)) int isalpha(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static inline __attribute__((unused)) int isdigit(int c)
{
    return c >= '0' && c <= '9';
}

static inline __attribute__((unused)) int isalnum(int c)
{
    return isalpha(c) || isdigit(c);
}

static inline __attribute__((unused)) int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' ||
           c == '\r' || c == '\f' || c == '\v';
}

static inline __attribute__((unused)) int isupper(int c)
{
    return c >= 'A' && c <= 'Z';
}

static inline __attribute__((unused)) int islower(int c)
{
    return c >= 'a' && c <= 'z';
}

static inline __attribute__((unused)) int toupper(int c)
{
    return islower(c) ? c - 32 : c;
}

static inline __attribute__((unused)) int tolower(int c)
{
    return isupper(c) ? c + 32 : c;
}

static inline __attribute__((unused)) int iscntrl(int c)
{
    return (c >= 0 && c < 32) || c == 127;
}

static inline __attribute__((unused)) int ispunct(int c)
{
    return (c >= 33 && c <= 47) || (c >= 58 && c <= 64) ||
           (c >= 91 && c <= 96) || (c >= 123 && c <= 126);
}

static inline __attribute__((unused)) int isgraph(int c)
{
    return c > 32 && c < 127;
}

static inline __attribute__((unused)) int isprint(int c)
{
    return c >= 32 && c < 127;
}

static inline __attribute__((unused)) int isxdigit(int c)
{
    return isdigit(c) ||
           (c >= 'A' && c <= 'F') ||
           (c >= 'a' && c <= 'f');
}

#endif /* _COMPAT_CTYPE_H */
