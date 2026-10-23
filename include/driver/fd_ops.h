/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   include/driver/fd_ops.h — 可选服务的 fd 操作表

   对应 Linux 的 `struct file_operations`：fd 从哪来、怎么 mmap/ioctl/close
   由创建它的驱动决定，syscall 核心只认这张表。

   priv 是驱动自己的私有数据（ION 是 handle，dma-buf 是表下标），
   由 task_alloc_*_fd() 创建 fd 时写入 fd_obj。
*/

#ifndef DRIVER_FD_OPS_H
#define DRIVER_FD_OPS_H

#include "types.h"

typedef struct fd_ops {
    const char *name; /* 日志用，例如 "ion" / "dmabuf" */

    /*
       取设备可见的物理地址和缓冲区大小，供 sys_mmap 做通用连续页映射。
       返回 0 成功；-1 表示 fd 无效。
    */
    int (*get_phys)(void *priv, uint64_t *paddr, uint64_t *size);

    /* dup/继承 fd 时增加底层引用；返回 0 成功 */
    int (*dup)(void *priv);

    /* fd 关闭时释放底层引用 */
    void (*close)(void *priv);
} fd_ops_t;

#endif /* DRIVER_FD_OPS_H */
