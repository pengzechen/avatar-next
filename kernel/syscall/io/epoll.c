/*
 * io/epoll.c — epoll 子系统实现
 *
 * 事件驱动 I/O 多路复用。核心机制：
 *   1. epoll_ctl(ADD) 时在目标 fd 的 wq 上注册 waiter
 *   2. fd 状态变化时（pipe write、socket recv 等）调用 fd_notify_waiters()
 *   3. fd_notify_waiters() 将对应 epoll_item 标记为 ready 并唤醒 blocked task
 *   4. epoll_wait 收集就绪项返回用户空间
 */
#include "syscall/io/epoll.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/fs/pipe.h"
#include "syscall/net/ksocket.h"
#include "syscall/fs/pty.h"
#include "syscall/syscall_internal.h"
#include "task/task.h"
#include "task/sched.h"
#include "klog.h"
#include "string.h"

/* ── epoll 实例池 ───────────────────────────────────────────────── */

static epoll_instance_t g_epoll_pool[EPOLL_MAX_INSTANCES];

static int epoll_instance_alloc(void)
{
    for (int i = 0; i < EPOLL_MAX_INSTANCES; i++) {
        if (!g_epoll_pool[i].in_use) {
            memset(&g_epoll_pool[i], 0, sizeof(epoll_instance_t));
            g_epoll_pool[i].in_use = true;
            return i;
        }
    }
    return -1;
}

static void epoll_instance_free(int idx)
{
    if (idx >= 0 && idx < EPOLL_MAX_INSTANCES)
        g_epoll_pool[idx].in_use = false;
}

static epoll_instance_t *epoll_get(int idx)
{
    if (idx < 0 || idx >= EPOLL_MAX_INSTANCES)
        return NULL;
    if (!g_epoll_pool[idx].in_use)
        return NULL;
    return &g_epoll_pool[idx];
}

/* ── fd_waitqueue 操作 ──────────────────────────────────────────── */

static int wq_register(fd_waitqueue_t *wq, epoll_instance_t *ep, int item_idx)
{
    if (wq->waiter_count >= FD_WAIT_MAX)
        return -1;
    fd_waiter_t *w = &wq->waiters[wq->waiter_count++];
    w->ep = ep;
    w->item_idx = item_idx;
    return 0;
}

static void wq_unregister(fd_waitqueue_t *wq, epoll_instance_t *ep, int item_idx)
{
    for (int i = 0; i < wq->waiter_count; i++) {
        if (wq->waiters[i].ep == ep && wq->waiters[i].item_idx == item_idx) {
            wq->waiters[i] = wq->waiters[--wq->waiter_count];
            return;
        }
    }
}

/* ── fd_notify_waiters — 事件通知核心 ───────────────────────────── */

void fd_notify_waiters(int pool_idx, uint32_t events)
{
    if (pool_idx < 0 || pool_idx >= FD_POOL_SIZE)
        return;
    fd_obj_t *obj = &g_fd_pool[pool_idx];
    fd_waitqueue_t *wq = &obj->wq;

    for (int i = 0; i < wq->waiter_count; i++) {
        fd_waiter_t *w = &wq->waiters[i];
        epoll_instance_t *ep = w->ep;
        if (!ep || !ep->in_use)
            continue;
        int idx = w->item_idx;
        if (idx < 0 || idx >= ep->item_count)
            continue;
        epoll_item_t *item = &ep->items[idx];
        if (item->pool_idx != pool_idx)
            continue;
        if (!(item->events & events) && !(events & (EPOLLERR | EPOLLHUP)))
            continue;
        if (!item->ready) {
            item->ready = true;
            ep->ready_count++;
        }
        if (ep->blocked_task && ep->blocked_task->state == TASK_BLOCKED) {
            task_unblock(ep->blocked_task);
            ep->blocked_task = NULL;
        }
    }
}

/* ── fd_close_notify — fd 关闭时从所有 epoll 注销 ───────────────── */

void fd_close_notify(int pool_idx)
{
    if (pool_idx < 0 || pool_idx >= FD_POOL_SIZE)
        return;
    fd_obj_t *obj = &g_fd_pool[pool_idx];
    fd_waitqueue_t *wq = &obj->wq;

    while (wq->waiter_count > 0) {
        fd_waiter_t *w = &wq->waiters[0];
        epoll_instance_t *ep = w->ep;
        int item_idx = w->item_idx;

        wq->waiters[0] = wq->waiters[--wq->waiter_count];

        if (ep && ep->in_use && item_idx >= 0 && item_idx < ep->item_count) {
            epoll_item_t *item = &ep->items[item_idx];
            if (item->pool_idx == pool_idx) {
                if (item->ready)
                    ep->ready_count--;

                int last = ep->item_count - 1;
                if (item_idx < last) {
                    fd_obj_t *last_obj = &g_fd_pool[ep->items[last].pool_idx];
                    wq_unregister(&last_obj->wq, ep, last);
                    ep->items[item_idx] = ep->items[last];
                    if (last_obj->wq.waiter_count < FD_WAIT_MAX)
                        wq_register(&last_obj->wq, ep, item_idx);
                }
                ep->item_count--;
            }
        }
    }
}

/* ── fd_poll — 统一 poll 接口 ───────────────────────────────────── */

uint32_t fd_poll(task_t *task, int fd)
{
    uint32_t revents = 0;

    if (fd < 0 || fd >= (int)TASK_MAX_FD)
        return 0;

    if (fd == 0) {
        if (!uart_ringbuf_empty())
            revents |= EPOLLIN;
        revents |= EPOLLOUT;
        return revents;
    }
    if (fd == 1 || fd == 2)
        return EPOLLOUT;

    fd_obj_t *obj = task_get_fd(task, fd);
    if (!obj)
        return 0;

    int pool_idx = task->fd_table[fd];

    switch (obj->type) {
    case FDT_PIPE:
        if (!obj->pipe.is_write_end) {
            if (pipe_poll_readable(pool_idx))
                revents |= EPOLLIN;
        }
        if (obj->pipe.is_write_end || pipe_poll_writable(pool_idx))
            revents |= EPOLLOUT;
        if (!obj->pipe.is_write_end && pipe_poll_writable(pool_idx))
            ; /* read end doesn't report EPOLLOUT */
        /* re-evaluate: read end reports readable, write end reports writable */
        revents = 0;
        if (obj->pipe.is_write_end) {
            if (pipe_poll_writable(pool_idx))
                revents |= EPOLLOUT;
        } else {
            if (pipe_poll_readable(pool_idx))
                revents |= EPOLLIN;
        }
        break;

    case FDT_SOCKET:
        if (ksock_poll_readable(obj->sock.sock_idx))
            revents |= EPOLLIN;
        if (ksock_poll_writable(obj->sock.sock_idx))
            revents |= EPOLLOUT;
        break;

    case FDT_PTY:
        if (obj->pty.is_master) {
            if (pty_poll_readable_master(obj->pty.pty_idx))
                revents |= EPOLLIN;
        } else {
            if (pty_poll_readable_slave(obj->pty.pty_idx))
                revents |= EPOLLIN;
        }
        if (pty_poll_writable(obj->pty.pty_idx))
            revents |= EPOLLOUT;
        break;

    case FDT_EPOLL:
        break;

    default:
        revents = EPOLLIN | EPOLLOUT;
        break;
    }

    return revents;
}

/* ── epoll_create1 handler ──────────────────────────────────────── */

void epoll_create1_handler(uint64_t regs[6], task_t *current)
{
    uint32_t flags = (uint32_t)regs[0];
    if (flags & ~(uint32_t)EPOLL_CLOEXEC) {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }

    int ep_idx = epoll_instance_alloc();
    if (ep_idx < 0) {
        regs[0] = (uint64_t)(int64_t)-ENFILE;
        return;
    }

    int pool = fd_pool_alloc();
    if (pool < 0) {
        epoll_instance_free(ep_idx);
        regs[0] = (uint64_t)(int64_t)-ENFILE;
        return;
    }

    fd_obj_t *obj = &g_fd_pool[pool];
    obj->type = FDT_EPOLL;
    obj->flags = 0;
    obj->epoll.ep_idx = (int16_t)ep_idx;
    obj->path[0] = '\0';
    memset(&obj->wq, 0, sizeof(fd_waitqueue_t));

    int fd = task_alloc_fd(current, pool);
    if (fd < 0) {
        fd_pool_free(pool);
        epoll_instance_free(ep_idx);
        regs[0] = (uint64_t)(int64_t)-EMFILE;
        return;
    }

    epoll_instance_t *ep = &g_epoll_pool[ep_idx];
    ep->owner_pid = current->id;

    if (flags & EPOLL_CLOEXEC)
        current->fd_cloexec[fd / 8] |= (1 << (fd % 8));

    KLOG_DEBUG("[epoll] create1: pid=%u fd=%d ep_idx=%d\n", current->id, fd, ep_idx);
    regs[0] = (uint64_t)fd;
}

/* ── epoll_ctl handler ──────────────────────────────────────────── */

void epoll_ctl_handler(uint64_t regs[6], task_t *current)
{
    int epfd = (int)regs[0];
    int op   = (int)regs[1];
    int fd   = (int)regs[2];
    struct epoll_event *ev = (struct epoll_event *)regs[3];

    fd_obj_t *ep_obj = task_get_fd(current, epfd);
    if (!ep_obj || ep_obj->type != FDT_EPOLL) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }
    epoll_instance_t *ep = epoll_get(ep_obj->epoll.ep_idx);
    if (!ep) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }

    if (fd == epfd) {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }

    fd_obj_t *target = task_get_fd(current, fd);
    if (!target) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }
    int target_pool = current->fd_table[fd];

    switch (op) {
    case EPOLL_CTL_ADD: {
        for (int i = 0; i < ep->item_count; i++) {
            if (ep->items[i].fd == fd) {
                regs[0] = (uint64_t)(int64_t)-EINVAL;
                return;
            }
        }
        if (ep->item_count >= EPOLL_MAX_ITEMS) {
            regs[0] = (uint64_t)(int64_t)-ENOMEM;
            return;
        }
        if (!ev) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
        }

        int idx = ep->item_count;
        epoll_item_t *item = &ep->items[idx];
        item->fd       = fd;
        item->pool_idx = target_pool;
        item->events   = ev->events;
        item->data     = ev->data;
        item->ready    = false;
        ep->item_count++;

        if (wq_register(&target->wq, ep, idx) < 0) {
            ep->item_count--;
            regs[0] = (uint64_t)(int64_t)-ENOMEM;
            return;
        }

        uint32_t cur_events = fd_poll(current, fd);
        if (cur_events & item->events) {
            item->ready = true;
            ep->ready_count++;
            if (ep->blocked_task && ep->blocked_task->state == TASK_BLOCKED) {
                task_unblock(ep->blocked_task);
                ep->blocked_task = NULL;
            }
        }

        KLOG_DEBUG("[epoll] ctl ADD: ep=%d fd=%d events=0x%x\n",
                   ep_obj->epoll.ep_idx, fd, ev->events);
        regs[0] = 0;
        break;
    }

    case EPOLL_CTL_DEL: {
        int found = -1;
        for (int i = 0; i < ep->item_count; i++) {
            if (ep->items[i].fd == fd) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            regs[0] = (uint64_t)(int64_t)-ENOENT;
            return;
        }

        wq_unregister(&target->wq, ep, found);

        if (ep->items[found].ready)
            ep->ready_count--;

        int last = ep->item_count - 1;
        if (found < last) {
            fd_obj_t *last_obj = &g_fd_pool[ep->items[last].pool_idx];
            wq_unregister(&last_obj->wq, ep, last);
            ep->items[found] = ep->items[last];
            wq_register(&last_obj->wq, ep, found);
        }
        ep->item_count--;

        KLOG_DEBUG("[epoll] ctl DEL: ep=%d fd=%d\n", ep_obj->epoll.ep_idx, fd);
        regs[0] = 0;
        break;
    }

    case EPOLL_CTL_MOD: {
        int found = -1;
        for (int i = 0; i < ep->item_count; i++) {
            if (ep->items[i].fd == fd) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            regs[0] = (uint64_t)(int64_t)-ENOENT;
            return;
        }
        if (!ev) {
            regs[0] = (uint64_t)(int64_t)-EFAULT;
            return;
        }

        ep->items[found].events = ev->events;
        ep->items[found].data   = ev->data;

        bool was_ready = ep->items[found].ready;
        uint32_t cur_events = fd_poll(current, fd);
        bool now_ready = (cur_events & ev->events) != 0;

        if (!was_ready && now_ready) {
            ep->items[found].ready = true;
            ep->ready_count++;
            if (ep->blocked_task && ep->blocked_task->state == TASK_BLOCKED) {
                task_unblock(ep->blocked_task);
                ep->blocked_task = NULL;
            }
        } else if (was_ready && !now_ready) {
            ep->items[found].ready = false;
            ep->ready_count--;
        }

        regs[0] = 0;
        break;
    }

    default:
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        break;
    }
}

/* ── epoll_pwait handler ────────────────────────────────────────── */

void epoll_pwait_handler(uint64_t regs[6], task_t *current)
{
    int epfd = (int)regs[0];
    struct epoll_event *events = (struct epoll_event *)regs[1];
    int maxevents = (int)regs[2];
    int timeout_ms = (int)(int32_t)regs[3];

    if (maxevents <= 0 || !events) {
        regs[0] = (uint64_t)(int64_t)-EINVAL;
        return;
    }

    fd_obj_t *ep_obj = task_get_fd(current, epfd);
    if (!ep_obj || ep_obj->type != FDT_EPOLL) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }
    epoll_instance_t *ep = epoll_get(ep_obj->epoll.ep_idx);
    if (!ep) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }

    uint64_t deadline_ns = 0;
    if (timeout_ms > 0)
        deadline_ns = kernel_get_ns() + (uint64_t)timeout_ms * 1000000ULL;

    for (;;) {
        signal_check_uart();
        if (current->pending_sigs & ~current->blocked_sigs) {
            regs[0] = (uint64_t)(int64_t)-EINTR;
            return;
        }

        /* re-scan: check all items for current readiness */
        ep->ready_count = 0;
        for (int i = 0; i < ep->item_count; i++) {
            uint32_t cur = fd_poll(current, ep->items[i].fd);
            uint32_t matched = cur & ep->items[i].events;
            if (cur & (EPOLLERR | EPOLLHUP))
                matched |= cur & (EPOLLERR | EPOLLHUP);
            ep->items[i].ready = (matched != 0);
            if (ep->items[i].ready)
                ep->ready_count++;
        }

        if (ep->ready_count > 0) {
            int n = 0;
            for (int i = 0; i < ep->item_count && n < maxevents; i++) {
                if (!ep->items[i].ready)
                    continue;
                uint32_t cur = fd_poll(current, ep->items[i].fd);
                uint32_t matched = cur & ep->items[i].events;
                if (cur & (EPOLLERR | EPOLLHUP))
                    matched |= cur & (EPOLLERR | EPOLLHUP);
                events[n].events = matched;
                events[n].data   = ep->items[i].data;
                ep->items[i].ready = false;
                n++;
            }
            ep->ready_count = 0;
            regs[0] = (uint64_t)n;
            return;
        }

        if (timeout_ms == 0) {
            regs[0] = 0;
            return;
        }

        if (timeout_ms > 0 && kernel_get_ns() >= deadline_ns) {
            regs[0] = 0;
            return;
        }

        ep->blocked_task = current;
        task_block(NULL);
        ep->blocked_task = NULL;
    }
}

/* ── epoll 实例销毁（close(epfd) 时调用）────────────────────────── */

void epoll_destroy(int ep_idx)
{
    epoll_instance_t *ep = epoll_get(ep_idx);
    if (!ep)
        return;

    for (int i = 0; i < ep->item_count; i++) {
        int pool_idx = ep->items[i].pool_idx;
        if (pool_idx >= 0 && pool_idx < FD_POOL_SIZE) {
            fd_obj_t *obj = &g_fd_pool[pool_idx];
            if (obj->type != FDT_FREE)
                wq_unregister(&obj->wq, ep, i);
        }
    }

    if (ep->blocked_task && ep->blocked_task->state == TASK_BLOCKED) {
        task_unblock(ep->blocked_task);
        ep->blocked_task = NULL;
    }

    epoll_instance_free(ep_idx);
    KLOG_DEBUG("[epoll] destroy: ep_idx=%d\n", ep_idx);
}
