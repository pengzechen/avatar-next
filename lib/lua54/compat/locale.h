/* lib/lua54/compat/locale.h — stub locale for Lua */
#ifndef _COMPAT_LOCALE_H
#define _COMPAT_LOCALE_H

#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5

struct lconv {
    char *decimal_point;
    char *thousands_sep;
};

static inline char *setlocale(int category, const char *locale)
{
    (void)category; (void)locale;
    return (char *)"C";
}

static inline struct lconv *localeconv(void)
{
    static struct lconv c = { (char *)".", (char *)"" };
    return &c;
}

#endif /* _COMPAT_LOCALE_H */
