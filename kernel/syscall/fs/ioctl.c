/*
 * fs/ioctl.c - ioctl 系统调用
 *
 * 由 kernel/syscall/syscall.c 拆出。优先派发给 pseudofs 设备节点；
 * 否则按 TTY 终端 ioctl 处理（TCGETS / TIOCGWINSZ / TIOCGPGRP 等）。
 */
#include "syscall/syscall_internal.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/fs/tty.h"
#include "syscall/fs/pty.h"
#include "task/task.h"
#include "pseudofs.h"
#include "klog.h"

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
        regs[0] = 0;
        return;
    }

    /* PTY 优先派发（fd 0/1/2 也可能是 PTY slave） */
    if (ioctl_obj && ioctl_obj->type == FDT_PTY) {
        int rc = pty_ioctl(ioctl_obj->pty.pty_idx, ioctl_obj->pty.is_master,
                           (uint32_t)request, argp);
        regs[0] = rc < 0 ? (uint64_t)(int64_t)rc : 0;
        return;
    }

    /* pseudofs 设备节点 */
    if (ioctl_obj && ioctl_obj->type == FDT_PSEUDO) {
        int rc = pseudo_ioctl(ioctl_obj->pseudo.node_id, request, argp);
        KLOG_DEBUG("[ioctl] pseudo fd=%d node=%d req=0x%llx rc=%d\n",
                   ioctl_fd, ioctl_obj->pseudo.node_id,
                   (unsigned long long)request, rc);
        if (rc >= 0) { regs[0] = 0; return; }
        if (rc != -38 /* ENOSYS */) {
            regs[0] = (uint64_t)(int64_t)rc;
            return;
        }
    }

    /* 非 TTY 类型（socket/pipe/file/dir）不支持终端 ioctl */
    if (ioctl_obj && ioctl_obj->type != FDT_PSEUDO) {
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
