#ifndef _FREESTANDING_STDLIB_H
#define _FREESTANDING_STDLIB_H

#include "types.h"

int atoi(const char *s);

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));

#endif /* _FREESTANDING_STDLIB_H */
