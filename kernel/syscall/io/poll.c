/*
 * io/poll.c - poll / ppoll 处理器
 *
 * 语义：fd==0 读就绪仅当 UART ring buffer 非空；
 *       fd>=3 一律报告可读可写（管道/文件不阻塞）。
 *       负 timeout = 阻塞等待；timeout==0 = 立即返回。
 */
#include "syscall/syscall_internal.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/fs/pipe.h"
#include "syscall/net/ksocket.h"
#include "syscall/fs/pty.h"
#include "task/task.h"
#include "task/sched.h"

void
poll_handler(uint64_t regs[6], uint64_t syscall_num, task_t *current)
{
    struct kernel_pollfd *pfds = (struct kernel_pollfd *)regs[0];
    uint64_t nfds = regs[1];
    int64_t  timeout_ms;
    if (syscall_num == X86_SYS_POLL) {
        timeout_ms = (int64_t)regs[2];
    } else {
        struct kernel_timespec *ts = (struct kernel_timespec *)regs[2];
        if (!ts)            timeout_ms = -1;
        else if (ts->tv_sec == 0 && ts->tv_nsec == 0) timeout_ms = 0;
        else                timeout_ms = ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
    }

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
        if (pfds && nfds > 0) {
            for (uint64_t pi = 0; pi < nfds; pi++) {
                pfds[pi].revents = 0;
                short ev = pfds[pi].events;
                if (pfds[pi].fd < 0) continue;

                fd_obj_t *obj = task_get_fd(current, pfds[pi].fd);
                if (obj && obj->type == FDT_PIPE) {
                    int pool_idx = current->fd_table[pfds[pi].fd];
                    if ((ev & 0x01) && pipe_poll_readable(pool_idx))
                        pfds[pi].revents |= 0x01;
                    if ((ev & 0x04) && pipe_poll_writable(pool_idx))
                        pfds[pi].revents |= 0x04;
                } else if (obj && obj->type == FDT_SOCKET) {
                    if ((ev & 0x01) && ksock_poll_readable(obj->sock.sock_idx))
                        pfds[pi].revents |= 0x01;
                    if ((ev & 0x04) && ksock_poll_writable(obj->sock.sock_idx))
                        pfds[pi].revents |= 0x04;
                } else if (obj && obj->type == FDT_PTY) {
                    if (ev & 0x01) {
                        bool rd = obj->pty.is_master ?
                            pty_poll_readable_master(obj->pty.pty_idx) :
                            pty_poll_readable_slave(obj->pty.pty_idx);
                        if (rd) pfds[pi].revents |= 0x01;
                    }
                    if ((ev & 0x04) && pty_poll_writable(obj->pty.pty_idx))
                        pfds[pi].revents |= 0x04;
                } else if (pfds[pi].fd == 0) {
                    if (has_stdin) pfds[pi].revents = ev & 0x01;
                } else if (obj) {
                    pfds[pi].revents = ev & 0x05; /* POLLIN|POLLOUT */
                }
                if (pfds[pi].revents) ready++;
            }
        }
        if (ready > 0 || timeout_ms == 0) { regs[0] = (uint64_t)ready; return; }
        if (timeout_ms > 0 && kernel_get_ns() >= deadline_ns) {
            regs[0] = 0; return;
        }
        task_yield();
    }
}
