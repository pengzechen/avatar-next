#include "types.h"
#include "string.h"

int atoi(const char *s)
{
    int sign = 1;
    int value = 0;

    if (!s)
        return 0;

    while (*s == ' ' || *s == '\t' || *s == '\n' ||
           *s == '\r' || *s == '\f' || *s == '\v')
        s++;

    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }

    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        s++;
    }

    return sign * value;
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
    if (nmemb <= 1u || size == 0u)
        return;

    uint8_t *arr = (uint8_t *)base;
    uint8_t tmp[256];
    if (size > sizeof(tmp))
        return;

    for (size_t i = 1u; i < nmemb; i++) {
        memcpy(tmp, arr + i * size, size);
        size_t j = i;
        while (j > 0u && compar(arr + (j - 1u) * size, tmp) > 0) {
            memcpy(arr + j * size, arr + (j - 1u) * size, size);
            j--;
        }
        memcpy(arr + j * size, tmp, size);
    }
}
