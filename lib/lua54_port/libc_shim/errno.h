/* lib/lua54_port/libc_shim/errno.h — minimal errno for Lua error paths */
#ifndef _COMPAT_ERRNO_H
#define _COMPAT_ERRNO_H

/* Single per-CPU errno — Lua is single-threaded in our kernel */
extern int _lua_errno;
#define errno _lua_errno

/* POSIX errno codes used by Lua / lauxlib.c */
#define EPERM    1
#define ENOENT   2
#define EIO      5
#define ENOEXEC  8
#define EBADF    9
#define ENOMEM   12
#define EACCES   13
#define EFAULT   14
#define EBUSY    16
#define EEXIST   17
#define ENODEV   19
#define ENOTDIR  20
#define EINVAL   22
#define ENOSPC   28
#define ERANGE   34

#endif /* _COMPAT_ERRNO_H */
