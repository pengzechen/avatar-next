/* lib/lua54_port/libc_shim/stdio.h — freestanding stdio stubs for Lua */
#ifndef _COMPAT_STDIO_H
#define _COMPAT_STDIO_H

#include <stdarg.h>   /* va_list */
#include <stddef.h>   /* size_t  */

/* Opaque FILE type — Lua only uses it as a pointer */
typedef void FILE;

/* stderr / stdout — treated as no-op targets (kprintf always outputs) */
#define stderr  ((FILE *)0)
#define stdout  ((FILE *)0)
#define stdin   ((FILE *)0)

#define EOF  (-1)
#define BUFSIZ  8192  /* standard I/O buffer size */

/* Declarations — implemented in lib/lua54_port/platform.c */
int  printf(const char *fmt, ...)
     __attribute__((format(printf, 1, 2)));
int  fprintf(FILE *f, const char *fmt, ...)
     __attribute__((format(printf, 2, 3)));
int  vfprintf(FILE *f, const char *fmt, va_list ap);
int  snprintf(char *buf, size_t n, const char *fmt, ...)
     __attribute__((format(printf, 3, 4)));
int  vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int  fflush(FILE *f);

/* fputs / fwrite — map to printf so Lua's print() produces output */
static inline int fputs(const char *s, FILE *f)
{
    (void)f;
    return printf("%s", s);
}

static inline size_t fwrite(const void *buf, size_t sz, size_t n, FILE *f)
{
    /* Lua strings are always null-terminated; printf("%s") is safe here.
     * sz*n gives the byte count but we rely on the terminator for output. */
    (void)sz; (void)n; (void)f;
    if (buf)
        printf("%s", (const char *)buf);
    return n;
}

/* Minimal file I/O stubs — luaL_loadfile will fail gracefully */
typedef void *  LUACOMPAT_FILE_HANDLE;
#define fopen(path, mode)   ((FILE *)0)
#define fclose(fp)          (0)
#define freopen(p,m,f)      ((FILE *)0)
#define ferror(fp)          (1)
#define fread(b,s,n,f)      ((size_t)0)
#define feof(f)             (1)
#define clearerr(f)         ((void)(f))
#define ungetc(c, f)        (EOF)
#define getc(f)             (EOF)

#endif /* _COMPAT_STDIO_H */
