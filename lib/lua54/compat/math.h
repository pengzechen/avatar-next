/* lib/lua54/compat/math.h — IEEE 754 math declarations for Lua freestanding */
#ifndef _COMPAT_MATH_H
#define _COMPAT_MATH_H

/* Constants using GCC builtins (work in any mode) */
#define HUGE_VAL   __builtin_huge_val()
#define HUGE_VALF  __builtin_huge_valf()
#define HUGE_VALL  __builtin_huge_vall()
#define NAN        __builtin_nan("")
#define INFINITY   __builtin_inff()

/* Classification macros */
#define isinf(x)     __builtin_isinf(x)
#define isnan(x)     __builtin_isnan(x)
#define isfinite(x)  __builtin_isfinite(x)
#define isnormal(x)  __builtin_isnormal(x)
#define signbit(x)   __builtin_signbit(x)

/* Function declarations — implemented in lib/lua54/compat/lua_math_impl.c */
double floor(double x);
double ceil(double x);
double fabs(double x);
double fmod(double x, double y);
double pow(double x, double y);
double sqrt(double x);
double frexp(double x, int *exp);
double ldexp(double x, int exp);
double modf(double x, double *iptr);
double log(double x);
double log2(double x);
double exp(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);

/* float versions — redirect to double */
static inline float floorf(float x)       { return (float)floor((double)x); }
static inline float ceilf(float x)        { return (float)ceil((double)x); }
static inline float fabsf(float x)        { return (float)fabs((double)x); }
static inline float sqrtf(float x)        { return (float)sqrt((double)x); }
static inline float powf(float x, float y){ return (float)pow((double)x,(double)y); }

/* M_PI and friends */
#define M_E         2.71828182845904523536
#define M_LOG2E     1.44269504088896340736
#define M_LOG10E    0.434294481903251827651
#define M_LN2       0.693147180559945309417
#define M_LN10      2.30258509299404568402
#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.785398163397448309616
#define M_SQRT2     1.41421356237309504880

#endif /* _COMPAT_MATH_H */
