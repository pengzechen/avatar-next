/* lib/lua54/compat/assert.h — freestanding assert for Lua */
#ifndef _COMPAT_ASSERT_H
#define _COMPAT_ASSERT_H

/* Forward declaration to avoid pulling in all of klog.h */
extern void kprintf(const char *fmt, ...);

#ifdef NDEBUG
#define assert(expr)  ((void)(expr))
#else
#define assert(expr)                                             \
    do {                                                         \
        if (!(expr)) {                                           \
            kprintf("Lua ASSERT FAILED: %s  [%s:%d]\n",         \
                    #expr, __FILE__, __LINE__);                  \
            while (1) { __asm__ volatile("" ::: "memory"); }    \
        }                                                        \
    } while (0)
#endif

#endif /* _COMPAT_ASSERT_H */
