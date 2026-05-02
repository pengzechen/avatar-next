/*
 * compat/stdint.h - 为 lwext4 提供 stdint.h 兼容层
 * 转发到项目自身的 types.h
 */
#ifndef _COMPAT_STDINT_H_
#define _COMPAT_STDINT_H_

#include <types.h>

/* INT*_MAX / UINT*_MAX 已在 types.h 中定义 */

/* intmax_t / uintmax_t */
typedef int64_t  intmax_t;
typedef uint64_t uintmax_t;

#define INTMAX_MAX   INT64_MAX
#define INTMAX_MIN   INT64_MIN
#define UINTMAX_MAX  UINT64_MAX

#define INTPTR_MIN   INT64_MIN
#define INTPTR_MAX   INT64_MAX
#define UINTPTR_MAX  UINT64_MAX

#define PTRDIFF_MIN  INT64_MIN
#define PTRDIFF_MAX  INT64_MAX

#endif /* _COMPAT_STDINT_H_ */
