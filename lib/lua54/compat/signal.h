/* lib/lua54/compat/signal.h — minimal signal stubs for Lua */
#ifndef _COMPAT_SIGNAL_H
#define _COMPAT_SIGNAL_H

typedef int sig_atomic_t;   /* async-signal-safe integer */

#define SIGABRT   6
#define SIGFPE    8
#define SIGILL    4
#define SIGINT    2
#define SIGSEGV   11
#define SIGTERM   15

typedef void (*sighandler_t)(int);

#define SIG_DFL  ((sighandler_t)0)
#define SIG_IGN  ((sighandler_t)1)
#define SIG_ERR  ((sighandler_t)-1)

static inline sighandler_t signal(int sig, sighandler_t handler)
{
    (void)sig; (void)handler;
    return SIG_DFL;
}

static inline int raise(int sig)
{
    (void)sig;
    return 0;
}

#endif /* _COMPAT_SIGNAL_H */
