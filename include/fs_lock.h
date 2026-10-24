/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/fs_lock.h - 全局文件系统串行化锁

   lwext4 的全局缓冲缓存没有内部同步；FatFs 也不是线程安全的，而且两套文件系统
   共用同一个块设备接口（bdif）。所以任何 lwext4 / FatFs API 调用都要被同一把
   可重入锁串起来。

   lwext4 的“每次 API 调用”这一层由 fs/lwext4_port/fs_lock.c 的 --wrap 自动加上；
   下面两个函数给**跨多次调用**的临界区用（例如 f_stat + f_open、f_lseek +
   f_truncate、整个 f_readdir 循环），FatFs 胶水也直接用它们。

   锁可重入（按 CPU 记 owner + 深度），持锁期间关本核中断。不要在锁内打 KLOG
   （UART 锁与 FS 锁的获取顺序会引入跨核死锁风险）。
*/
#ifndef FS_LOCK_H
#define FS_LOCK_H

void fs_global_lock(void);
void fs_global_unlock(void);

#endif /* FS_LOCK_H */
