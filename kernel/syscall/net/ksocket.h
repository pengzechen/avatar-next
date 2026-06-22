#ifndef KERNEL_SYSCALL_NET_KSOCKET_H
#define KERNEL_SYSCALL_NET_KSOCKET_H

#include "types.h"

struct task;

#define KSOCK_MAX         32
#define KSOCK_BACKLOG_MAX  8
#define KSOCK_RECV_QMAX   16
#define KSOCK_MSG_DONTWAIT 0x40

typedef enum {
    KSOCK_FREE = 0,
    KSOCK_CREATED,
    KSOCK_BOUND,
    KSOCK_LISTENING,
    KSOCK_CONNECTING,
    KSOCK_CONNECTED,
    KSOCK_CLOSED,
} ksock_state_t;

typedef enum {
    KSOCK_TCP = 0,
    KSOCK_UDP,
} ksock_proto_t;

int  ksock_create(int domain, int type, int protocol);
int  ksock_bind(int si, uint32_t addr, uint16_t port);
int  ksock_listen(int si, int backlog);
int  ksock_accept(int si, uint32_t *out_addr, uint16_t *out_port);
int  ksock_connect(int si, uint32_t addr, uint16_t port);
int  ksock_send(int si, const void *buf, size_t len, int flags);
int  ksock_recv(int si, void *buf, size_t len, int flags);
int  ksock_sendto(int si, const void *buf, size_t len, int flags,
                  uint32_t addr, uint16_t port);
int  ksock_recvfrom(int si, void *buf, size_t len, int flags,
                    uint32_t *out_addr, uint16_t *out_port);
int  ksock_close(int si);
void ksock_ref(int si);
int  ksock_shutdown(int si, int how);
int  ksock_setsockopt(int si, int level, int optname,
                      const void *optval, uint32_t optlen);
int  ksock_getsockopt(int si, int level, int optname,
                      void *optval, uint32_t *optlen);
int  ksock_getsockname(int si, uint32_t *addr, uint16_t *port);
int  ksock_getpeername(int si, uint32_t *addr, uint16_t *port);

bool ksock_poll_readable(int si);
bool ksock_poll_writable(int si);

#endif /* KERNEL_SYSCALL_NET_KSOCKET_H */
