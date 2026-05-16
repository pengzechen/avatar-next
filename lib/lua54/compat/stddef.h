/* lib/lua54/compat/stddef.h — freestanding stddef for Lua */
#ifndef _COMPAT_STDDEF_H
#define _COMPAT_STDDEF_H

#include <types.h>   /* uint8_t, uint64_t, uintptr_t, ... */

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

typedef __PTRDIFF_TYPE__ ptrdiff_t;

#endif /* _COMPAT_STDDEF_H */
