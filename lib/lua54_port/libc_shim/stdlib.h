/* lib/lua54_port/libc_shim/stdlib.h — freestanding stdlib stubs for Lua */
#ifndef _COMPAT_STDLIB_H
#define _COMPAT_STDLIB_H

#include <stddef.h>   /* size_t */

/* Memory allocation — implemented in lib/lua54_port/platform.c */
void *malloc(size_t size);
void  free(void *ptr);
void *realloc(void *ptr, size_t size);

/* Number conversion — implemented in lib/lua54_port/libc_shim/lua_math_impl.c */
double        strtod(const char *s, char **endptr);
float         strtof(const char *s, char **endptr);
long double   strtold(const char *s, char **endptr);
long          strtol(const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);

/* Abort / exit — just hang */
#define abort()  do { while (1) { __asm__ volatile("" ::: "memory"); } } while (0)
#define exit(c)  abort()

/* Absolute value for integer types */
static inline int  abs(int x)  { return x < 0 ? -x : x; }

#endif /* _COMPAT_STDLIB_H */
