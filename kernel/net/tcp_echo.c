#include "net/tcp_echo.h"

#include "klog.h"

#include "lwip/tcp.h"

static err_t echo_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    (void)arg;

    if (err != ERR_OK) {
        if (p)
            pbuf_free(p);
        return err;
    }

    if (!p) {
        tcp_close(tpcb);
        return ERR_OK;
    }

    tcp_recved(tpcb, p->tot_len);

    for (struct pbuf *q = p; q != NULL; q = q->next) {
        err_t wr = tcp_write(tpcb, q->payload, q->len, TCP_WRITE_FLAG_COPY);
        if (wr != ERR_OK) {
            KLOG_WARN("[tcp-echo] tcp_write failed rc=%d len=%u\n",
                      wr, (unsigned)q->len);
            break;
        }
        if (q->tot_len == q->len)
            break;
    }
    tcp_output(tpcb);
    pbuf_free(p);
    return ERR_OK;
}

static void echo_err(void *arg, err_t err)
{
    (void)arg;
    KLOG_WARN("[tcp-echo] connection aborted err=%d\n", err);
}

static err_t echo_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;

    if (err != ERR_OK || !newpcb)
        return ERR_VAL;

    tcp_arg(newpcb, NULL);
    tcp_recv(newpcb, echo_recv);
    tcp_err(newpcb, echo_err);
    KLOG_INFO("[tcp-echo] accepted connection\n");
    return ERR_OK;
}

void tcp_echo_init(void)
{
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) {
        KLOG_ERROR("[tcp-echo] tcp_new failed\n");
        return;
    }

    err_t rc = tcp_bind(pcb, IP_ADDR_ANY, 1234);
    if (rc != ERR_OK) {
        KLOG_ERROR("[tcp-echo] tcp_bind failed rc=%d\n", rc);
        tcp_close(pcb);
        return;
    }

    struct tcp_pcb *listen_pcb = tcp_listen(pcb);
    if (!listen_pcb) {
        KLOG_ERROR("[tcp-echo] tcp_listen failed\n");
        tcp_close(pcb);
        return;
    }

    tcp_accept(listen_pcb, echo_accept);
    KLOG_INFO("[tcp-echo] listening on 0.0.0.0:1234\n");
}
