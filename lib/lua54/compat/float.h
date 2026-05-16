/* lib/lua54/compat/float.h — IEEE 754 double/float constants for Lua */
#ifndef _COMPAT_FLOAT_H
#define _COMPAT_FLOAT_H

/* Radix of all floating-point types */
#define FLT_RADIX          2

/* double precision (64-bit IEEE 754) */
#define DBL_MANT_DIG       53
#define DBL_MAX_EXP        1024
#define DBL_MIN_EXP        (-1021)
#define DBL_DIG            15
#define DBL_MAX_10_EXP     308
#define DBL_MIN_10_EXP     (-307)
#define DBL_EPSILON        2.2204460492503131e-16
#define DBL_MAX            1.7976931348623157e+308
#define DBL_MIN            2.2250738585072014e-308
#define DBL_TRUE_MIN       5.0e-324

/* single precision (32-bit IEEE 754) */
#define FLT_MANT_DIG       24
#define FLT_MAX_EXP        128
#define FLT_MIN_EXP        (-125)
#define FLT_DIG            6
#define FLT_MAX_10_EXP     38
#define FLT_MIN_10_EXP     (-37)
#define FLT_EPSILON        1.19209290e-07f
#define FLT_MAX            3.40282347e+38f
#define FLT_MIN            1.17549435e-38f
#define FLT_TRUE_MIN       1.4e-45f

/* long double — treat as double in freestanding */
#define LDBL_MANT_DIG      DBL_MANT_DIG
#define LDBL_MAX_EXP       DBL_MAX_EXP
#define LDBL_MIN_EXP       DBL_MIN_EXP
#define LDBL_DIG           DBL_DIG
#define LDBL_MAX_10_EXP    DBL_MAX_10_EXP
#define LDBL_MIN_10_EXP    DBL_MIN_10_EXP
#define LDBL_EPSILON       (long double)DBL_EPSILON
#define LDBL_MAX           (long double)DBL_MAX
#define LDBL_MIN           (long double)DBL_MIN

#endif /* _COMPAT_FLOAT_H */
