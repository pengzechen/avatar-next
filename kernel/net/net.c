#include "net/net.h"

#include "klog.h"
#include "net/bwtest.h"
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

    /*
     * 收包中断的唤醒回调**暂不注册**（原因见上面"收包唤醒链路（暂缓）"）：
     * 将来这里要挂的是"唤醒 net-poll 任务"的函数，即
     *     netdev_set_rx_wakeup(<唤醒函数>);
     * 现在 net-poll 走协作式让出、始终在就绪队列里，不需要谁来唤醒；
     * 中断本身仍然使能（cvitek_eth_init 里），只做去断言。
     */

    KLOG_INFO("[net] IPv4 addr=192.168.7.1 mask=255.255.255.0 gw=192.168.7.1\n");
    dhcp_server_init(&ipaddr, &netmask, &lease);
    tcp_echo_init();
    bwtest_init();
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
    bwtest_poll();
}

/* ── 收包唤醒链路（暂缓）──────────────────────────────────────────────── */

/*
 * 目标形态是：net-poll 空闲时 task_block() 阻塞，由收包中断唤醒。这样它在
 * 没有流量时不烧 CPU，也就不会在 g++ 编译这类 CPU 饱和场景下跟别的任务抢
 * 时间片、进而让 RX 环溢出丢包。
 *
 * **这条路目前走不通，已退回协作式让出（见 net_poll_task）。**
 * 原因在内核的异常返回路径，不在网络层：
 *
 *   boot/riscv64/exception.S 在 sched_check_and_yield_from_trap() 返回后，
 *   仍然假定 sp 指向进入 trap 时构造的那个帧：
 *
 *       call sched_check_and_yield_from_trap   ← 这里可能已经切换了任务
 *       ld   t0, 32*8(sp) / csrw sepc, t0
 *       ld   t0, 35*8(sp) / csrw sstatus, t0   ← 仍按旧假设从栈上读
 *
 *   被中断抢占的任务满足这个假设（它的 sp 本就是自己 trap frame 的基址），
 *   所以一直没暴露。但在任务上下文里通过 task_block() 睡下的任务不满足 ——
 *   它的 sp 指向内核栈中间的 sched_schedule 调用帧，那里没有 trap frame。
 *   被唤醒切过去时会从栈上读到垃圾并写进 sstatus，结果中断从此不再投递。
 *
 * 实测（SG2002，LOG=none）：改成阻塞之后，tick 回调打在 10/20/30/40ms 后
 * 再无输出（定时器停摆），串口失去响应、ping 不通；而 net-poll 的循环计数
 * 停在它阻塞的那一刻 —— 任务被解阻塞了，但再没被调度起来。把阻塞改回
 * task_yield() 后全部恢复：收包 95 Mbps、零丢包。
 *
 * 要支持这个模式，得先让异常返回路径在调度切换之后重新取得当前任务的帧指针
 * （例如把 frame 指针存进 per-CPU 变量、由调度器在切换时更新）。
 * 在那之前，netdev 层的唤醒钩子（include/net/netdev.h 的
 * netdev_set_rx_wakeup / netdev_rx_wakeup）保持未注册：驱动的收包中断只做
 * 去断言，不唤醒任何人。
 */

void net_poll_task(void *arg)
{
    (void)arg;

    if (!g_net_ready) {
        KLOG_WARN("[net] poll task started before net_init\n");
        task_exit();
    }

    KLOG_INFO("[net] poll task started\n");

    for (;;) {
        net_poll_once();

        /*
         * 回到协作式让出。
         *
         * ⚠️ 不能改成 task_block() 阻塞 + 由中断唤醒：本内核的异常返回路径
         * （boot/riscv64/exception.S）在 sched_check_and_yield_from_trap()
         * 返回后**仍然假定 sp 指向本次 trap 的帧**：
         *     call sched_check_and_yield_from_trap   ← 这里可能已切换任务
         *     ld t0, 35*8(sp) / csrw sstatus, t0     ← 按旧假设从栈上读
         * 被抢占的任务满足这个假设（它的 sp 本就是自己的 trap frame），
         * 但在任务上下文里通过 task_block() 睡下的任务，sp 指向内核栈中间的
         * sched_schedule 调用帧 —— 被唤醒切过去时会从那里读到垃圾并写进
         * sstatus，结果中断从此不再投递（实测：定时器几毫秒内停摆）。
         *
         * 要支持"中断唤醒阻塞任务"，得先让异常返回路径在切换后重新取得正确
         * 的帧指针。在那之前，net-poll 只能继续协程式让出。
         */
        task_yield();
    }
}
