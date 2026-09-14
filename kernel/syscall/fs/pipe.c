/*
 * fs/pipe.c - kernel pipe implementation
 *
 * Ring buffer pipe with blocking read/write via task_block/task_unblock.
 * Each pipe_alloc() creates one pipe_t and two fd_obj_t entries
 * (read end + write end).
 */
#include "syscall/fs/pipe.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/io/epoll.h"
#include "task/task.h"
#include "klog.h"
#include "string.h"

static pipe_t g_pipes[PIPE_MAX];

static int pipe_slot_alloc(void)
{
    for (int i = 0; i < PIPE_MAX; i++) {
        if (g_pipes[i].rd_refcount == 0 && g_pipes[i].wr_refcount == 0)
            return i;
    }
    return -1;
}

static pipe_t *pipe_get_by_idx(int pi)
{
    if (pi < 0 || pi >= PIPE_MAX)
        return NULL;
    if (g_pipes[pi].rd_refcount == 0 && g_pipes[pi].wr_refcount == 0)
        return NULL;
    return &g_pipes[pi];
}

int pipe_alloc(task_t *task, int fds_out[2])
{
    int pi = pipe_slot_alloc();
    if (pi < 0)
        return -1;

    int rd_pool = fd_pool_alloc();
    if (rd_pool < 0)
        return -1;
    int wr_pool = fd_pool_alloc();
    if (wr_pool < 0) {
        fd_pool_free(rd_pool);
        return -1;
    }

    pipe_t *p = &g_pipes[pi];
    memset(p, 0, sizeof(*p));
    p->rd_refcount = 1;
    p->wr_refcount = 1;
    p->rd_pool_idx = rd_pool;
    p->wr_pool_idx = wr_pool;

    vfs_file_t *rd_vf = NULL;
    if (vfs_create_pipe_file(pi, false, 0, &rd_vf) != 0) {
        fd_pool_free(rd_pool);
        fd_pool_free(wr_pool);
        p->rd_refcount = 0;
        p->wr_refcount = 0;
        return -1;
    }
    fd_obj_attach_vfs(rd_pool, rd_vf);

    vfs_file_t *wr_vf = NULL;
    if (vfs_create_pipe_file(pi, true, 0, &wr_vf) != 0) {
        vfs_discard_unopened(rd_vf);
        fd_pool_free(rd_pool);
        fd_pool_free(wr_pool);
        p->rd_refcount = 0;
        p->wr_refcount = 0;
        return -1;
    }
    fd_obj_attach_vfs(wr_pool, wr_vf);

    int rd_fd = task_alloc_fd(task, rd_pool);
    if (rd_fd < 0) {
        vfs_discard_unopened(rd_vf);
        vfs_discard_unopened(wr_vf);
        fd_pool_free(rd_pool);
        fd_pool_free(wr_pool);
        p->rd_refcount = 0;
        p->wr_refcount = 0;
        return -1;
    }
    int wr_fd = task_alloc_fd(task, wr_pool);
    if (wr_fd < 0) {
        task->fd_table[rd_fd] = -1;
        vfs_discard_unopened(rd_vf);
        vfs_discard_unopened(wr_vf);
        fd_pool_free(rd_pool);
        fd_pool_free(wr_pool);
        p->rd_refcount = 0;
        p->wr_refcount = 0;
        return -1;
    }

    fds_out[0] = rd_fd;
    fds_out[1] = wr_fd;
    return 0;
}

int pipe_read_endpoint(int pipe_idx, void *buf, size_t count)
{
    pipe_t *p = pipe_get_by_idx(pipe_idx);
    if (!p || p->rd_refcount <= 0)
        return -1;

    uint8_t *dst = (uint8_t *)buf;
    size_t total = 0;

    while (total == 0) {
        while (p->count > 0 && total < count) {
            dst[total++] = p->buf[p->rd % PIPE_BUF_SIZE];
            p->rd++;
            p->count--;
        }
        if (total > 0)
            break;
        if (p->wr_refcount <= 0)
            return 0;
        p->blocked_reader = task_current();
        task_block(NULL);
        p->blocked_reader = NULL;
    }

    if (p->blocked_writer) {
        task_t *w = p->blocked_writer;
        p->blocked_writer = NULL;
        task_unblock(w);
    }

    fd_notify_waiters(p->wr_pool_idx, EPOLLOUT);

    return (int)total;
}

int pipe_write_endpoint(int pipe_idx, const void *buf, size_t count)
{
    pipe_t *p = pipe_get_by_idx(pipe_idx);
    if (!p || p->wr_refcount <= 0)
        return -1;

    if (p->rd_refcount <= 0) {
        task_send_signal(task_current(), SIGPIPE);
        return -32; /* -EPIPE */
    }

    const uint8_t *src = (const uint8_t *)buf;
    size_t total = 0;

    while (total < count) {
        while (p->count < PIPE_BUF_SIZE && total < count) {
            p->buf[p->wr % PIPE_BUF_SIZE] = src[total++];
            p->wr++;
            p->count++;
        }

        if (p->blocked_reader) {
            task_t *r = p->blocked_reader;
            p->blocked_reader = NULL;
            task_unblock(r);
        }

        fd_notify_waiters(p->rd_pool_idx, EPOLLIN);

        if (total < count) {
            if (p->rd_refcount <= 0) {
                task_send_signal(task_current(), SIGPIPE);
                return total > 0 ? (int)total : -32;
            }
            p->blocked_writer = task_current();
            task_block(NULL);
            p->blocked_writer = NULL;
        }
    }

    return (int)total;
}

void pipe_ref_endpoint(int pipe_idx, bool is_write_end)
{
    pipe_t *p = pipe_get_by_idx(pipe_idx);
    if (!p)
        return;
    if (is_write_end)
        p->wr_refcount++;
    else
        p->rd_refcount++;
}

void pipe_close_endpoint(int pipe_idx, bool is_write_end)
{
    pipe_t *p = pipe_get_by_idx(pipe_idx);
    if (!p)
        return;
    if (is_write_end) {
        if (--p->wr_refcount <= 0) {
            p->wr_refcount = 0;
            if (p->blocked_reader) {
                task_t *r = p->blocked_reader;
                p->blocked_reader = NULL;
                task_unblock(r);
            }
            fd_notify_waiters(p->rd_pool_idx, EPOLLHUP);
        }
        return;
    }

    if (--p->rd_refcount <= 0) {
        p->rd_refcount = 0;
        if (p->blocked_writer) {
            task_t *w = p->blocked_writer;
            p->blocked_writer = NULL;
            task_unblock(w);
        }
        fd_notify_waiters(p->wr_pool_idx, EPOLLERR);
    }
}

bool pipe_poll_readable_endpoint(int pipe_idx)
{
    pipe_t *p = pipe_get_by_idx(pipe_idx);
    if (!p)
        return false;
    return p->count > 0 || p->wr_refcount <= 0;
}

bool pipe_poll_writable_endpoint(int pipe_idx)
{
    pipe_t *p = pipe_get_by_idx(pipe_idx);
    if (!p)
        return false;
    return p->count < PIPE_BUF_SIZE || p->rd_refcount <= 0;
}
