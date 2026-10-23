/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   fs/ioctl.c - ioctl 系统调用

   由 kernel/syscall/syscall.c 拆出。优先派发给 pseudofs 设备节点；
   否则按 TTY 终端 ioctl 处理(TCGETS / TIOCGWINSZ / TIOCGPGRP 等)。
*/
#include "syscall/syscall_internal.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/fs/tty.h"
#include "task/task.h"
#include "klog.h"
#include "vfs.h"
#include "dma_buf.h"

static bool trace_heavy_task(task_t *current)
{
    return current && current->is_user_process && current->heap_end >= 0x3000000ULL;
}

void ioctl_handler(uint64_t regs[6], task_t *current)
{
    int ioctl_fd = (int)regs[0];
    /*
       注意： 必须截成 32 位。Linux 的 `SYSCALL_DEFINE3(ioctl, ..., unsigned int,
          cmd, ...)` 就是 32 位, 这里照做。

       不截会怎样：musl 的声明是 `int ioctl(int, int, ...)`(glibc 是
       `unsigned long`), 于是 `0xc0406400` 这种最高位为 1 的号在用户态被当成
       负数, 符号扩展到 `0xffffffffc0406400` 才进内核。内核拿它去和 32 位
       的 _IOC 常量比对, 永远不等 —— 症状是"所有带 'd'/'T' 这类高位 magic 的
       ioctl 全部走进 default 分支", 而 glibc 编的程序完全正常。
       实测：drmtest(musl)报 `未实现的 DRM ioctl req=0xffffffffc0406400`。

       下面那个 `(uint32_t)req` 的强转在 pseudo_ioctl 里本来就有 —— 说明这个
       坑以前被踩过, 只是当时只补了一条路径。
    */
    uint32_t request = (uint32_t)regs[1];
    void *argp = (void *)regs[2];

    /* 查找 fd 对象(任何 fd, 包括 0/1/2) */
    fd_obj_t *ioctl_obj = task_get_fd(current, ioctl_fd);
    if (trace_heavy_task(current)) {
        KLOG_SYSCALL("[vfsioctl] pid=%u fd=%d req=0x%llx arg=0x%llx path=%s kind=%d\n", current->id, ioctl_fd, request,
                     (uint64_t)argp, (ioctl_obj && fd_obj_file(ioctl_obj)) ? fd_obj_file(ioctl_obj)->path : "<none>",
                     (ioctl_obj && fd_obj_file(ioctl_obj)) ? (int)fd_obj_file(ioctl_obj)->kind : -1);
    }

    if (request == FIONBIO) {
        int on;
        if (!argp || copy_from_user_bytes(argp, &on, sizeof(on)) < 0) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
        }
        if (!ioctl_obj) {
            regs[0] = (uint64_t)(int64_t)-EBADF;
            return;
        }
        if (on)
            ioctl_obj->flags |= 04000; /* O_NONBLOCK */
        else
            ioctl_obj->flags &= ~04000;
        if (fd_obj_file(ioctl_obj))
            fd_obj_file(ioctl_obj)->flags = ioctl_obj->flags;
        regs[0] = 0;
        return;
    }

    /* VFS-backed fd: devices handle their own ioctl, regular files return ENOSYS. */
    if (ioctl_obj && fd_obj_file(ioctl_obj)) {
        int rc = vfs_ioctl(fd_obj_file(ioctl_obj), request, argp);
        if (rc >= 0) {
            regs[0] = 0;
            return;
        }
        if (rc != -38 /* ENOSYS */) {
            regs[0] = (uint64_t)(int64_t)rc;
            return;
        }
    }

    /*
       dma-buf fd 没有 vfs_file —— 它不是一个文件, 是一个缓冲区句柄。
       DMA_BUF_IOCTL_SYNC 打在 fd 自己身上(librknnrt 实测)；参数里的用户
       指针由 dev.c 的 dma_buf_fd_ioctl 统一走 copy_from_user。
    */
    if (ioctl_obj && ioctl_obj->type == FDT_DMABUF) {
        int rc = dma_buf_fd_ioctl(ioctl_obj->dmabuf.idx, request, argp);
        regs[0] = (rc < 0) ? (uint64_t)(int64_t)rc : 0;
        return;
    }

    /* 非默认 UART fd 不支持全局终端 ioctl fallback。 */
    if (ioctl_obj && fd_obj_file(ioctl_obj)) {
        regs[0] = (uint64_t)(int64_t)-ENOTTY;
        return;
    }

    /*
       fd 0-2 默认 UART 或无 pool entry：全局 TTY

       下面全部经 copy_to_user_bytes / copy_from_user_bytes 访问用户指针。
       以前是裸解引用(如 `*(struct kernel_termios *)argp = g_termios;`),
       用户传非法地址(例如 (void*)-1)就会让内核态写坏地址 → #PF →
       异常处理的死循环 → 整机挂死。copy_*_bytes 内部用 user_range_ok()
       校验, 非法指针返回 -1, 这里转成 -EFAULT。
    */
    if (request == TCGETS && argp) {
        if (copy_to_user_bytes(&g_termios, argp, sizeof(g_termios)) < 0)
            regs[0] = (uint64_t)(int64_t)-EFAULT;
        else
            regs[0] = 0;
    } else if (request == TIOCGWINSZ && argp) {
        struct kernel_winsize ws = {
            .ws_row = 24,
            .ws_col = 80,
            .ws_xpixel = 0,
            .ws_ypixel = 0,
        };
        if (copy_to_user_bytes(&ws, argp, sizeof(ws)) < 0)
            regs[0] = (uint64_t)(int64_t)-EFAULT;
        else
            regs[0] = 0;
    } else if ((request == TCSETS || request == TCSETSW || request == TCSETSF) && argp) {
        struct kernel_termios t;
        if (copy_from_user_bytes(argp, &t, sizeof(t)) < 0)
            regs[0] = (uint64_t)(int64_t)-EFAULT;
        else {
            g_termios = t;
            regs[0] = 0;
        }
    } else if (request == TIOCGPGRP && argp) {
        int pgrp = (int)(g_fg_pgid ? g_fg_pgid : current->pgid);
        if (copy_to_user_bytes(&pgrp, argp, sizeof(pgrp)) < 0)
            regs[0] = (uint64_t)(int64_t)-EFAULT;
        else
            regs[0] = 0;
    } else if (request == TIOCSPGRP && argp) {
        int pgrp;
        if (copy_from_user_bytes(argp, &pgrp, sizeof(pgrp)) < 0)
            regs[0] = (uint64_t)(int64_t)-EFAULT;
        else {
            g_fg_pgid = (uint32_t)pgrp;
            regs[0] = 0;
        }
    } else if (request == TIOCSWINSZ) {
        regs[0] = 0;
    } else if (request == TIOCSCTTY || request == TIOCNOTTY) {
        regs[0] = 0;
    } else {
        KLOG_SYSCALL("[ioctl] unsupported fd=%d req=0x%llx\n", ioctl_fd, (unsigned long long)request);
        regs[0] = (uint64_t)(int64_t)-ENOTTY;
    }
}
