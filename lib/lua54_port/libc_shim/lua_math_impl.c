/*
 * lib/lua54_port/libc_shim/lua_math_impl.c
 *
 * Freestanding math + string conversion implementations for Lua 5.4.
 * MUST be compiled with FP enabled (no -mgeneral-regs-only, etc.)
 * Use LUA_CFLAGS rather than CFLAGS.
 */

/* Avoid re-declaring via our own compat headers */
#include <stddef.h>   /* size_t */
#include <errno.h>    /* _lua_errno */

/* errno storage */
int _lua_errno = 0;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

typedef unsigned long long u64;
typedef long long           i64;

typedef union { double d; u64 u; } dbl_bits_t;

#define DBL_EXP_MASK   0x7FF0000000000000ULL
#define DBL_MANT_MASK  0x000FFFFFFFFFFFFFULL
#define DBL_SIGN_MASK  0x8000000000000000ULL
#define DBL_EXP_BIAS   1023

/* ── fabs ────────────────────────────────────────────────────────────────── */
double fabs(double x)
{
    dbl_bits_t b = { .d = x };
    b.u &= ~DBL_SIGN_MASK;
    return b.d;
}

/* ── floor ───────────────────────────────────────────────────────────────── */
double floor(double x)
{
    if (x != x) return x;          /* NaN */
    i64 n = (i64)x;
    double d = (double)n;
    return d > x ? d - 1.0 : d;
}

/* ── ceil ────────────────────────────────────────────────────────────────── */
double ceil(double x)
{
    if (x != x) return x;
    i64 n = (i64)x;
    double d = (double)n;
    return d < x ? d + 1.0 : d;
}

/* ── fmod ────────────────────────────────────────────────────────────────── */
double fmod(double x, double y)
{
    if (y == 0.0) return __builtin_nan("");
    double q = x / y;
    i64    n = (i64)q;
    return x - (double)n * y;
}

/* ── modf ────────────────────────────────────────────────────────────────── */
double modf(double x, double *iptr)
{
    double i = floor(fabs(x));
    if (x < 0.0) i = -i;
    *iptr = i;
    return x - i;
}

/* ── frexp ───────────────────────────────────────────────────────────────── */
double frexp(double x, int *exp)
{
    dbl_bits_t b = { .d = x };
    int e;

    if (x == 0.0) { *exp = 0; return x; }
    if (x != x || x == (1.0 / 0.0) || x == (-1.0 / 0.0)) {
        *exp = 0; return x;
    }

    e = (int)((b.u & DBL_EXP_MASK) >> 52) - DBL_EXP_BIAS;
    /* Clear exponent, set exponent to -1 (value in [0.5, 1.0)) */
    b.u = (b.u & ~DBL_EXP_MASK) | ((u64)(DBL_EXP_BIAS - 1) << 52);
    *exp = e + 1;
    return b.d;
}

/* ── ldexp ───────────────────────────────────────────────────────────────── */
double ldexp(double x, int exp)
{
    dbl_bits_t b = { .d = x };
    int e;

    if (x == 0.0 || x != x) return x;

    e = (int)((b.u & DBL_EXP_MASK) >> 52);
    e += exp;

    if (e <= 0) return 0.0;              /* underflow */
    if (e >= 2047) return __builtin_huge_val(); /* overflow */

    b.u = (b.u & ~DBL_EXP_MASK) | ((u64)e << 52);
    return b.d;
}

/* ── sqrt ────────────────────────────────────────────────────────────────── */
double sqrt(double x)
{
    return __builtin_sqrt(x);
}

/* ── pow ─────────────────────────────────────────────────────────────────── */
double pow(double base, double exp)
{
    /* Handle special cases required by Lua (luai_numpow uses this) */
    if (exp == 0.0) return 1.0;
    if (exp == 1.0) return base;
    if (base == 0.0) return 0.0;

    /* Integer exponent via repeated squaring */
    i64 n = (i64)exp;
    if ((double)n == exp) {
        int neg = 0;
        double result = 1.0;
        u64 p;
        double b = base;
        if (n < 0) { neg = 1; n = -n; }
        p = (u64)n;
        while (p) {
            if (p & 1) result *= b;
            b *= b;
            p >>= 1;
        }
        return neg ? 1.0 / result : result;
    }

    /* General case: x^y = exp(y * ln(x))
     * We implement ln and exp via their Taylor series. */

    /* Only handle positive base in general case */
    if (base < 0.0) return __builtin_nan("");

    /* ln(x) via ln(x) = 2*atanh((x-1)/(x+1))  for x > 0 */
    double lnx;
    {
        /* reduce x to [1, 2) by extracting exponent */
        int e2;
        double m = frexp(base, &e2);  /* base = m * 2^e2, m in [0.5, 1) */
        m *= 2.0;  e2--;              /* m in [1, 2) */
        /* ln(m): use ln(1+t) Taylor for t = m-1 in (-0.5, 1) approx */
        double t = (m - 1.0) / (m + 1.0);
        double t2 = t * t;
        /* atanh(t) = t + t^3/3 + t^5/5 + ... (12 terms sufficient) */
        double s = t;
        double tp = t * t2;
        s += tp / 3.0;  tp *= t2;
        s += tp / 5.0;  tp *= t2;
        s += tp / 7.0;  tp *= t2;
        s += tp / 9.0;  tp *= t2;
        s += tp / 11.0; tp *= t2;
        s += tp / 13.0; tp *= t2;
        s += tp / 15.0; tp *= t2;
        s += tp / 17.0; tp *= t2;
        s += tp / 19.0;
        lnx = 2.0 * s + (double)e2 * 0.6931471805599453; /* ln(2) */
    }

    /* exp(y * lnx) */
    double yl = exp * lnx;
    {
        /* exp(x) = exp(n) * exp(f),  n = floor(x), f in [0,1)
         * exp(n) via ldexp(1, n/ln2 rounded)
         * We do: reduce to [-ln2/2, ln2/2] and use Taylor */
        double ln2 = 0.6931471805599453;
        i64 n2 = (i64)(yl / ln2);
        double f = yl - (double)n2 * ln2;
        /* Taylor: e^f = 1 + f + f^2/2! + ... (10 terms) */
        double ef = 1.0 + f;
        double fp = f * f;
        double fact = 2.0;
        ef += fp / fact; fp *= f; fact *= 3.0;
        ef += fp / fact; fp *= f; fact *= 4.0;
        ef += fp / fact; fp *= f; fact *= 5.0;
        ef += fp / fact; fp *= f; fact *= 6.0;
        ef += fp / fact; fp *= f; fact *= 7.0;
        ef += fp / fact; fp *= f; fact *= 8.0;
        ef += fp / fact; fp *= f; fact *= 9.0;
        ef += fp / fact; fp *= f; fact *= 10.0;
        ef += fp / fact;
        return ldexp(ef, (int)n2);
    }
}

/* ── log / log2 / exp ────────────────────────────────────────────────────── */
double log(double x)
{
    if (x <= 0.0) return __builtin_nan("");
    if (x == 1.0) return 0.0;
    int e2;
    double m = frexp(x, &e2);
    m *= 2.0; e2--;
    double t  = (m - 1.0) / (m + 1.0);
    double t2 = t * t, tp = t * t2, s = t;
    s += tp/3.0;  tp *= t2;
    s += tp/5.0;  tp *= t2;
    s += tp/7.0;  tp *= t2;
    s += tp/9.0;  tp *= t2;
    s += tp/11.0; tp *= t2;
    s += tp/13.0; tp *= t2;
    s += tp/15.0; tp *= t2;
    s += tp/17.0; tp *= t2;
    s += tp/19.0;
    return 2.0 * s + (double)e2 * 0.6931471805599453;
}

double log2(double x)
{
    return log(x) / 0.6931471805599453;
}

double exp(double x)
{
    double ln2 = 0.6931471805599453;
    i64 n = (i64)(x / ln2);
    double f = x - (double)n * ln2;
    double ef = 1.0 + f;
    double fp = f * f, fact = 2.0;
    ef += fp/fact; fp*=f; fact*=3.0;
    ef += fp/fact; fp*=f; fact*=4.0;
    ef += fp/fact; fp*=f; fact*=5.0;
    ef += fp/fact; fp*=f; fact*=6.0;
    ef += fp/fact; fp*=f; fact*=7.0;
    ef += fp/fact; fp*=f; fact*=8.0;
    ef += fp/fact; fp*=f; fact*=9.0;
    ef += fp/fact; fp*=f; fact*=10.0;
    ef += fp/fact;
    return ldexp(ef, (int)n);
}

/* ── trig stubs (not needed by base Lua but declared in math.h) ──────────── */
double sin(double x)  { (void)x; return 0.0; }
double cos(double x)  { (void)x; return 1.0; }
double tan(double x)  { (void)x; return 0.0; }
double asin(double x) { (void)x; return 0.0; }
double acos(double x) { (void)x; return 0.0; }
double atan(double x) { (void)x; return 0.0; }
double atan2(double y, double x) { (void)y; (void)x; return 0.0; }
double sinh(double x) { (void)x; return 0.0; }
double cosh(double x) { (void)x; return 0.0; }
double tanh(double x) { (void)x; return 0.0; }

/* ── strtod ──────────────────────────────────────────────────────────────── */
static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int is_xdigit(char c)
{
    return is_digit(c) || (c>='a' && c<='f') || (c>='A' && c<='F');
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

double strtod(const char *s, char **endptr)
{
    const char *p = s;
    int sign = 1;
    double result = 0.0;

    /* Skip leading whitespace */
    while (is_space(*p)) p++;

    /* Sign */
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') { p++; }

    /* Check for inf/nan */
    if ((p[0]=='i'||p[0]=='I') && (p[1]=='n'||p[1]=='N') && (p[2]=='f'||p[2]=='F')) {
        if (endptr) *endptr = (char *)(p + 3);
        return sign * __builtin_huge_val();
    }
    if ((p[0]=='n'||p[0]=='N') && (p[1]=='a'||p[1]=='A') && (p[2]=='n'||p[2]=='N')) {
        if (endptr) *endptr = (char *)(p + 3);
        return __builtin_nan("");
    }

    const char *start = p;

    /* Hexadecimal float: 0x... */
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        double intpart = 0.0;
        while (is_xdigit(*p)) { intpart = intpart * 16.0 + hex_val(*p); p++; }
        double fracpart = 0.0;
        if (*p == '.') {
            p++;
            double scale = 1.0 / 16.0;
            while (is_xdigit(*p)) { fracpart += hex_val(*p) * scale; scale /= 16.0; p++; }
        }
        result = (intpart + fracpart) * (double)sign;
        /* binary exponent */
        if (*p == 'p' || *p == 'P') {
            p++;
            int esign = 1;
            if (*p == '+') p++;
            else if (*p == '-') { esign = -1; p++; }
            int ex = 0;
            while (is_digit(*p)) { ex = ex * 10 + (*p - '0'); p++; }
            result = ldexp(result, esign * ex);
        }
        if (endptr) *endptr = (char *)p;
        return result;
    }

    /* Decimal integer part */
    while (is_digit(*p)) { result = result * 10.0 + (*p - '0'); p++; }

    /* Decimal fraction part */
    if (*p == '.') {
        p++;
        double scale = 0.1;
        while (is_digit(*p)) { result += (*p - '0') * scale; scale *= 0.1; p++; }
    }

    /* Exponent */
    if (*p == 'e' || *p == 'E') {
        p++;
        int esign = 1;
        if (*p == '+') p++;
        else if (*p == '-') { esign = -1; p++; }
        int ex = 0;
        while (is_digit(*p)) { ex = ex * 10 + (*p - '0'); p++; }
        /* result *= 10^(esign*ex) via repeated multiply */
        double mul = (esign > 0) ? 10.0 : 0.1;
        int n = ex;
        while (n--) result *= mul;
    }

    if (p == start) {
        /* no digits consumed */
        _lua_errno = 22; /* EINVAL */
        if (endptr) *endptr = (char *)s;
        return 0.0;
    }

    if (endptr) *endptr = (char *)p;
    return result * (double)sign;
}

/* strtold / strtof redirect to strtod */
long double strtold(const char *s, char **endptr)
{
    /* Not called when LUA_C89_NUMBERS=1. Return 0 via union to avoid
     * __extenddftf2 (AArch64 long double is 128-bit quad precision). */
    union { long double ld; unsigned long long u[2]; } zero;
    zero.u[0] = 0; zero.u[1] = 0;
    (void)s;
    if (endptr) *endptr = (char *)s;
    return zero.ld;
}

float strtof(const char *s, char **endptr)
{
    return (float)strtod(s, endptr);
}

/* ── strtol / strtoul ────────────────────────────────────────────────────── */
unsigned long strtoul(const char *s, char **endptr, int base)
{
    const char *p = s;
    while (is_space(*p)) p++;
    unsigned long result = 0;
    int sign = 1;

    if (*p == '+') p++;
    else if (*p == '-') { sign = -1; p++; }

    if ((base == 0 || base == 16) && p[0] == '0' &&
        (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (base == 0 && p[0] == '0') {
        base = 8;
        p++;
    } else if (base == 0) {
        base = 10;
    }

    const char *start = p;
    while (*p) {
        int d;
        if (is_digit(*p))        d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else break;
        if (d >= base) break;
        result = result * (unsigned)base + (unsigned)d;
        p++;
    }

    (void)start;
    if (endptr) *endptr = (char *)p;
    return (sign < 0) ? (unsigned long)(-(long)result) : result;
}

long strtol(const char *s, char **endptr, int base)
{
    return (long)strtoul(s, endptr, base);
}
