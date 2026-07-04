/*
 * io/poll.c - poll / ppoll 处理器
 *
 * 使用 fd_poll() 统一接口查询 fd 就绪状态。
 * 阻塞模式下使用 task_yield() 让出 CPU。
 */
#include "syscall/syscall_internal.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/io/epoll.h"
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
        if (pfds && nfds > 0) {
            for (uint64_t pi = 0; pi < nfds; pi++) {
                pfds[pi].revents = 0;
                short ev = pfds[pi].events;
                if (pfds[pi].fd < 0) continue;

                uint32_t poll_ev = fd_poll(current, pfds[pi].fd);
                if ((ev & 0x01) && (poll_ev & EPOLLIN))
                    pfds[pi].revents |= 0x01;
                if ((ev & 0x04) && (poll_ev & EPOLLOUT))
                    pfds[pi].revents |= 0x04;
                if (poll_ev & EPOLLERR)
                    pfds[pi].revents |= 0x08;
                if (poll_ev & EPOLLHUP)
                    pfds[pi].revents |= 0x10;

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
