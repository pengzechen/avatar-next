#include "net/net.h"

#include "klog.h"
#include "net/tcp_echo.h"
#include "task/task.h"

#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"

#include "netif/ethernet.h"
#include "lwip_port/netif_avatar.h"

#if DRIVER_USB_DWC2
#include "net/webcam_httpd.h"
#endif

static struct netif g_lwip_netif;
static int g_net_ready;

void net_init(void)
{
    ip4_addr_t ipaddr;
    ip4_addr_t netmask;
    ip4_addr_t gw;

    IP4_ADDR(&ipaddr, 192, 168, 100, 2);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 192, 168, 100, 1);

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

    KLOG_INFO("[net] IPv4 addr=192.168.100.2 mask=255.255.255.0 gw=192.168.100.1\n");
    tcp_echo_init();
#if DRIVER_USB_DWC2
    webcam_httpd_init();
#endif
}

void net_poll_task(void *arg)
{
    (void)arg;

    if (!g_net_ready) {
        KLOG_WARN("[net] poll task started before net_init\n");
        task_exit();
    }

    KLOG_INFO("[net] poll task started\n");
    for (;;) {
        avatar_lwip_poll_rx(&g_lwip_netif);
        sys_check_timeouts();
        task_yield();
    }
}
