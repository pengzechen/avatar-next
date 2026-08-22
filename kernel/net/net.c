#include "net/net.h"

#include "klog.h"
#include "net/dhcp_server.h"
#include "net/tcp_echo.h"
#include "task/task.h"

#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"

#include "netif/ethernet.h"
#include "lwip_port/netif_avatar.h"

#include "net/http_server.h"

static struct netif g_lwip_netif;
static int g_net_ready;

void net_init(void)
{
    ip4_addr_t ipaddr;
    ip4_addr_t netmask;
    ip4_addr_t gw;
    ip4_addr_t lease;

    IP4_ADDR(&ipaddr, 192, 168, 7, 1);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 192, 168, 7, 1);
    IP4_ADDR(&lease, 192, 168, 7, 2);

    lwip_init();

    if (!netif_add(&g_lwip_netif, &ipaddr, &netmask, &gw, NULL,
                   avatar_netif_init, ethernet_input)) {
        KLOG_ERROR("[net] netif_add failed\n");
        return;
    }

    netif_set_default(&g_lwip_netif);
    netif_set_up(&g_lwip_netif);
    netif_set_link_up(&g_lwip_netif);
    g_net_ready = 1;

    KLOG_INFO("[net] IPv4 addr=192.168.7.1 mask=255.255.255.0 gw=192.168.7.1\n");
    dhcp_server_init(&ipaddr, &netmask, &lease);
    tcp_echo_init();
#if defined(RUN_NGINX_TEST)
    KLOG_INFO("[http] kernel HTTP server disabled for nginx test\n");
#else
    http_server_init();
#endif
}

void net_poll_once(void)
{
    if (!g_net_ready)
        return;

    avatar_lwip_poll_rx(&g_lwip_netif);
    sys_check_timeouts();
}

void net_poll_task(void *arg)
{
    static unsigned poll_count;

    (void)arg;

    if (!g_net_ready) {
        KLOG_WARN("[net] poll task started before net_init\n");
        task_exit();
    }

    KLOG_INFO("[net] poll task started\n");
    for (;;) {
        poll_count++;
        if (poll_count <= 8U || (poll_count & (poll_count - 1U)) == 0U)
            KLOG_INFO("[net] poll alive #%u\n", poll_count);
        net_poll_once();
        task_yield();
    }
}
