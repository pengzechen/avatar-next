/* lib/lua54/compat/stdint.h — redirect to kernel types */
#ifndef _COMPAT_STDINT_H
#define _COMPAT_STDINT_H

#include <types.h>

/* Lua uses SIZE_MAX and PTRDIFF_MAX */
#ifndef SIZE_MAX
#define SIZE_MAX  ((size_t)-1)
#endif

#ifndef PTRDIFF_MAX
#define PTRDIFF_MAX  ((ptrdiff_t)(SIZE_MAX >> 1))
#endif

#ifndef PTRDIFF_MIN
#define PTRDIFF_MIN  ((ptrdiff_t)(-PTRDIFF_MAX - 1))
#endif

/* uintmax_t: largest unsigned integer type */
#ifndef uintmax_t
typedef unsigned long long uintmax_t;
#endif
#ifndef UINTMAX_MAX
#define UINTMAX_MAX  ((uintmax_t)-1)
#endif
#ifndef intmax_t
typedef long long intmax_t;
#endif
#ifndef INTMAX_MAX
#define INTMAX_MAX  ((intmax_t)(UINTMAX_MAX >> 1))
#endif

#endif /* _COMPAT_STDINT_H */
