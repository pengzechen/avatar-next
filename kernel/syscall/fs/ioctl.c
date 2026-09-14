/*
 * fs/ioctl.c - ioctl 系统调用
 *
 * 由 kernel/syscall/syscall.c 拆出。优先派发给 pseudofs 设备节点；
 * 否则按 TTY 终端 ioctl 处理（TCGETS / TIOCGWINSZ / TIOCGPGRP 等）。
 */
#include "syscall/syscall_internal.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/fs/tty.h"
#include "task/task.h"
#include "klog.h"
#include "vfs.h"

void ioctl_handler(uint64_t regs[6], task_t *current)
{
    int      ioctl_fd = (int)regs[0];
    uint64_t request  = regs[1];
    void    *argp     = (void *)regs[2];

    /* 查找 fd 对象（任何 fd，包括 0/1/2） */
    fd_obj_t *ioctl_obj = task_get_fd(current, ioctl_fd);

    if (request == FIONBIO) {
        if (!argp) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
        }
        if (!ioctl_obj) {
            regs[0] = (uint64_t)(int64_t)-EBADF;
            return;
        }
        if (*(int *)argp)
            ioctl_obj->flags |= 04000;  /* O_NONBLOCK */
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
        if (rc >= 0) { regs[0] = 0; return; }
        if (rc != -38 /* ENOSYS */) {
            regs[0] = (uint64_t)(int64_t)rc;
            return;
        }
    }

    /* 非默认 UART fd 不支持全局终端 ioctl fallback。 */
    if (ioctl_obj) {
        regs[0] = (uint64_t)(int64_t)-ENOTTY;
        return;
    }

    /* fd 0-2 默认 UART 或无 pool entry：全局 TTY */

    if (request == TCGETS && argp) {
        *(struct kernel_termios *)argp = g_termios;
        regs[0] = 0;
    } else if (request == TIOCGWINSZ && argp) {
        struct kernel_winsize *ws = (struct kernel_winsize *)argp;
        ws->ws_row    = 24;
        ws->ws_col    = 80;
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        regs[0] = 0;
    } else if ((request == TCSETS || request == TCSETSW || request == TCSETSF) && argp) {
        g_termios = *(struct kernel_termios *)argp;
        regs[0] = 0;
    } else if (request == TIOCGPGRP && argp) {
        *(int *)argp = (int)(g_fg_pgid ? g_fg_pgid : current->pgid);
        regs[0] = 0;
    } else if (request == TIOCSPGRP && argp) {
        g_fg_pgid = (uint32_t)*(int *)argp;
        regs[0] = 0;
    } else if (request == TIOCSWINSZ) {
        regs[0] = 0;
    } else if (request == TIOCSCTTY || request == TIOCNOTTY) {
        regs[0] = 0;
    } else {
        KLOG_DEBUG("[ioctl] unsupported fd=%d req=0x%llx\n",
                   ioctl_fd, (unsigned long long)request);
        regs[0] = (uint64_t)(int64_t)-ENOTTY;
    }
}
