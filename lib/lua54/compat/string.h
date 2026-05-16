/* lib/lua54/compat/string.h — supplement kernel string.h with missing functions
 *
 * Include order in LUA_CFLAGS: -Ilib/lua54/compat is placed BEFORE -Iinclude,
 * so THIS file is found for <string.h> when compiling Lua sources.
 * We include the kernel's string.h via an explicit path and add what's missing.
 */
#ifndef _COMPAT_STRING_H
#define _COMPAT_STRING_H

/* Pull in the kernel's string.h (memcpy, memmove, memset, memcmp,
 * strlen, strcmp, strncmp, strchr, strstr, strncpy, strcpy, strcat). */
#include <../include/string.h>

#include <stddef.h>   /* size_t */

/* strspn — count leading chars in s that are all in accept */
static inline __attribute__((unused))
size_t strspn(const char *s, const char *accept)
{
    size_t count = 0;
    while (s[count]) {
        const char *a = accept;
        int found = 0;
        while (*a) {
            if (s[count] == *a++) { found = 1; break; }
        }
        if (!found) break;
        count++;
    }
    return count;
}

/* strcspn — count leading chars in s that are NOT in reject */
static inline __attribute__((unused))
size_t strcspn(const char *s, const char *reject)
{
    size_t count = 0;
    while (s[count]) {
        const char *r = reject;
        while (*r) {
            if (s[count] == *r++) goto done;
        }
        count++;
    }
done:
    return count;
}

/* strpbrk — find first char in s that appears in accept */
static inline __attribute__((unused))
char *strpbrk(const char *s, const char *accept)
{
    while (*s) {
        const char *a = accept;
        while (*a) {
            if (*s == *a++) return (char *)s;
        }
        s++;
    }
    return (char *)0;
}

/* strerror — kernel doesn't have errno; return static description */
static inline __attribute__((unused))
char *strerror(int errnum)
{
    (void)errnum;
    return (char *)"error";
}

/* strcoll — locale-aware compare; without locale support, same as strcmp */
static inline __attribute__((unused))
int strcoll(const char *a, const char *b)
{
    return strcmp(a, b);
}

#endif /* _COMPAT_STRING_H */
