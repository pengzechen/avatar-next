/*
 * fs/pty.c - pseudo-terminal (PTY) implementation
 *
 * Provides /dev/ptmx (master) and /dev/pts/N (slave) pairs.
 * Dropbear SSH opens ptmx, forks, child opens pts/N, exec shell.
 */
#include "syscall/fs/pty.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/io/epoll.h"
#include "syscall/syscall_internal.h"
#include "task/task.h"
#include "klog.h"
#include "string.h"

static pty_pair_t g_ptys[PTY_MAX];

static void pty_notify_epoll(int pty_idx, bool notify_master, uint32_t events)
{
    for (int i = 0; i < FD_POOL_SIZE; i++) {
        fd_obj_t *obj = &g_fd_pool[i];
        if (obj->type == FDT_PTY && obj->pty.pty_idx == pty_idx &&
            obj->pty.is_master == notify_master)
            fd_notify_waiters(i, events);
    }
}

static uint32_t ring_read(pty_ring_t *r, void *buf, size_t count)
{
    uint8_t *dst = (uint8_t *)buf;
    uint32_t n = 0;
    while (n < count && r->count > 0) {
        dst[n++] = r->buf[r->rd % PTY_BUF_SIZE];
        r->rd++;
        r->count--;
    }
    return n;
}

static uint32_t ring_write(pty_ring_t *r, const void *buf, size_t count)
{
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t n = 0;
    while (n < count && r->count < PTY_BUF_SIZE) {
        r->buf[r->wr % PTY_BUF_SIZE] = src[n++];
        r->wr++;
        r->count++;
    }
    return n;
}

int pty_match_pts_path(const char *path)
{
    if (!path) return -1;
    /* /dev/pts/0 .. /dev/pts/7 */
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' &&
        path[4] == '/' && path[5] == 'p' && path[6] == 't' && path[7] == 's' &&
        path[8] == '/') {
        int idx = 0;
        for (int i = 9; path[i]; i++) {
            if (path[i] < '0' || path[i] > '9') return -1;
            idx = idx * 10 + (path[i] - '0');
        }
        if (idx >= 0 && idx < PTY_MAX && g_ptys[idx].in_use)
            return idx;
    }
    return -1;
}

int pty_alloc_master(task_t *task)
{
    int pi = -1;
    for (int i = 0; i < PTY_MAX; i++) {
        if (!g_ptys[i].in_use) { pi = i; break; }
    }
    if (pi < 0) return -24; /* EMFILE */

    int pool = fd_pool_alloc();
    if (pool < 0) return -24;

    pty_pair_t *p = &g_ptys[pi];
    memset(p, 0, sizeof(*p));
    p->in_use = true;
    p->master_open = true;
    p->master_refcnt = 1;
    p->locked = true;
    p->ws_row = 24;
    p->ws_col = 80;
    p->c_lflag = 0x8A3B; /* ECHO|ECHOE|ECHOK|ECHOCTL|ECHOKE|ICANON|ISIG|IEXTEN */
    p->c_iflag = 0x0500; /* ICRNL|IXON */
    p->c_oflag = 0x0005; /* OPOST|ONLCR */
    p->c_cflag = 0x00BF; /* CS8|CREAD|HUPCL|B38400 */

    fd_obj_t *obj = &g_fd_pool[pool];
    obj->type = FDT_PTY;
    obj->flags = 0;
    obj->pty.pty_idx = (int16_t)pi;
    obj->pty.is_master = true;
    memcpy(obj->path, "/dev/ptmx", sizeof("/dev/ptmx"));

    int fd = task_alloc_fd(task, pool);
    if (fd < 0) {
        fd_pool_free(pool);
        p->in_use = false;
        return -24;
    }

    return fd;
}

int pty_open_slave(task_t *task, int pty_idx)
{
    if (pty_idx < 0 || pty_idx >= PTY_MAX)
        return -2; /* ENOENT */
    pty_pair_t *p = &g_ptys[pty_idx];
    if (!p->in_use || !p->master_open)
        return -2;
    if (p->locked)
        return -13; /* EACCES */

    int pool = fd_pool_alloc();
    if (pool < 0) return -24;

    fd_obj_t *obj = &g_fd_pool[pool];
    obj->type = FDT_PTY;
    obj->flags = 0;
    obj->pty.pty_idx = (int16_t)pty_idx;
    obj->pty.is_master = false;
    obj->path[0] = '/'; obj->path[1] = 'd'; obj->path[2] = 'e'; obj->path[3] = 'v';
    obj->path[4] = '/'; obj->path[5] = 'p'; obj->path[6] = 't'; obj->path[7] = 's';
    obj->path[8] = '/';
    if (pty_idx < 10) {
        obj->path[9] = (char)('0' + pty_idx);
        obj->path[10] = '\0';
    } else {
        obj->path[9] = (char)('0' + pty_idx / 10);
        obj->path[10] = (char)('0' + pty_idx % 10);
        obj->path[11] = '\0';
    }

    int fd = task_alloc_fd(task, pool);
    if (fd < 0) {
        fd_pool_free(pool);
        return -24;
    }

    p->slave_open = true;
    p->slave_refcnt++;
    return fd;
}

int pty_master_read(int pty_idx, void *buf, size_t count, bool nonblock)
{
    if (pty_idx < 0 || pty_idx >= PTY_MAX) return -9;
    pty_pair_t *p = &g_ptys[pty_idx];

    while (p->s2m.count == 0) {
        if (!p->slave_open) return 0;
        if (nonblock) return -EAGAIN;
        p->blocked_master = task_current();
        task_block(NULL);
        p->blocked_master = NULL;
    }

    uint32_t n = ring_read(&p->s2m, buf, count);
    if (p->blocked_slave) {
        task_t *t = p->blocked_slave;
        p->blocked_slave = NULL;
        task_unblock(t);
    }
    pty_notify_epoll(pty_idx, false, EPOLLOUT);
    return (int)n;
}

int pty_master_write(int pty_idx, const void *buf, size_t count, bool nonblock)
{
    if (pty_idx < 0 || pty_idx >= PTY_MAX) return -9;
    pty_pair_t *p = &g_ptys[pty_idx];

    const uint8_t *src = (const uint8_t *)buf;
    bool icrnl = (p->c_iflag & 0x0100) != 0;
    size_t total = 0;

    while (total < count) {
        while (total < count && p->m2s.count < PTY_BUF_SIZE) {
            uint8_t c = src[total];
            if (icrnl && c == '\r') c = '\n';
            p->m2s.buf[p->m2s.wr % PTY_BUF_SIZE] = c;
            p->m2s.wr++;
            p->m2s.count++;
            total++;
        }

        if (p->blocked_slave) {
            task_t *t = p->blocked_slave;
            p->blocked_slave = NULL;
            task_unblock(t);
        }
        pty_notify_epoll(pty_idx, false, EPOLLIN);

        if (total < count) {
            if (!p->slave_open) return total > 0 ? (int)total : -32;
            if (nonblock) return total > 0 ? (int)total : -EAGAIN;
            p->blocked_master = task_current();
            task_block(NULL);
            p->blocked_master = NULL;
        }
    }
    return (int)total;
}

int pty_slave_read(int pty_idx, void *buf, size_t count, bool nonblock)
{
    if (pty_idx < 0 || pty_idx >= PTY_MAX) return -9;
    pty_pair_t *p = &g_ptys[pty_idx];

    while (p->m2s.count == 0) {
        if (!p->master_open) return 0;
        if (nonblock) return -EAGAIN;
        p->blocked_slave = task_current();
        task_block(NULL);
        p->blocked_slave = NULL;
    }

    uint32_t n = ring_read(&p->m2s, buf, count);
    if (p->blocked_master) {
        task_t *t = p->blocked_master;
        p->blocked_master = NULL;
        task_unblock(t);
    }
    pty_notify_epoll(pty_idx, true, EPOLLOUT);
    return (int)n;
}

int pty_slave_write(int pty_idx, const void *buf, size_t count, bool nonblock)
{
    if (pty_idx < 0 || pty_idx >= PTY_MAX) return -9;
    pty_pair_t *p = &g_ptys[pty_idx];

    const uint8_t *src = (const uint8_t *)buf;
    bool onlcr = (p->c_oflag & 0x0001) && (p->c_oflag & 0x0004);
    size_t si = 0;

    while (si < count) {
        if (onlcr) {
            while (si < count && p->s2m.count < PTY_BUF_SIZE) {
                uint8_t c = src[si];
                if (c == '\n') {
                    if (p->s2m.count + 2 > PTY_BUF_SIZE) break;
                    p->s2m.buf[p->s2m.wr % PTY_BUF_SIZE] = '\r';
                    p->s2m.wr++;
                    p->s2m.count++;
                }
                p->s2m.buf[p->s2m.wr % PTY_BUF_SIZE] = c;
                p->s2m.wr++;
                p->s2m.count++;
                si++;
            }
        } else {
            uint32_t n = ring_write(&p->s2m, src + si, count - si);
            si += n;
        }

        if (p->blocked_master) {
            task_t *t = p->blocked_master;
            p->blocked_master = NULL;
            task_unblock(t);
        }
        pty_notify_epoll(pty_idx, true, EPOLLIN);

        if (si < count) {
            if (!p->master_open) return si > 0 ? (int)si : -32;
            if (nonblock) return si > 0 ? (int)si : -EAGAIN;
            p->blocked_slave = task_current();
            task_block(NULL);
            p->blocked_slave = NULL;
        }
    }
    return (int)count;
}

void pty_close_master(int pty_idx)
{
    if (pty_idx < 0 || pty_idx >= PTY_MAX) return;
    pty_pair_t *p = &g_ptys[pty_idx];
    if (p->master_refcnt > 0)
        p->master_refcnt--;
    if (p->master_refcnt == 0) {
        p->master_open = false;
        if (p->blocked_slave) {
            task_t *t = p->blocked_slave;
            p->blocked_slave = NULL;
            task_unblock(t);
        }
        pty_notify_epoll(pty_idx, false, EPOLLHUP);
        if (!p->slave_open)
            p->in_use = false;
    }
}

void pty_ref_master(int pty_idx)
{
    if (pty_idx < 0 || pty_idx >= PTY_MAX) return;
    g_ptys[pty_idx].master_refcnt++;
}

void pty_ref_slave(int pty_idx)
{
    if (pty_idx < 0 || pty_idx >= PTY_MAX) return;
    g_ptys[pty_idx].slave_refcnt++;
}

void pty_close_slave(int pty_idx)
{
    if (pty_idx < 0 || pty_idx >= PTY_MAX) return;
    pty_pair_t *p = &g_ptys[pty_idx];
    if (p->slave_refcnt > 0)
        p->slave_refcnt--;
    if (p->slave_refcnt == 0) {
        p->slave_open = false;
        if (p->blocked_master) {
            task_t *t = p->blocked_master;
            p->blocked_master = NULL;
            task_unblock(t);
        }
        pty_notify_epoll(pty_idx, true, EPOLLHUP);
        if (!p->master_open)
            p->in_use = false;
    }
}

int pty_ioctl(int pty_idx, bool is_master, uint32_t req, void *argp)
{
    (void)is_master;
    if (pty_idx < 0 || pty_idx >= PTY_MAX) return -9;
    pty_pair_t *p = &g_ptys[pty_idx];

    switch (req) {
    case TIOCGPTN: {
        if (!argp) return -14;
        *(uint32_t *)argp = (uint32_t)pty_idx;
        return 0;
    }
    case TIOCSPTLCK: {
        if (!argp) return -14;
        p->locked = *(int *)argp ? true : false;
        return 0;
    }
    case TCGETS: {
        if (!argp) return -14;
        struct kernel_termios *t = (struct kernel_termios *)argp;
        t->c_iflag = p->c_iflag;
        t->c_oflag = p->c_oflag;
        t->c_cflag = p->c_cflag;
        t->c_lflag = p->c_lflag;
        t->c_line  = 0;
        memcpy(t->c_cc, p->c_cc, sizeof(p->c_cc));
        return 0;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        if (!argp) return -14;
        struct kernel_termios *t = (struct kernel_termios *)argp;
        p->c_iflag = t->c_iflag;
        p->c_oflag = t->c_oflag;
        p->c_cflag = t->c_cflag;
        p->c_lflag = t->c_lflag;
        memcpy(p->c_cc, t->c_cc, sizeof(p->c_cc));
        return 0;
    }
    case TIOCGWINSZ: {
        if (!argp) return -14;
        struct kernel_winsize *ws = (struct kernel_winsize *)argp;
        ws->ws_row = p->ws_row;
        ws->ws_col = p->ws_col;
        ws->ws_xpixel = p->ws_xpixel;
        ws->ws_ypixel = p->ws_ypixel;
        return 0;
    }
    case TIOCSWINSZ: {
        if (!argp) return -14;
        struct kernel_winsize *ws = (struct kernel_winsize *)argp;
        p->ws_row = ws->ws_row;
        p->ws_col = ws->ws_col;
        p->ws_xpixel = ws->ws_xpixel;
        p->ws_ypixel = ws->ws_ypixel;
        return 0;
    }
    case TIOCGPGRP:
        if (!argp) return -14;
        *(int *)argp = (int)p->fg_pgid;
        return 0;
    case TIOCSPGRP:
        if (!argp) return -14;
        p->fg_pgid = (uint32_t)*(int *)argp;
        return 0;
    case TIOCSCTTY: {
        task_t *cur = task_current();
        if (cur && !is_master)
            cur->ctty_pty_idx = (int16_t)pty_idx;
        if (cur && p->fg_pgid == 0)
            p->fg_pgid = cur->pgid;
        return 0;
    }
    case TIOCNOTTY:
        {
            task_t *cur = task_current();
            if (cur)
                cur->ctty_pty_idx = -1;
        }
        return 0;
    default:
        return -25; /* ENOTTY */
    }
}

bool pty_poll_readable_master(int pty_idx)
{
    if (pty_idx < 0 || pty_idx >= PTY_MAX) return false;
    return g_ptys[pty_idx].s2m.count > 0 || !g_ptys[pty_idx].slave_open;
}

bool pty_poll_readable_slave(int pty_idx)
{
    if (pty_idx < 0 || pty_idx >= PTY_MAX) return false;
    return g_ptys[pty_idx].m2s.count > 0 || !g_ptys[pty_idx].master_open;
}

bool pty_poll_writable(int pty_idx)
{
    (void)pty_idx;
    return true;
}
