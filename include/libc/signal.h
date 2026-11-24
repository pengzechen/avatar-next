/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/libc/signal.h — 第三方 <signal.h> 兼容转发

   真正的信号 ABI 在 include/linux/signal.h（Linux/POSIX 语义）。
   这个文件只为 lwIP PPP 等仍写 #include <signal.h> 的第三方代码保留；
   内核代码请直接 include "linux/signal.h"。
*/

#ifndef AVATAR_LIBC_SIGNAL_SHIM_H
#define AVATAR_LIBC_SIGNAL_SHIM_H

#include "linux/signal.h"

#endif /* AVATAR_LIBC_SIGNAL_SHIM_H */
