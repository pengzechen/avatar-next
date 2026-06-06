/*
 * fs/lwext4_port/libc_shim/stddef.h - 为 lwext4 提供 stddef.h 兼容层
 */
#ifndef _COMPAT_STDDEF_H_
#define _COMPAT_STDDEF_H_

#include <types.h>

/* offsetof 使用 GCC 内置实现，无需标准库 */
#ifndef offsetof
#define offsetof(type, member)  __builtin_offsetof(type, member)
#endif

#endif /* _COMPAT_STDDEF_H_ */
