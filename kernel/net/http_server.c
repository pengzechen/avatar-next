#include "net/http_server.h"

#include "klog.h"
#include "string.h"
#include "arg.h"

#include "lwip/tcp.h"

#define HTTP_PORT 80

extern int my_vsnprintf(char *buf, int size, const char *fmt, va_list va);

static int snfmt(char *buf, int size, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int n = my_vsnprintf(buf, size, fmt, va);
    va_end(va);
    return n;
}

static const char INDEX_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Avatar OS</title>"
    "<style>"
    "*{margin:0;padding:0;box-sizing:border-box}"
    "body{background:#0a0a0a;color:#e0e0e0;font-family:'Courier New',monospace;"
    "display:flex;justify-content:center;align-items:center;min-height:100vh}"
    ".c{text-align:center;padding:2em}"
    "h1{font-size:3em;color:#00ff88;text-shadow:0 0 20px #00ff8844;margin-bottom:.2em}"
    ".sub{color:#888;font-size:1.1em;margin-bottom:2em}"
    ".box{background:#141414;border:1px solid #333;border-radius:12px;"
    "padding:2em;max-width:480px;margin:0 auto}"
    ".row{display:flex;justify-content:space-between;padding:.5em 0;"
    "border-bottom:1px solid #222}"
    ".row:last-child{border:none}"
    ".k{color:#888}.v{color:#00cc66}"
    ".foot{margin-top:2em;color:#555;font-size:.85em}"
    "</style></head><body>"
    "<div class=\"c\">"
    "<h1>&gt;_ Avatar OS</h1>"
    "<p class=\"sub\">A 64-bit Operating System Kernel</p>"
    "<div class=\"box\">"
    "<div class=\"row\"><span class=\"k\">Status</span>"
    "<span class=\"v\">Running</span></div>"
    "<div class=\"row\"><span class=\"k\">Architecture</span>"
#if defined(__aarch64__)
    "<span class=\"v\">AArch64</span></div>"
#elif defined(__riscv)
    "<span class=\"v\">RISC-V 64</span></div>"
#elif defined(__x86_64__)
    "<span class=\"v\">x86_64</span></div>"
#else
    "<span class=\"v\">Unknown</span></div>"
#endif
    "<div class=\"row\"><span class=\"k\">Network</span>"
    "<span class=\"v\">lwIP / virtio-net</span></div>"
    "<div class=\"row\"><span class=\"k\">TCP Echo</span>"
    "<span class=\"v\">Port 1234</span></div>"
    "<div class=\"row\"><span class=\"k\">HTTP</span>"
    "<span class=\"v\">Port 80</span></div>"
    "</div>"
    "<p class=\"foot\">Served directly from the kernel &middot; No userspace</p>"
    "</div></body></html>";

/* ── Per-connection state ─────────────────────────────────────────────── */

typedef enum {
    CS_RECV_REQUEST,
    CS_SEND_HEADER,
    CS_SEND_BODY,
    CS_DONE,
} conn_state_t;

typedef struct {
    conn_state_t state;
    const uint8_t *body;
    uint32_t body_len;
    uint32_t body_sent;
    char hdr_buf[256];
    uint16_t hdr_len;
    uint16_t hdr_sent;
} http_conn_t;

static void conn_try_send(struct tcp_pcb *tpcb, http_conn_t *c);
static void conn_close(struct tcp_pcb *tpcb, http_conn_t *c);
static void conn_free(http_conn_t *c);

/* ── Build HTTP response ──────────────────────────────────────────────── */

static void build_response(http_conn_t *c)
{
    c->body     = (const uint8_t *)INDEX_HTML;
    c->body_len = (uint32_t)(sizeof(INDEX_HTML) - 1);
    c->body_sent = 0;
    int hlen = snfmt(c->hdr_buf, sizeof(c->hdr_buf),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        (unsigned)(sizeof(INDEX_HTML) - 1));
    c->hdr_len  = (uint16_t)hlen;
    c->hdr_sent = 0;
    c->state    = CS_SEND_HEADER;
}

/* ── TCP send engine ──────────────────────────────────────────────────── */

static void conn_try_send(struct tcp_pcb *tpcb, http_conn_t *c)
{
    err_t rc;

    if (c->state == CS_SEND_HEADER) {
        uint16_t remain = c->hdr_len - c->hdr_sent;
        if (remain > 0) {
            uint16_t space = tcp_sndbuf(tpcb);
            uint16_t chunk = remain < space ? remain : space;
            if (chunk == 0)
                return;
            rc = tcp_write(tpcb, c->hdr_buf + c->hdr_sent, chunk,
                           TCP_WRITE_FLAG_COPY);
            if (rc != ERR_OK)
                return;
            c->hdr_sent += chunk;
        }
        if (c->hdr_sent >= c->hdr_len)
            c->state = CS_SEND_BODY;
    }

    if (c->state == CS_SEND_BODY) {
        while (c->body_sent < c->body_len) {
            uint16_t space = tcp_sndbuf(tpcb);
            if (space == 0)
                break;
            uint32_t remain = c->body_len - c->body_sent;
            uint16_t chunk = (remain > space) ? space : (uint16_t)remain;
            rc = tcp_write(tpcb, c->body + c->body_sent, chunk,
                           TCP_WRITE_FLAG_COPY);
            if (rc != ERR_OK)
                break;
            c->body_sent += chunk;
        }
        tcp_output(tpcb);
        if (c->body_sent >= c->body_len) {
            c->state = CS_DONE;
            conn_close(tpcb, c);
        }
    }
}

/* ── TCP callbacks ────────────────────────────────────────────────────── */

static void conn_close(struct tcp_pcb *tpcb, http_conn_t *c)
{
    tcp_arg(tpcb, NULL);
    tcp_recv(tpcb, NULL);
    tcp_sent(tpcb, NULL);
    tcp_err(tpcb, NULL);
    conn_free(c);
    tcp_close(tpcb);
}

static err_t http_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    (void)len;
    http_conn_t *c = (http_conn_t *)arg;
    if (!c)
        return ERR_OK;
    conn_try_send(tpcb, c);
    return ERR_OK;
}

static err_t http_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p,
                       err_t err)
{
    http_conn_t *c = (http_conn_t *)arg;

    if (err != ERR_OK || !p || !c) {
        if (p)
            pbuf_free(p);
        if (c)
            conn_close(tpcb, c);
        return ERR_OK;
    }

    if (c->state != CS_RECV_REQUEST) {
        tcp_recved(tpcb, p->tot_len);
        pbuf_free(p);
        return ERR_OK;
    }

    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    build_response(c);
    conn_try_send(tpcb, c);
    return ERR_OK;
}

static void http_err(void *arg, err_t err)
{
    (void)err;
    conn_free((http_conn_t *)arg);
}

/* ── Connection pool ──────────────────────────────────────────────────── */

#define MAX_CONNS 4
static http_conn_t g_conns[MAX_CONNS];
static uint8_t     g_conn_used[MAX_CONNS];

static http_conn_t *conn_alloc(void)
{
    for (int i = 0; i < MAX_CONNS; i++) {
        if (!g_conn_used[i]) {
            g_conn_used[i] = 1;
            memset(&g_conns[i], 0, sizeof(g_conns[i]));
            g_conns[i].state = CS_RECV_REQUEST;
            return &g_conns[i];
        }
    }
    return NULL;
}

static void conn_free(http_conn_t *c)
{
    if (!c)
        return;
    int idx = (int)(c - g_conns);
    if (idx >= 0 && idx < MAX_CONNS)
        g_conn_used[idx] = 0;
}

static err_t http_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !newpcb)
        return ERR_VAL;

    http_conn_t *c = conn_alloc();
    if (!c) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    tcp_arg(newpcb, c);
    tcp_recv(newpcb, http_recv);
    tcp_sent(newpcb, http_sent);
    tcp_err(newpcb, http_err);
    return ERR_OK;
}

/* ── Public API ───────────────────────────────────────────────────────── */

void http_server_init(void)
{
    memset(g_conn_used, 0, sizeof(g_conn_used));

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) {
        KLOG_ERROR("[http] tcp_new failed\n");
        return;
    }

    err_t rc = tcp_bind(pcb, IP_ADDR_ANY, HTTP_PORT);
    if (rc != ERR_OK) {
        KLOG_ERROR("[http] tcp_bind port %u failed rc=%d\n", HTTP_PORT, rc);
        tcp_close(pcb);
        return;
    }

    struct tcp_pcb *lpcb = tcp_listen(pcb);
    if (!lpcb) {
        KLOG_ERROR("[http] tcp_listen failed\n");
        tcp_close(pcb);
        return;
    }

    tcp_accept(lpcb, http_accept);
    KLOG_INFO("[http] listening on 0.0.0.0:%u\n", HTTP_PORT);
}
