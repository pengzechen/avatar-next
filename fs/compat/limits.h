/*
 * fs/compat/limits.h — freestanding 环境下 limits.h 的最小替代
 *
 * 仅提供 lwext4 实际用到的常量。
 */
#ifndef _COMPAT_LIMITS_H
#define _COMPAT_LIMITS_H

/* 无符号 char 最大值 */
#define UCHAR_MAX   255U

/* char 范围（lwext4 偶有使用） */
#define CHAR_BIT    8
#define SCHAR_MIN   (-128)
#define SCHAR_MAX   127
#define CHAR_MIN    0
#define CHAR_MAX    UCHAR_MAX

/* 常用整型上界 */
#define SHRT_MAX    32767
#define USHRT_MAX   65535U
#define INT_MAX     2147483647
#define INT_MIN     (-INT_MAX - 1)
#define UINT_MAX    4294967295U
#define LONG_MAX    9223372036854775807L
#define LONG_MIN    (-LONG_MAX - 1L)
#define ULONG_MAX   18446744073709551615UL

#endif /* _COMPAT_LIMITS_H */
