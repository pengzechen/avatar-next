/*
 * fs/lwext4_port/libc_shim/inttypes.h - 为 lwext4 提供 inttypes.h 兼容层
 * 定义 PRI* 格式化宏
 */
#ifndef _COMPAT_INTTYPES_H_
#define _COMPAT_INTTYPES_H_

#include <types.h>

/* 64-bit */
#ifndef PRId64
#define PRId64  "lld"
#endif
#ifndef PRIu64
#define PRIu64  "llu"
#endif
#ifndef PRIx64
#define PRIx64  "llx"
#endif
#ifndef PRIX64
#define PRIX64  "llX"
#endif

/* 32-bit */
#ifndef PRId32
#define PRId32  "d"
#endif
#ifndef PRIu32
#define PRIu32  "u"
#endif
#ifndef PRIx32
#define PRIx32  "x"
#endif

/* 16-bit */
#ifndef PRId16
#define PRId16  "hd"
#endif
#ifndef PRIu16
#define PRIu16  "hu"
#endif

/* 8-bit */
#ifndef PRId8
#define PRId8   "hhd"
#endif
#ifndef PRIu8
#define PRIu8   "hhu"
#endif

#endif /* _COMPAT_INTTYPES_H_ */
