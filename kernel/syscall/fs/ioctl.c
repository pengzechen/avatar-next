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
#include "pseudofs.h"

void ioctl_handler(uint64_t regs[6], task_t *current)
{
    int      ioctl_fd = (int)regs[0];
    uint64_t request  = regs[1];
    void    *argp     = (void *)regs[2];

    /* pseudofs 设备节点优先 */
    if (ioctl_fd >= 3) {
        fd_obj_t *ioctl_obj = task_get_fd(current, ioctl_fd);
        if (ioctl_obj && ioctl_obj->type == FDT_PSEUDO) {
            int rc = pseudo_ioctl(ioctl_obj->pseudo.node_id, request, argp);
            if (rc >= 0) { regs[0] = 0; return; }
            if (rc != -38 /* ENOSYS */) {
                regs[0] = (uint64_t)(int64_t)rc;
                return;
            }
        }
    }

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
    } else {
        regs[0] = (uint64_t)(int64_t)-ENOTTY;
    }
}
