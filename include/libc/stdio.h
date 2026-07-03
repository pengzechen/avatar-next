#ifndef _FREESTANDING_STDIO_H
#define _FREESTANDING_STDIO_H

#include "types.h"

int printf(const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);

#endif /* _FREESTANDING_STDIO_H */
