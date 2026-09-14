#include "pseudofs_internal.h"
#include "string.h"

size_t pfs_strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int pfs_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int pfs_strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n ? ((unsigned char)*a - (unsigned char)*b) : 0;
}

int u64_to_dec(char *buf, uint64_t v)
{
    if (v == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }

    char tmp[24];
    int n = 0;
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (int i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return n;
}

int pfs_puts(char *buf, size_t pos, size_t bufsz, const char *s)
{
    int written = 0;
    while (*s && pos < bufsz) {
        buf[pos++] = *s++;
        written++;
    }
    return written;
}

int pfs_copy_out(uint64_t off, void *buf, size_t len, const char *src, size_t total)
{
    if ((size_t)off >= total)
        return 0;
    size_t avail = total - (size_t)off;
    size_t copy = avail < len ? avail : len;
    memcpy(buf, src + off, copy);
    return (int)copy;
}
