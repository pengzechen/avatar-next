/*
 * io/select.c - select / pselect6 处理器
 *
 * fd_set 是 bitmap，每位 1 个 fd。stdin(fd=0) 仅在 UART 非空时可读，
 * 其他 fd 始终可读/可写。支持阻塞、超时与信号中断。
 */
#include "syscall/syscall_internal.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/fs/pipe.h"
#include "syscall/net/ksocket.h"
#include "syscall/fs/pty.h"
#include "task/task.h"
#include "task/sched.h"

void
select_handler(uint64_t regs[6], uint64_t syscall_num, task_t *current)
{
    int            nfds      = (int)regs[0];
    unsigned long *rfds      = (unsigned long *)regs[1];
    unsigned long *wfds      = (unsigned long *)regs[2];
    unsigned long *efds      = (unsigned long *)regs[3];
    int64_t        timeout_ms;
    if (syscall_num == X86_SYS_SELECT) {
        /* select 用 struct timeval { long tv_sec; long tv_usec; } */
        struct { int64_t tv_sec; int64_t tv_usec; } *tv = (void *)regs[4];
        if (!tv)          timeout_ms = -1;
        else if (tv->tv_sec == 0 && tv->tv_usec == 0) timeout_ms = 0;
        else              timeout_ms = tv->tv_sec * 1000 + tv->tv_usec / 1000;
    } else {
        struct kernel_timespec *ts = (struct kernel_timespec *)regs[4];
        if (!ts)          timeout_ms = -1;
        else if (ts->tv_sec == 0 && ts->tv_nsec == 0) timeout_ms = 0;
        else              timeout_ms = ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
    }
    if (nfds < 0) nfds = 0;
    if (nfds > 1024) nfds = 1024;
    int nwords = (nfds + 63) / 64;

    /* 备份原始 fd_set，循环重复使用 */
    unsigned long r_in[16] = {0}, w_in[16] = {0}, e_in[16] = {0};
    if (rfds) for (int i = 0; i < nwords; i++) r_in[i] = rfds[i];
    if (wfds) for (int i = 0; i < nwords; i++) w_in[i] = wfds[i];
    if (efds) for (int i = 0; i < nwords; i++) e_in[i] = efds[i];

    uint64_t deadline_ns = 0;
    if (timeout_ms > 0)
        deadline_ns = kernel_get_ns() + (uint64_t)timeout_ms * 1000000ULL;

    int ready = 0;
    for (;;) {
        signal_check_uart();
        if (current && (current->pending_sigs & ~current->blocked_sigs)) {
            regs[0] = (uint64_t)(int64_t)-EINTR;
            return;
        }
        ready = 0;
        int has_stdin = !uart_ringbuf_empty();
        /* 清零输出位图，再根据 readiness 设置 */
        if (rfds) for (int i = 0; i < nwords; i++) rfds[i] = 0;
        if (wfds) for (int i = 0; i < nwords; i++) wfds[i] = 0;
        if (efds) for (int i = 0; i < nwords; i++) efds[i] = 0;
        for (int fd = 0; fd < nfds; fd++) {
            int w = fd >> 6, b = fd & 63;
            unsigned long mask = 1UL << b;
            if (r_in[w] & mask) {
                int rdy;
                fd_obj_t *obj = task_get_fd(current, fd);
                if (obj && obj->type == FDT_PIPE)
                    rdy = pipe_poll_readable(current->fd_table[fd]);
                else if (obj && obj->type == FDT_SOCKET)
                    rdy = ksock_poll_readable(obj->sock.sock_idx);
                else if (obj && obj->type == FDT_PTY)
                    rdy = obj->pty.is_master ?
                        pty_poll_readable_master(obj->pty.pty_idx) :
                        pty_poll_readable_slave(obj->pty.pty_idx);
                else if (fd == 0)
                    rdy = has_stdin;
                else
                    rdy = 1;
                if (rdy) { if (rfds) rfds[w] |= mask; ready++; }
            }
            if (w_in[w] & mask) {
                int wrdy;
                fd_obj_t *obj = task_get_fd(current, fd);
                if (obj && obj->type == FDT_PIPE)
                    wrdy = pipe_poll_writable(current->fd_table[fd]);
                else if (obj && obj->type == FDT_SOCKET)
                    wrdy = ksock_poll_writable(obj->sock.sock_idx);
                else
                    wrdy = 1;
                if (wrdy) {
                    if (wfds) wfds[w] |= mask;
                    ready++;
                }
            }
            /* exceptfds: 永不报异常 */
            (void)e_in; (void)efds;
        }
        if (ready > 0 || timeout_ms == 0) { regs[0] = (uint64_t)ready; return; }
        if (timeout_ms > 0 && kernel_get_ns() >= deadline_ns) {
            regs[0] = 0; return;
        }
        task_yield();
    }
}
