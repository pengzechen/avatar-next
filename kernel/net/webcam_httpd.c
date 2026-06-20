#include "net/webcam_httpd.h"

#include "klog.h"
#include "string.h"
#include "arg.h"
#include "usb/uvc_video.h"
#include "timer/timer.h"

#include "lwip/tcp.h"

#define WEBCAM_PORT       80
#define FRAME_BUF_SIZE    (512U * 1024U)
#define MIN_CAPTURE_MS    200

extern int my_vsnprintf(char *buf, int size, const char *fmt, va_list va);

static int snfmt(char *buf, int size, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int n = my_vsnprintf(buf, size, fmt, va);
    va_end(va);
    return n;
}

static uint8_t g_frame_buf[FRAME_BUF_SIZE];
static uint32_t g_cached_len;
static uint64_t g_last_capture_ms;
static uint32_t g_fail_streak;

#define MAX_FAIL_STREAK 3

static int webcam_grab_frame(void)
{
    uint64_t now = timer_get_uptime_ms();
    if (g_cached_len > 0 && (now - g_last_capture_ms) < MIN_CAPTURE_MS)
        return (int)g_cached_len;

    if (g_fail_streak >= MAX_FAIL_STREAK) {
        if (g_cached_len > 0)
            return (int)g_cached_len;
        return -5;
    }

    int n = uvc_video_read_frame(g_frame_buf, FRAME_BUF_SIZE);
    g_last_capture_ms = now;
    if (n > 0) {
        g_cached_len = (uint32_t)n;
        g_fail_streak = 0;
    } else {
        g_fail_streak++;
        if (g_fail_streak >= MAX_FAIL_STREAK)
            KLOG_WARN("[webcam] USB capture failed %u times, serving cached frame\n",
                      g_fail_streak);
    }
    if (n <= 0 && g_cached_len > 0)
        return (int)g_cached_len;
    return n;
}

/* ── HTML page ─────────────────────────────────────────────────────────── */

static const char INDEX_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<title>Avatar OS WebCam</title>"
    "<style>"
    "body{background:#111;color:#eee;font-family:monospace;text-align:center;margin:2em}"
    "img{max-width:100%;border:2px solid #444;border-radius:8px}"
    "h1{color:#0f0}"
    "</style></head><body>"
    "<h1>Avatar OS &mdash; WebCam</h1>"
    "<img id=\"cam\">"
    "<p id=\"st\">loading...</p>"
    "<script>"
    "var img=document.getElementById('cam'),"
        "st=document.getElementById('st'),"
        "n=0;"
    "function grab(){"
        "var t=new Image();"
        "t.onload=function(){"
            "img.src=t.src;n++;"
            "st.textContent='frame #'+n+' '+t.naturalWidth+'x'+t.naturalHeight;"
            "setTimeout(grab,100);"
        "};"
        "t.onerror=function(){st.textContent='error';setTimeout(grab,1000);};"
        "t.src='/frame.jpg?'+Date.now();"
    "}"
    "grab();"
    "</script></body></html>";

/* ── Per-connection state ──────────────────────────────────────────────── */

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
} webcam_conn_t;

/* ── Forward declarations ──────────────────────────────────────────────── */

static void conn_try_send(struct tcp_pcb *tpcb, webcam_conn_t *c);
static void conn_close(struct tcp_pcb *tpcb, webcam_conn_t *c);
static void conn_free(webcam_conn_t *c);

/* ── Request parsing ───────────────────────────────────────────────────── */

static int match_path(const char *req, uint16_t len,
                      const char *path, size_t plen)
{
    const char *p = req;
    const char *end = req + len;

    if (end - p < 4 || p[0] != 'G' || p[1] != 'E' || p[2] != 'T' || p[3] != ' ')
        return 0;
    p += 4;
    if ((size_t)(end - p) < plen)
        return 0;
    return memcmp(p, path, plen) == 0 &&
           (p[plen] == ' ' || p[plen] == '?' || p[plen] == '\r');
}

static int build_response(webcam_conn_t *c, const char *req, uint16_t len)
{
    if (match_path(req, len, "/frame.jpg", 10)) {
        int n = webcam_grab_frame();
        if (n <= 0) {
            const char *err = "HTTP/1.0 503 Service Unavailable\r\n"
                              "Content-Length: 0\r\n"
                              "Connection: close\r\n\r\n";
            size_t elen = strlen(err);
            memcpy(c->hdr_buf, err, elen);
            c->hdr_len = (uint16_t)elen;
            c->hdr_sent = 0;
            c->body = NULL;
            c->body_len = 0;
            c->body_sent = 0;
            c->state = CS_SEND_HEADER;
            return 0;
        }
        c->body = g_frame_buf;
        c->body_len = (uint32_t)n;
        c->body_sent = 0;
        int hlen = snfmt(c->hdr_buf, sizeof(c->hdr_buf),
            "HTTP/1.0 200 OK\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %d\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: close\r\n\r\n", n);
        c->hdr_len = (uint16_t)hlen;
        c->hdr_sent = 0;
        c->state = CS_SEND_HEADER;
        return 0;
    }

    c->body = (const uint8_t *)INDEX_HTML;
    c->body_len = (uint32_t)(sizeof(INDEX_HTML) - 1);
    c->body_sent = 0;
    int hlen = snfmt(c->hdr_buf, sizeof(c->hdr_buf),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        (unsigned)(sizeof(INDEX_HTML) - 1));
    c->hdr_len = (uint16_t)hlen;
    c->hdr_sent = 0;
    c->state = CS_SEND_HEADER;
    return 0;
}

/* ── TCP send engine ───────────────────────────────────────────────────── */

static void conn_try_send(struct tcp_pcb *tpcb, webcam_conn_t *c)
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
        if (c->hdr_sent >= c->hdr_len) {
            if (c->body_len == 0) {
                c->state = CS_DONE;
                tcp_output(tpcb);
                conn_close(tpcb, c);
                return;
            }
            c->state = CS_SEND_BODY;
        }
    }

    if (c->state == CS_SEND_BODY) {
        while (c->body_sent < c->body_len) {
            uint16_t space = tcp_sndbuf(tpcb);
            if (space == 0)
                break;
            uint32_t remain = c->body_len - c->body_sent;
            uint16_t chunk = (remain > space) ? space : (uint16_t)remain;
            uint8_t flags = TCP_WRITE_FLAG_COPY;
            rc = tcp_write(tpcb, c->body + c->body_sent, chunk, flags);
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

/* ── TCP callbacks ─────────────────────────────────────────────────────── */

static void conn_close(struct tcp_pcb *tpcb, webcam_conn_t *c)
{
    tcp_arg(tpcb, NULL);
    tcp_recv(tpcb, NULL);
    tcp_sent(tpcb, NULL);
    tcp_err(tpcb, NULL);
    conn_free(c);
    tcp_close(tpcb);
}

static err_t webcam_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    (void)len;
    webcam_conn_t *c = (webcam_conn_t *)arg;
    if (!c)
        return ERR_OK;
    conn_try_send(tpcb, c);
    return ERR_OK;
}

static err_t webcam_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p,
                         err_t err)
{
    webcam_conn_t *c = (webcam_conn_t *)arg;

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
    build_response(c, (const char *)p->payload, p->tot_len);
    pbuf_free(p);
    conn_try_send(tpcb, c);
    return ERR_OK;
}

static void webcam_err(void *arg, err_t err)
{
    (void)err;
    conn_free((webcam_conn_t *)arg);
}

/* Using a small pool of connection structs to avoid dynamic allocation */
#define MAX_CONNS 4
static webcam_conn_t g_conns[MAX_CONNS];
static uint8_t g_conn_used[MAX_CONNS];

static webcam_conn_t *conn_alloc(void)
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

static void conn_free(webcam_conn_t *c)
{
    if (!c)
        return;
    int idx = (int)(c - g_conns);
    if (idx >= 0 && idx < MAX_CONNS)
        g_conn_used[idx] = 0;
}

static err_t webcam_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    (void)arg;
    if (err != ERR_OK || !newpcb)
        return ERR_VAL;

    webcam_conn_t *c = conn_alloc();
    if (!c) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    tcp_arg(newpcb, c);
    tcp_recv(newpcb, webcam_recv);
    tcp_sent(newpcb, webcam_sent);
    tcp_err(newpcb, webcam_err);
    return ERR_OK;
}

/* ── Public API ────────────────────────────────────────────────────────── */

void webcam_httpd_init(void)
{
    memset(g_conn_used, 0, sizeof(g_conn_used));

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (!pcb) {
        KLOG_ERROR("[webcam] tcp_new failed\n");
        return;
    }

    err_t rc = tcp_bind(pcb, IP_ADDR_ANY, WEBCAM_PORT);
    if (rc != ERR_OK) {
        KLOG_ERROR("[webcam] tcp_bind port %u failed rc=%d\n", WEBCAM_PORT, rc);
        tcp_close(pcb);
        return;
    }

    struct tcp_pcb *lpcb = tcp_listen(pcb);
    if (!lpcb) {
        KLOG_ERROR("[webcam] tcp_listen failed\n");
        tcp_close(pcb);
        return;
    }

    tcp_accept(lpcb, webcam_accept);
    KLOG_INFO("[webcam] listening on 0.0.0.0:%u\n", WEBCAM_PORT);
}
