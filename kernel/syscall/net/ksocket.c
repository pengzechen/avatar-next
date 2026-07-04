/*
 * net/ksocket.c - kernel socket layer over lwIP raw API
 *
 * Provides blocking socket semantics using task_block/task_unblock.
 * lwIP runs in NO_SYS=1 mode (no OS integration); all lwIP calls
 * happen from the net-poll task context via callbacks.
 */
#include "syscall/net/ksocket.h"
#include "syscall/fs/fd_pool.h"
#include "syscall/io/epoll.h"
#include "task/task.h"
#include "klog.h"
#include "string.h"

#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"

/* ── socket pool ─────────────────────────────────────────────── */

typedef struct ksock {
    ksock_state_t state;
    ksock_proto_t proto;

    struct tcp_pcb *tcp_pcb;
    struct udp_pcb *udp_pcb;

    /* recv queue: chain of pbufs */
    struct pbuf *recv_head;
    struct pbuf *recv_tail;
    uint32_t     recv_count;
    uint16_t     recv_offset;
    bool         recv_eof;

    /* accept queue (listening sockets): stores ksock indices */
    int             accept_queue[KSOCK_BACKLOG_MAX];
    int             accept_head;
    int             accept_tail;
    int             accept_count;

    /* UDP recvfrom: source address of last received packet */
    uint32_t udp_recv_addr;
    uint16_t udp_recv_port;

    /* local/remote address */
    uint32_t local_addr;
    uint16_t local_port;
    uint32_t remote_addr;
    uint16_t remote_port;

    /* blocking */
    struct task *blocked_task;

    /* error from lwIP callback */
    int last_err;

    int refcount;
} ksock_t;

static ksock_t g_ksocks[KSOCK_MAX];

void ksock_ref(int si)
{
    if (si >= 0 && si < KSOCK_MAX && g_ksocks[si].state != KSOCK_FREE)
        g_ksocks[si].refcount++;
}

static int ksock_alloc(void)
{
    for (int i = 0; i < KSOCK_MAX; i++) {
        if (g_ksocks[i].state == KSOCK_FREE)
            return i;
    }
    return -1;
}

static ksock_t *ksock_get(int si)
{
    if (si < 0 || si >= KSOCK_MAX)
        return NULL;
    if (g_ksocks[si].state == KSOCK_FREE)
        return NULL;
    return &g_ksocks[si];
}

static void ksock_unblock(ksock_t *sk)
{
    if (sk->blocked_task) {
        task_t *t = sk->blocked_task;
        sk->blocked_task = NULL;
        task_unblock(t);
    }
}

static void ksock_notify_epoll(int si, uint32_t events)
{
    for (int i = 0; i < FD_POOL_SIZE; i++) {
        if (g_fd_pool[i].type == FDT_SOCKET && g_fd_pool[i].sock.sock_idx == si)
            fd_notify_waiters(i, events);
    }
}

/* ── lwIP TCP callbacks ──────────────────────────────────────── */

static err_t ksock_tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static err_t ksock_tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err);
static err_t ksock_tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err);
static void  ksock_tcp_err_cb(void *arg, err_t err);
static err_t ksock_tcp_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len);

static err_t ksock_tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    (void)tpcb;
    ksock_t *sk = (ksock_t *)arg;
    if (!sk)
        return ERR_VAL;

    if (err != ERR_OK) {
        if (p) pbuf_free(p);
        sk->last_err = -5; /* EIO */
        ksock_unblock(sk);
        ksock_notify_epoll((int)(sk - g_ksocks), EPOLLERR);
        return ERR_OK;
    }

    if (!p) {
        sk->recv_eof = true;
        ksock_unblock(sk);
        ksock_notify_epoll((int)(sk - g_ksocks), EPOLLHUP | EPOLLIN);
        return ERR_OK;
    }

    if (sk->recv_count >= KSOCK_RECV_QMAX) {
        return ERR_MEM;
    }

    KLOG_DEBUG("[tcp_recv_cb] si=%d plen=%u tot=%u cur_off=%u cur_tot=%u\n",
               (int)(sk - g_ksocks), p->len, p->tot_len,
               sk->recv_offset,
               sk->recv_head ? sk->recv_head->tot_len : 0);

    if (sk->recv_head)
        pbuf_cat(sk->recv_head, p);
    else
        sk->recv_head = p;
    sk->recv_tail = p;
    while (sk->recv_tail->next)
        sk->recv_tail = sk->recv_tail->next;
    sk->recv_count++;

    tcp_recved(tpcb, p->tot_len);
    ksock_unblock(sk);
    ksock_notify_epoll((int)(sk - g_ksocks), EPOLLIN);
    return ERR_OK;
}

static err_t ksock_tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    ksock_t *sk = (ksock_t *)arg;
    if (!sk || err != ERR_OK || !newpcb)
        return ERR_VAL;

    if (sk->accept_count >= KSOCK_BACKLOG_MAX) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    int new_si = ksock_alloc();
    if (new_si < 0) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    ksock_t *nsk = &g_ksocks[new_si];
    memset(nsk, 0, sizeof(*nsk));
    nsk->refcount = 1;
    nsk->state = KSOCK_CONNECTED;
    nsk->proto = KSOCK_TCP;
    nsk->tcp_pcb = newpcb;
    nsk->remote_addr = ip4_addr_get_u32(&newpcb->remote_ip);
    nsk->remote_port = newpcb->remote_port;
    nsk->local_addr  = ip4_addr_get_u32(&newpcb->local_ip);
    nsk->local_port  = newpcb->local_port;

    tcp_arg(newpcb, nsk);
    tcp_recv(newpcb, ksock_tcp_recv_cb);
    tcp_err(newpcb, ksock_tcp_err_cb);
    tcp_sent(newpcb, ksock_tcp_sent_cb);

    sk->accept_queue[sk->accept_tail] = new_si;
    sk->accept_tail = (sk->accept_tail + 1) % KSOCK_BACKLOG_MAX;
    sk->accept_count++;
    ksock_unblock(sk);
    ksock_notify_epoll((int)(sk - g_ksocks), EPOLLIN);
    return ERR_OK;
}

static err_t ksock_tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    (void)tpcb;
    ksock_t *sk = (ksock_t *)arg;
    if (!sk) return ERR_VAL;

    if (err == ERR_OK) {
        sk->state = KSOCK_CONNECTED;
        sk->remote_addr = ip4_addr_get_u32(&tpcb->remote_ip);
        sk->remote_port = tpcb->remote_port;
    } else {
        sk->last_err = -111; /* ECONNREFUSED */
    }
    ksock_unblock(sk);
    ksock_notify_epoll((int)(sk - g_ksocks), err == ERR_OK ? EPOLLOUT : EPOLLERR);
    return ERR_OK;
}

static void ksock_tcp_err_cb(void *arg, err_t err)
{
    ksock_t *sk = (ksock_t *)arg;
    if (!sk) return;

    sk->tcp_pcb = NULL;
    switch (err) {
    case ERR_RST:  sk->last_err = -104; break; /* ECONNRESET */
    case ERR_ABRT: sk->last_err = -103; break; /* ECONNABORTED */
    default:       sk->last_err = -5;   break; /* EIO */
    }
    sk->recv_eof = true;
    ksock_unblock(sk);
    ksock_notify_epoll((int)(sk - g_ksocks), EPOLLERR | EPOLLHUP);
}

static err_t ksock_tcp_sent_cb(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    (void)tpcb;
    (void)len;
    ksock_t *sk = (ksock_t *)arg;
    if (sk) {
        ksock_unblock(sk);
        ksock_notify_epoll((int)(sk - g_ksocks), EPOLLOUT);
    }
    return ERR_OK;
}

/* ── lwIP UDP callback ───────────────────────────────────────── */

static void ksock_udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                              const ip_addr_t *addr, u16_t port)
{
    (void)pcb;
    ksock_t *sk = (ksock_t *)arg;
    if (!sk || !p) { if (p) pbuf_free(p); return; }

    if (sk->recv_count >= KSOCK_RECV_QMAX) {
        pbuf_free(p);
        return;
    }

    sk->udp_recv_addr = ip4_addr_get_u32(addr);
    sk->udp_recv_port = port;

    if (sk->recv_tail)
        pbuf_chain(sk->recv_tail, p);
    else
        sk->recv_head = p;
    sk->recv_tail = p;
    sk->recv_count++;
    ksock_unblock(sk);
    ksock_notify_epoll((int)(sk - g_ksocks), EPOLLIN);
}

/* ── public API ──────────────────────────────────────────────── */

int ksock_create(int domain, int type, int protocol)
{
    (void)protocol;
    if (domain != 2 /* AF_INET */)
        return -97; /* EAFNOSUPPORT */

    int si = ksock_alloc();
    if (si < 0)
        return -24; /* EMFILE */

    ksock_t *sk = &g_ksocks[si];
    memset(sk, 0, sizeof(*sk));
    sk->refcount = 1;

    int sock_type = type & 0xFF;
    if (sock_type == 1) { /* SOCK_STREAM */
        sk->proto = KSOCK_TCP;
        sk->tcp_pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (!sk->tcp_pcb) {
            sk->state = KSOCK_FREE;
            return -105; /* ENOBUFS */
        }
        tcp_arg(sk->tcp_pcb, sk);
        tcp_recv(sk->tcp_pcb, ksock_tcp_recv_cb);
        tcp_err(sk->tcp_pcb, ksock_tcp_err_cb);
        tcp_sent(sk->tcp_pcb, ksock_tcp_sent_cb);
    } else if (sock_type == 2) { /* SOCK_DGRAM */
        sk->proto = KSOCK_UDP;
        sk->udp_pcb = udp_new();
        if (!sk->udp_pcb) {
            sk->state = KSOCK_FREE;
            return -105;
        }
        udp_recv(sk->udp_pcb, ksock_udp_recv_cb, sk);
    } else {
        return -95; /* ENOTSUP */
    }

    sk->state = KSOCK_CREATED;
    return si;
}

int ksock_bind(int si, uint32_t addr, uint16_t port)
{
    ksock_t *sk = ksock_get(si);
    if (!sk) return -9; /* EBADF */

    ip_addr_t bind_addr;
    ip_addr_set_ip4_u32(&bind_addr, addr);
    err_t rc;

    if (sk->proto == KSOCK_TCP) {
        rc = tcp_bind(sk->tcp_pcb, &bind_addr, port);
    } else {
        rc = udp_bind(sk->udp_pcb, &bind_addr, port);
    }

    if (rc == ERR_USE)
        return -98; /* EADDRINUSE */
    if (rc != ERR_OK)
        return -22; /* EINVAL */

    sk->local_addr = addr;
    sk->local_port = port;
    sk->state = KSOCK_BOUND;
    return 0;
}

int ksock_listen(int si, int backlog)
{
    (void)backlog;
    ksock_t *sk = ksock_get(si);
    if (!sk || sk->proto != KSOCK_TCP) return -9;

    struct tcp_pcb *lpcb = tcp_listen(sk->tcp_pcb);
    if (!lpcb)
        return -98; /* EADDRINUSE */

    sk->tcp_pcb = lpcb;
    tcp_arg(lpcb, sk);
    tcp_accept(lpcb, ksock_tcp_accept_cb);
    sk->state = KSOCK_LISTENING;
    return 0;
}

int ksock_accept(int si, uint32_t *out_addr, uint16_t *out_port)
{
    ksock_t *sk = ksock_get(si);
    if (!sk || sk->state != KSOCK_LISTENING) return -9;

    while (sk->accept_count == 0) {
        sk->blocked_task = task_current();
        task_block(NULL);
        sk->blocked_task = NULL;
        if (sk->last_err)
            return sk->last_err;
    }

    int new_si = sk->accept_queue[sk->accept_head];
    sk->accept_head = (sk->accept_head + 1) % KSOCK_BACKLOG_MAX;
    sk->accept_count--;

    ksock_t *nsk = ksock_get(new_si);
    if (!nsk) return -9;

    if (out_addr) *out_addr = nsk->remote_addr;
    if (out_port) *out_port = nsk->remote_port;

    return new_si;
}

int ksock_connect(int si, uint32_t addr, uint16_t port)
{
    ksock_t *sk = ksock_get(si);
    if (!sk) return -9;

    if (sk->proto == KSOCK_TCP) {
        ip_addr_t dest;
        ip_addr_set_ip4_u32(&dest, addr);
        sk->state = KSOCK_CONNECTING;
        sk->last_err = 0;

        err_t rc = tcp_connect(sk->tcp_pcb, &dest, port, ksock_tcp_connected_cb);
        if (rc != ERR_OK)
            return -101; /* ENETUNREACH */

        sk->blocked_task = task_current();
        task_block(NULL);
        sk->blocked_task = NULL;

        if (sk->last_err)
            return sk->last_err;
        return 0;
    } else {
        ip_addr_t dest;
        ip_addr_set_ip4_u32(&dest, addr);
        udp_connect(sk->udp_pcb, &dest, port);
        sk->remote_addr = addr;
        sk->remote_port = port;
        sk->state = KSOCK_CONNECTED;
        return 0;
    }
}

int ksock_send(int si, const void *buf, size_t len, int flags)
{
    ksock_t *sk = ksock_get(si);
    if (!sk) return -9;

    int nonblock = (flags & KSOCK_MSG_DONTWAIT);

    if (sk->proto == KSOCK_TCP) {
        if (sk->state != KSOCK_CONNECTED || !sk->tcp_pcb)
            return -107; /* ENOTCONN */

        size_t sent = 0;
        while (sent < len) {
            if (sk->last_err) return sk->last_err;
            if (!sk->tcp_pcb) return -104;

            u16_t sndbuf = tcp_sndbuf(sk->tcp_pcb);
            if (sndbuf == 0) {
                if (nonblock)
                    return sent > 0 ? (int)sent : -11; /* EAGAIN */
                sk->blocked_task = task_current();
                task_block(NULL);
                sk->blocked_task = NULL;
                continue;
            }

            u16_t chunk = (u16_t)((len - sent) < sndbuf ? (len - sent) : sndbuf);
            err_t rc = tcp_write(sk->tcp_pcb, (const uint8_t *)buf + sent, chunk,
                                 TCP_WRITE_FLAG_COPY);
            if (rc != ERR_OK) {
                tcp_output(sk->tcp_pcb);
                return sent > 0 ? (int)sent : -105;
            }
            sent += chunk;
        }
        tcp_output(sk->tcp_pcb);
        return (int)sent;
    } else {
        return ksock_sendto(si, buf, len, flags,
                           sk->remote_addr, sk->remote_port);
    }
}

int ksock_recv(int si, void *buf, size_t len, int flags)
{
    ksock_t *sk = ksock_get(si);
    if (!sk) return -9;

    int nonblock = (flags & KSOCK_MSG_DONTWAIT);

    if (sk->proto == KSOCK_TCP) {
        while (!sk->recv_head) {
            if (sk->recv_eof) return 0;
            if (sk->last_err) return sk->last_err;
            if (nonblock) return -11; /* EAGAIN */
            sk->blocked_task = task_current();
            task_block(NULL);
            sk->blocked_task = NULL;
        }

        uint16_t avail = sk->recv_head->tot_len - sk->recv_offset;
        uint16_t to_copy = (uint16_t)((len < avail) ? len : avail);
        uint16_t copied = pbuf_copy_partial(sk->recv_head, buf, to_copy,
                                            sk->recv_offset);
        sk->recv_offset += copied;

        /* DEBUG: 显示读取的字节 (仅对1字节读取打印前32次) */
        {
            static int dbg_cnt = 0;
            if (len == 1 && dbg_cnt < 64) {
                unsigned char c = ((unsigned char *)buf)[0];
                KLOG_DEBUG("[ksock_recv] si=%d off=%u tot=%u byte=0x%02x '%c'\n",
                           si, sk->recv_offset, sk->recv_head ? sk->recv_head->tot_len : 0,
                           c, (c >= 0x20 && c < 0x7f) ? c : '.');
                dbg_cnt++;
            }
        }

        if (sk->recv_offset >= sk->recv_head->tot_len) {
            struct pbuf *old = sk->recv_head;
            sk->recv_head = NULL;
            sk->recv_tail = NULL;
            sk->recv_count = 0;
            sk->recv_offset = 0;
            pbuf_free(old);
        }
        return (int)copied;
    } else {
        uint32_t dummy_addr;
        uint16_t dummy_port;
        return ksock_recvfrom(si, buf, len, flags, &dummy_addr, &dummy_port);
    }
}

int ksock_sendto(int si, const void *buf, size_t len, int flags,
                 uint32_t addr, uint16_t port)
{
    (void)flags;
    ksock_t *sk = ksock_get(si);
    if (!sk) return -9;

    if (sk->proto == KSOCK_TCP)
        return ksock_send(si, buf, len, flags);

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!p) return -105;
    memcpy(p->payload, buf, len);

    ip_addr_t dest;
    ip_addr_set_ip4_u32(&dest, addr);
    err_t rc = udp_sendto(sk->udp_pcb, p, &dest, port);
    pbuf_free(p);

    return rc == ERR_OK ? (int)len : -105;
}

int ksock_recvfrom(int si, void *buf, size_t len, int flags,
                   uint32_t *out_addr, uint16_t *out_port)
{
    ksock_t *sk = ksock_get(si);
    if (!sk) return -9;

    int nonblock = (flags & KSOCK_MSG_DONTWAIT);

    if (sk->proto == KSOCK_TCP)
        return ksock_recv(si, buf, len, flags);

    while (!sk->recv_head) {
        if (sk->last_err) return sk->last_err;
        if (nonblock) return -11; /* EAGAIN */
        sk->blocked_task = task_current();
        task_block(NULL);
        sk->blocked_task = NULL;
    }

    struct pbuf *p = sk->recv_head;
    uint16_t copied = pbuf_copy_partial(p, buf, (u16_t)len, 0);
    sk->recv_head = p->next;
    if (p->next)
        pbuf_ref(p->next);
    else
        sk->recv_tail = NULL;
    sk->recv_count--;
    pbuf_free(p);

    if (out_addr) *out_addr = sk->udp_recv_addr;
    if (out_port) *out_port = sk->udp_recv_port;

    return (int)copied;
}

int ksock_close(int si)
{
    ksock_t *sk = ksock_get(si);
    if (!sk) return -9;

    if (--sk->refcount > 0)
        return 0;

    /* close pre-allocated sockets in accept queue */
    if (sk->state == KSOCK_LISTENING) {
        while (sk->accept_count > 0) {
            int child_si = sk->accept_queue[sk->accept_head];
            sk->accept_head = (sk->accept_head + 1) % KSOCK_BACKLOG_MAX;
            sk->accept_count--;
            ksock_close(child_si);
        }
    }

    if (sk->proto == KSOCK_TCP && sk->tcp_pcb) {
        tcp_arg(sk->tcp_pcb, NULL);
        tcp_recv(sk->tcp_pcb, NULL);
        tcp_err(sk->tcp_pcb, NULL);
        tcp_sent(sk->tcp_pcb, NULL);
        tcp_close(sk->tcp_pcb);
    } else if (sk->proto == KSOCK_UDP && sk->udp_pcb) {
        udp_remove(sk->udp_pcb);
    }

    /* free recv queue */
    while (sk->recv_head) {
        struct pbuf *p = sk->recv_head;
        sk->recv_head = p->next;
        if (p->next)
            pbuf_ref(p->next);
        pbuf_free(p);
    }

    memset(sk, 0, sizeof(*sk));
    return 0;
}

int ksock_shutdown(int si, int how)
{
    ksock_t *sk = ksock_get(si);
    if (!sk) return -9;

    if (sk->proto == KSOCK_TCP && sk->tcp_pcb) {
        int shut_rx = (how == 0 || how == 2) ? 1 : 0;
        int shut_tx = (how == 1 || how == 2) ? 1 : 0;
        tcp_shutdown(sk->tcp_pcb, shut_rx, shut_tx);
    }
    return 0;
}

int ksock_setsockopt(int si, int level, int optname,
                     const void *optval, uint32_t optlen)
{
    (void)si; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

int ksock_getsockopt(int si, int level, int optname,
                     void *optval, uint32_t *optlen)
{
    (void)level; (void)optname;
    ksock_t *sk = ksock_get(si);
    if (!sk) return -9;

    if (optval && optlen && *optlen >= 4) {
        *(int *)optval = sk->last_err ? -sk->last_err : 0;
        *optlen = 4;
    }
    return 0;
}

int ksock_getsockname(int si, uint32_t *addr, uint16_t *port)
{
    ksock_t *sk = ksock_get(si);
    if (!sk) return -9;
    if (addr) *addr = sk->local_addr;
    if (port) *port = sk->local_port;
    return 0;
}

int ksock_getpeername(int si, uint32_t *addr, uint16_t *port)
{
    ksock_t *sk = ksock_get(si);
    if (!sk) return -9;
    if (sk->state != KSOCK_CONNECTED)
        return -107;
    if (addr) *addr = sk->remote_addr;
    if (port) *port = sk->remote_port;
    return 0;
}

bool ksock_poll_readable(int si)
{
    ksock_t *sk = ksock_get(si);
    if (!sk) return false;
    if (sk->state == KSOCK_LISTENING)
        return sk->accept_count > 0;
    return sk->recv_head != NULL || sk->recv_eof;
}

bool ksock_poll_writable(int si)
{
    ksock_t *sk = ksock_get(si);
    if (!sk) return false;
    if (sk->proto == KSOCK_TCP && sk->tcp_pcb)
        return tcp_sndbuf(sk->tcp_pcb) > 0;
    return true;
}
