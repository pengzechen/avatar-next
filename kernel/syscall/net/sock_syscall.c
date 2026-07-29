/*
 * net/sock_syscall.c - socket syscall handlers
 *
 * Translates Linux syscall ABI (sockaddr_in, etc.) into
 * kernel socket API (ksocket.h) calls.
 */
#include "syscall/syscall_internal.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/net/ksocket.h"
#include "task/task.h"
#include "klog.h"
#include "string.h"

/* Linux sockaddr_in layout */
struct kernel_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;       /* network byte order */
    uint32_t sin_addr;       /* network byte order */
    uint8_t  sin_zero[8];
};

#define AF_INET    2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_NONBLOCK 04000
#define O_NONBLOCK    04000

static uint16_t ntohs_val(uint16_t x)
{
    return (uint16_t)((x >> 8) | (x << 8));
}

static uint16_t htons_val(uint16_t x)
{
    return (uint16_t)((x >> 8) | (x << 8));
}

void socket_handler(uint64_t regs[6], task_t *current)
{
    int domain   = (int)regs[0];
    int type     = (int)regs[1];
    int protocol = (int)regs[2];

    int si = ksock_create(domain, type, protocol);
    if (si < 0) {
        regs[0] = (uint64_t)(int64_t)si;
        return;
    }

    int pool = fd_pool_alloc();
    if (pool < 0) {
        ksock_close(si);
        regs[0] = (uint64_t)(int64_t)-EMFILE;
        return;
    }

    fd_obj_t *obj = &g_fd_pool[pool];
    obj->type = FDT_SOCKET;
    obj->flags = (type & SOCK_NONBLOCK) ? O_NONBLOCK : 0;
    obj->sock.sock_idx = (int16_t)si;
    obj->path[0] = '\0';

    int fd = task_alloc_fd(current, pool);
    if (fd < 0) {
        ksock_close(si);
        fd_pool_free(pool);
        regs[0] = (uint64_t)(int64_t)-EMFILE;
        return;
    }

    regs[0] = (uint64_t)fd;
}

static int get_sock_idx(task_t *current, int fd)
{
    fd_obj_t *obj = task_get_fd(current, fd);
    if (!obj || obj->type != FDT_SOCKET)
        return -1;
    return obj->sock.sock_idx;
}

void bind_handler(uint64_t regs[6], task_t *current)
{
    int fd = (int)regs[0];
    struct kernel_sockaddr_in *addr = (struct kernel_sockaddr_in *)regs[1];

    int si = get_sock_idx(current, fd);
    if (si < 0) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }
    if (!addr)   { regs[0] = (uint64_t)(int64_t)-EFAULT; return; }

    int rc = ksock_bind(si, addr->sin_addr, ntohs_val(addr->sin_port));
    regs[0] = rc < 0 ? (uint64_t)(int64_t)rc : 0;
}

void listen_handler(uint64_t regs[6], task_t *current)
{
    int fd      = (int)regs[0];
    int backlog = (int)regs[1];

    int si = get_sock_idx(current, fd);
    if (si < 0) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }

    int rc = ksock_listen(si, backlog);
    regs[0] = rc < 0 ? (uint64_t)(int64_t)rc : 0;
}

void accept_handler(uint64_t regs[6], task_t *current)
{
    int fd = (int)regs[0];
    struct kernel_sockaddr_in *addr = (struct kernel_sockaddr_in *)regs[1];
    uint32_t *addrlen = (uint32_t *)regs[2];
    int accept_flags = (int)regs[3];

    fd_obj_t *listen_obj = task_get_fd(current, fd);
    if (!listen_obj || listen_obj->type != FDT_SOCKET) {
        regs[0] = (uint64_t)(int64_t)-EBADF;
        return;
    }
    int si = listen_obj->sock.sock_idx;

    int kflags = 0;
    if ((listen_obj->flags & O_NONBLOCK) || (accept_flags & SOCK_NONBLOCK))
        kflags |= KSOCK_MSG_DONTWAIT;

    uint32_t out_addr;
    uint16_t out_port;
    int new_si = ksock_accept(si, &out_addr, &out_port, kflags);
    KLOG_DEBUG("[sock] accept fd=%d listen_flags=0x%x accept_flags=0x%x kflags=0x%x rc=%d\n",
               fd, listen_obj->flags, accept_flags, kflags, new_si);
    if (new_si < 0) {
        regs[0] = (uint64_t)(int64_t)new_si;
        return;
    }

    int pool = fd_pool_alloc();
    if (pool < 0) {
        ksock_close(new_si);
        regs[0] = (uint64_t)(int64_t)-EMFILE;
        return;
    }

    fd_obj_t *obj = &g_fd_pool[pool];
    obj->type = FDT_SOCKET;
    obj->flags = 0;
    if ((listen_obj->flags & O_NONBLOCK) || (accept_flags & SOCK_NONBLOCK))
        obj->flags |= O_NONBLOCK;
    obj->sock.sock_idx = (int16_t)new_si;
    obj->path[0] = '\0';

    int new_fd = task_alloc_fd(current, pool);
    if (new_fd < 0) {
        ksock_close(new_si);
        fd_pool_free(pool);
        regs[0] = (uint64_t)(int64_t)-EMFILE;
        return;
    }

    if (addr) {
        memset(addr, 0, sizeof(*addr));
        addr->sin_family = AF_INET;
        addr->sin_port = htons_val(out_port);
        addr->sin_addr = out_addr;
    }
    if (addrlen)
        *addrlen = sizeof(struct kernel_sockaddr_in);

    regs[0] = (uint64_t)new_fd;
}

void connect_handler(uint64_t regs[6], task_t *current)
{
    int fd = (int)regs[0];
    struct kernel_sockaddr_in *addr = (struct kernel_sockaddr_in *)regs[1];

    int si = get_sock_idx(current, fd);
    if (si < 0) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }
    if (!addr)   { regs[0] = (uint64_t)(int64_t)-EFAULT; return; }

    int rc = ksock_connect(si, addr->sin_addr, ntohs_val(addr->sin_port));
    regs[0] = rc < 0 ? (uint64_t)(int64_t)rc : 0;
}

void sendto_handler(uint64_t regs[6], task_t *current)
{
    int fd           = (int)regs[0];
    const void *buf  = (const void *)regs[1];
    size_t len       = (size_t)regs[2];
    int flags        = (int)regs[3];
    struct kernel_sockaddr_in *dest = (struct kernel_sockaddr_in *)regs[4];

    int si = get_sock_idx(current, fd);
    if (si < 0) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }

    int rc;
    if (dest) {
        rc = ksock_sendto(si, buf, len, flags,
                          dest->sin_addr, ntohs_val(dest->sin_port));
    } else {
        rc = ksock_send(si, buf, len, flags);
    }
    regs[0] = rc < 0 ? (uint64_t)(int64_t)rc : (uint64_t)rc;
}

void recvfrom_handler(uint64_t regs[6], task_t *current)
{
    int fd           = (int)regs[0];
    void *buf        = (void *)regs[1];
    size_t len       = (size_t)regs[2];
    int flags        = (int)regs[3];
    struct kernel_sockaddr_in *src = (struct kernel_sockaddr_in *)regs[4];
    uint32_t *addrlen = (uint32_t *)regs[5];

    int si = get_sock_idx(current, fd);
    if (si < 0) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }

    int rc;
    if (src) {
        uint32_t out_addr;
        uint16_t out_port;
        rc = ksock_recvfrom(si, buf, len, flags, &out_addr, &out_port);
        if (rc >= 0) {
            memset(src, 0, sizeof(*src));
            src->sin_family = AF_INET;
            src->sin_port = htons_val(out_port);
            src->sin_addr = out_addr;
            if (addrlen)
                *addrlen = sizeof(struct kernel_sockaddr_in);
        }
    } else {
        rc = ksock_recv(si, buf, len, flags);
    }
    regs[0] = rc < 0 ? (uint64_t)(int64_t)rc : (uint64_t)rc;
}

void setsockopt_handler(uint64_t regs[6], task_t *current)
{
    int fd        = (int)regs[0];
    int level     = (int)regs[1];
    int optname   = (int)regs[2];
    const void *v = (const void *)regs[3];
    uint32_t len  = (uint32_t)regs[4];

    int si = get_sock_idx(current, fd);
    if (si < 0) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }

    int rc = ksock_setsockopt(si, level, optname, v, len);
    regs[0] = rc < 0 ? (uint64_t)(int64_t)rc : 0;
}

void getsockopt_handler(uint64_t regs[6], task_t *current)
{
    int fd         = (int)regs[0];
    int level      = (int)regs[1];
    int optname    = (int)regs[2];
    void *v        = (void *)regs[3];
    uint32_t *len  = (uint32_t *)regs[4];

    int si = get_sock_idx(current, fd);
    if (si < 0) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }

    int rc = ksock_getsockopt(si, level, optname, v, len);
    regs[0] = rc < 0 ? (uint64_t)(int64_t)rc : 0;
}

void getsockname_handler(uint64_t regs[6], task_t *current)
{
    int fd = (int)regs[0];
    struct kernel_sockaddr_in *addr = (struct kernel_sockaddr_in *)regs[1];
    uint32_t *addrlen = (uint32_t *)regs[2];

    int si = get_sock_idx(current, fd);
    if (si < 0) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }

    uint32_t a; uint16_t p;
    int rc = ksock_getsockname(si, &a, &p);
    if (rc < 0) { regs[0] = (uint64_t)(int64_t)rc; return; }

    if (addr) {
        memset(addr, 0, sizeof(*addr));
        addr->sin_family = AF_INET;
        addr->sin_port = htons_val(p);
        addr->sin_addr = a;
    }
    if (addrlen) *addrlen = sizeof(struct kernel_sockaddr_in);
    regs[0] = 0;
}

void getpeername_handler(uint64_t regs[6], task_t *current)
{
    int fd = (int)regs[0];
    struct kernel_sockaddr_in *addr = (struct kernel_sockaddr_in *)regs[1];
    uint32_t *addrlen = (uint32_t *)regs[2];

    int si = get_sock_idx(current, fd);
    if (si < 0) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }

    uint32_t a; uint16_t p;
    int rc = ksock_getpeername(si, &a, &p);
    if (rc < 0) { regs[0] = (uint64_t)(int64_t)rc; return; }

    if (addr) {
        memset(addr, 0, sizeof(*addr));
        addr->sin_family = AF_INET;
        addr->sin_port = htons_val(p);
        addr->sin_addr = a;
    }
    if (addrlen) *addrlen = sizeof(struct kernel_sockaddr_in);
    regs[0] = 0;
}

void shutdown_handler(uint64_t regs[6], task_t *current)
{
    int fd  = (int)regs[0];
    int how = (int)regs[1];

    int si = get_sock_idx(current, fd);
    if (si < 0) { regs[0] = (uint64_t)(int64_t)-EBADF; return; }

    int rc = ksock_shutdown(si, how);
    regs[0] = rc < 0 ? (uint64_t)(int64_t)rc : 0;
}
