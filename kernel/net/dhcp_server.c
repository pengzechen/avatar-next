#include "net/dhcp_server.h"

#include "klog.h"
#include "string.h"

#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define DHCP_OP_BOOTREQUEST 1
#define DHCP_OP_BOOTREPLY   2
#define DHCP_HTYPE_ETHERNET 1
#define DHCP_HLEN_ETHERNET  6

#define DHCP_MAGIC_COOKIE 0x63825363UL

#define DHCP_OPT_PAD         0
#define DHCP_OPT_SUBNET_MASK 1
#define DHCP_OPT_ROUTER      3
#define DHCP_OPT_DNS         6
#define DHCP_OPT_REQ_IP      50
#define DHCP_OPT_MSG_TYPE    53
#define DHCP_OPT_SERVER_ID   54
#define DHCP_OPT_LEASE_TIME  51
#define DHCP_OPT_RENEWAL     58
#define DHCP_OPT_REBINDING   59
#define DHCP_OPT_END         255

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

#define DHCP_MIN_PACKET_LEN 240
#define DHCP_FIXED_LEN      240
#define DHCP_REPLY_LEN      300
#define DHCP_LEASE_SECONDS  3600UL

static struct udp_pcb *g_dhcp_pcb;
static ip4_addr_t g_server_ip;
static ip4_addr_t g_netmask;
static ip4_addr_t g_lease_ip;

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t ip4_to_be32(const ip4_addr_t *ip)
{
    uint32_t v = ip4_addr_get_u32(ip);
    return ((v & 0x000000ffUL) << 24) |
           ((v & 0x0000ff00UL) << 8) |
           ((v & 0x00ff0000UL) >> 8) |
           ((v & 0xff000000UL) >> 24);
}

static void put_ip(uint8_t *p, const ip4_addr_t *ip)
{
    put_be32(p, ip4_to_be32(ip));
}

static void add_opt_u8(uint8_t *buf, size_t *off, uint8_t opt, uint8_t val)
{
    buf[(*off)++] = opt;
    buf[(*off)++] = 1;
    buf[(*off)++] = val;
}

static void add_opt_ip(uint8_t *buf, size_t *off, uint8_t opt, const ip4_addr_t *ip)
{
    buf[(*off)++] = opt;
    buf[(*off)++] = 4;
    put_ip(&buf[*off], ip);
    *off += 4;
}

static void add_opt_u32(uint8_t *buf, size_t *off, uint8_t opt, uint32_t val)
{
    buf[(*off)++] = opt;
    buf[(*off)++] = 4;
    put_be32(&buf[*off], val);
    *off += 4;
}

static uint8_t dhcp_get_msg_type(const uint8_t *pkt, size_t len)
{
    size_t off = DHCP_FIXED_LEN;

    while (off < len) {
        uint8_t opt = pkt[off++];
        if (opt == DHCP_OPT_PAD)
            continue;
        if (opt == DHCP_OPT_END)
            break;
        if (off >= len)
            break;
        uint8_t opt_len = pkt[off++];
        if (off + opt_len > len)
            break;
        if (opt == DHCP_OPT_MSG_TYPE && opt_len == 1)
            return pkt[off];
        off += opt_len;
    }

    return 0;
}

static void dhcp_send_reply(const uint8_t *req, size_t req_len, uint8_t msg_type)
{
    uint8_t reply[DHCP_REPLY_LEN];
    memset(reply, 0, sizeof(reply));

    reply[0] = DHCP_OP_BOOTREPLY;
    reply[1] = DHCP_HTYPE_ETHERNET;
    reply[2] = DHCP_HLEN_ETHERNET;
    reply[3] = 0;
    memcpy(&reply[4], &req[4], 4);      /* xid */
    memcpy(&reply[10], &req[10], 2);    /* flags */
    put_ip(&reply[16], &g_lease_ip);    /* yiaddr */
    put_ip(&reply[20], &g_server_ip);   /* siaddr */
    memcpy(&reply[28], &req[28], 16);   /* chaddr */
    put_be32(&reply[236], DHCP_MAGIC_COOKIE);

    size_t off = DHCP_FIXED_LEN;
    add_opt_u8(reply, &off, DHCP_OPT_MSG_TYPE, msg_type);
    add_opt_ip(reply, &off, DHCP_OPT_SERVER_ID, &g_server_ip);
    add_opt_u32(reply, &off, DHCP_OPT_LEASE_TIME, DHCP_LEASE_SECONDS);
    add_opt_ip(reply, &off, DHCP_OPT_SUBNET_MASK, &g_netmask);
    add_opt_ip(reply, &off, DHCP_OPT_ROUTER, &g_server_ip);
    add_opt_ip(reply, &off, DHCP_OPT_DNS, &g_server_ip);
    add_opt_u32(reply, &off, DHCP_OPT_RENEWAL, DHCP_LEASE_SECONDS / 2);
    add_opt_u32(reply, &off, DHCP_OPT_REBINDING, (DHCP_LEASE_SECONDS * 7) / 8);
    reply[off++] = DHCP_OPT_END;

    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)off, PBUF_RAM);
    if (!p) {
        KLOG_WARN("[dhcpd] no pbuf for reply\n");
        return;
    }
    if (pbuf_take(p, reply, (u16_t)off) != ERR_OK) {
        pbuf_free(p);
        KLOG_WARN("[dhcpd] pbuf_take failed\n");
        return;
    }

    ip_addr_t dst;
    ip_addr_set_ip4_u32(&dst, PP_HTONL(0xffffffffUL));
    err_t err = udp_sendto(g_dhcp_pcb, p, &dst, DHCP_CLIENT_PORT);
    pbuf_free(p);

    if (err == ERR_OK) {
        KLOG_INFO("[dhcpd] sent %s lease=192.168.7.2\n",
                  msg_type == DHCP_OFFER ? "OFFER" : "ACK");
    } else {
        KLOG_WARN("[dhcpd] send reply failed err=%d\n", err);
    }

    (void)req_len;
}

static void dhcp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                      const ip_addr_t *addr, u16_t port)
{
    (void)arg;
    (void)pcb;
    (void)addr;
    (void)port;

    if (!p)
        return;

    uint8_t pkt[576];
    size_t len = p->tot_len;
    if (len > sizeof(pkt))
        len = sizeof(pkt);
    pbuf_copy_partial(p, pkt, (u16_t)len, 0);
    pbuf_free(p);

    if (len < DHCP_MIN_PACKET_LEN || pkt[0] != DHCP_OP_BOOTREQUEST ||
        pkt[1] != DHCP_HTYPE_ETHERNET || pkt[2] != DHCP_HLEN_ETHERNET)
        return;

    uint32_t magic = ((uint32_t)pkt[236] << 24) |
                     ((uint32_t)pkt[237] << 16) |
                     ((uint32_t)pkt[238] << 8) |
                     (uint32_t)pkt[239];
    if (magic != DHCP_MAGIC_COOKIE)
        return;

    uint8_t type = dhcp_get_msg_type(pkt, len);
    if (type == DHCP_DISCOVER) {
        KLOG_INFO("[dhcpd] DISCOVER from %02x:%02x:%02x:%02x:%02x:%02x\n",
                  pkt[28], pkt[29], pkt[30], pkt[31], pkt[32], pkt[33]);
        dhcp_send_reply(pkt, len, DHCP_OFFER);
    } else if (type == DHCP_REQUEST) {
        KLOG_INFO("[dhcpd] REQUEST from %02x:%02x:%02x:%02x:%02x:%02x\n",
                  pkt[28], pkt[29], pkt[30], pkt[31], pkt[32], pkt[33]);
        dhcp_send_reply(pkt, len, DHCP_ACK);
    }
}

void dhcp_server_init(const ip4_addr_t *server_ip,
                      const ip4_addr_t *netmask,
                      const ip4_addr_t *lease_ip)
{
    if (g_dhcp_pcb)
        return;

    ip4_addr_copy(g_server_ip, *server_ip);
    ip4_addr_copy(g_netmask, *netmask);
    ip4_addr_copy(g_lease_ip, *lease_ip);

    g_dhcp_pcb = udp_new();
    if (!g_dhcp_pcb) {
        KLOG_ERROR("[dhcpd] udp_new failed\n");
        return;
    }

    err_t err = udp_bind(g_dhcp_pcb, IP_ADDR_ANY, DHCP_SERVER_PORT);
    if (err != ERR_OK) {
        KLOG_ERROR("[dhcpd] udp_bind failed err=%d\n", err);
        udp_remove(g_dhcp_pcb);
        g_dhcp_pcb = NULL;
        return;
    }

    udp_recv(g_dhcp_pcb, dhcp_recv, NULL);
    KLOG_INFO("[dhcpd] started: server=192.168.7.1 lease=192.168.7.2\n");
}
