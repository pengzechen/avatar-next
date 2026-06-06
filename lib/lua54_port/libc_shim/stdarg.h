/* lib/lua54_port/libc_shim/stdarg.h — freestanding va_list for Lua */
#ifndef _COMPAT_STDARG_H
#define _COMPAT_STDARG_H

typedef __builtin_va_list va_list;
#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)
#define va_copy(dst, src)   __builtin_va_copy(dst, src)

#endif /* _COMPAT_STDARG_H */
