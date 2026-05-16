/* lib/lua54/compat/time.h — stub time functions for Lua */
#ifndef _COMPAT_TIME_H
#define _COMPAT_TIME_H

typedef long time_t;
typedef long clock_t;

#define CLOCKS_PER_SEC  1000000L

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

static inline time_t time(time_t *t)
{
    if (t) *t = (time_t)0;
    return (time_t)0;
}

static inline clock_t clock(void)
{
    return (clock_t)0;
}

static inline struct tm *localtime(const time_t *t)
{
    (void)t;
    return (struct tm *)0;
}

static inline double difftime(time_t end, time_t start)
{
    return (double)(end - start);
}

#endif /* _COMPAT_TIME_H */
