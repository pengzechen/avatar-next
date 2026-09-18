/*
 * kernel/net/bwtest.c — 以太网带宽测试：UDP 丢弃式接收端 + 每秒速率统计
 *
 * ── 为什么用 UDP 而不是 TCP ──────────────────────────────────────────────
 * kernel/net/lwip_port/lwipopts.h 里
 *     TCP_WND     = 4 * TCP_MSS = 5840 字节
 *     TCP_SND_BUF = 4 * TCP_MSS = 5840 字节
 * 收发窗口都只有 5840 字节，TCP 吞吐被"在途字节数 / RTT"卡死在几 MB/s 量级 ——
 * 拿 TCP 流去测，量到的是窗口大小，不是以太网和驱动本身的能力。
 * UDP 没有窗口也没有 ACK 往返，才能真正把线打满，暴露 RX 环深度、
 * 轮询节奏、拷贝开销这些真实瓶颈。
 *
 * ── 为什么直接调 kprintf 而不走 KLOG_* ───────────────────────────────────
 * 测性能必须用 LOG=none 构建：收包热路径上的 KLOG（kernel/net/netdev.c 与
 * kernel/net/lwip_port/netif_avatar.c 每 32 帧各打一条）走的是 SG2002 的
 * 同步串口路径 driver/uart/uart_dw.c 的 sg2002_dbg_putc()，逐字节 busy-wait，
 * 每 32 帧要阻塞 ~7.6ms，而同样 32 帧在线上只要 3.9ms —— 日志本身就是瓶颈。
 * 但 LOG=none 会把 KLOG_* 编译期抹成 do{}while(0)，所以本模块的统计输出
 * 自己调 kprintf（include/klog.h），这样 LOG=none 构建里依然可见。
 *
 * ── 报文格式 ────────────────────────────────────────────────────────────
 * 每条 UDP datagram 开头 8 字节头，其后是任意填充（内容不解析）：
 *     offset 0..3: magic = 0x41564257 ("AVBW")，小端
 *     offset 4..7: seq   从 0 递增的序号，小端
 * 带 magic 是为了不把链路上的杂包（ARP / IPv6 / DHCP 广播）混进统计。
 *
 * PC 侧（PowerShell，无需装任何软件）：
 *
 *     $ip='192.168.7.1'; $port=1235; $n=50000; $size=1400
 *     $c = New-Object Net.Sockets.UdpClient
 *     $c.Connect($ip, $port)
 *     $buf = New-Object byte[] $size
 *     [BitConverter]::GetBytes([uint32]0x41564257).CopyTo($buf, 0)
 *     $sw = [Diagnostics.Stopwatch]::StartNew()
 *     for ($i=0; $i -lt $n; $i++) {
 *         [BitConverter]::GetBytes([uint32]$i).CopyTo($buf, 4)
 *         [void]$c.Send($buf, $size)
 *     }
 *     $sw.Stop()
 *     $mb = $n * $size / 1MB
 *     "发送 {0:N1} MB / {1:N2} s = {2:N2} MB/s ({3:N1} Mbps)" -f `
 *         $mb, $sw.Elapsed.TotalSeconds, ($mb/$sw.Elapsed.TotalSeconds), `
 *         ($mb*8/$sw.Elapsed.TotalSeconds)
 *     $c.Close()
 *
 * 注意：Windows 每发送一条就要做一次 sendto 系统调用，1400 字节/包时
 * 100 Mbps 只需要约 9000 包/秒，PowerShell 的循环开销远够用，不会成为瓶颈。
 */

#include "net/bwtest.h"

#include "klog.h"        /* kprintf */
#include "string.h"
#include "timer/timer.h"

#include "lwip/udp.h"

/* ── 配置 ──────────────────────────────────────────────────────────────── */

#define BW_PORT            1235U          /* 与 TCP echo(1234)/HTTP(80) 不冲突 */
#define BW_MAGIC           0x41564257U    /* "AVBW"，小端 */
#define BW_HDR_LEN         8U
#define BW_REPORT_MS       1000U          /* 统计窗口 */
#define BW_MAX_IDLE_WINDOWS 3U            /* 连续几个空窗口后停止打印 */

/* ── 状态 ──────────────────────────────────────────────────────────────── */

static struct udp_pcb *g_pcb;

/* 当前窗口 */
static uint32_t g_win_pkts;
static uint64_t g_win_bytes;
static uint32_t g_win_first_seq;
static uint32_t g_win_max_seq;
static uint32_t g_win_ooo;        /* 序号不大于已见最大值的到达（乱序或重复） */

/* 累计 */
static uint64_t g_total_pkts;
static uint64_t g_total_bytes;
static uint64_t g_total_lost;

static uint64_t g_win_start_ms;
static bool     g_running;        /* 收到过有效包 → 开始统计 */
static uint32_t g_idle_windows;

/* ── 工具 ──────────────────────────────────────────────────────────────── */

static inline uint32_t bw_rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void bw_reset_window(uint64_t now_ms)
{
    g_win_pkts      = 0;
    g_win_bytes     = 0;
    g_win_first_seq = 0;
    g_win_max_seq   = 0;
    g_win_ooo       = 0;
    g_win_start_ms  = now_ms;
}

/* ── 统计输出 ──────────────────────────────────────────────────────────── */

static void bw_report(uint32_t window_ms)
{
    if (window_ms == 0U)
        window_ms = 1U;

    uint32_t lost = 0U;
    if (g_win_pkts > 0U) {
        /* 序号从 0 连续递增时，[first, max] 之间应有的包数减去实收即为丢失 */
        uint32_t expected = g_win_max_seq - g_win_first_seq + 1U;
        if (expected > g_win_pkts)
            lost = expected - g_win_pkts;
    }

    uint64_t bytes_s  = g_win_bytes * 1000ULL / window_ms;
    uint64_t mbs_x100 = bytes_s * 100ULL / (1024ULL * 1024ULL);
    uint64_t mbps_x10 = bytes_s * 8ULL * 10ULL / 1000000ULL;
    uint32_t avg      = g_win_pkts ? (uint32_t)(g_win_bytes / g_win_pkts) : 0U;

    kprintf("[bwtest] %llu.%02llu MB/s  %llu.%01llu Mbps  pkt=%u  lost=%u  "
            "ooo=%u  avg=%uB  |  total %llu pkt / %llu B / lost %llu\n",
            (unsigned long long)(mbs_x100 / 100ULL),
            (unsigned long long)(mbs_x100 % 100ULL),
            (unsigned long long)(mbps_x10 / 10ULL),
            (unsigned long long)(mbps_x10 % 10ULL),
            (unsigned)g_win_pkts,
            (unsigned)lost,
            (unsigned)g_win_ooo,
            (unsigned)avg,
            (unsigned long long)g_total_pkts,
            (unsigned long long)g_total_bytes,
            (unsigned long long)g_total_lost);

    g_total_lost += lost;
}

/* ── 周期统计（net_poll_once 每轮调用一次）─────────────────────────────── */

void bwtest_poll(void)
{
    if (!g_running)
        return;                                  /* 快路径：一次 load */

    uint64_t now = timer_get_uptime_ms();
    uint64_t dt  = now - g_win_start_ms;
    if (dt < BW_REPORT_MS)
        return;

    bw_report((uint32_t)dt);

    if (g_win_pkts == 0U) {
        /* 空窗口：发送端可能已经停了，连着几个空窗口就收工不再刷屏 */
        if (++g_idle_windows >= BW_MAX_IDLE_WINDOWS) {
            g_running = false;
            kprintf("[bwtest] 统计已停止（再发包会自动重新开始）\n");
        }
    } else {
        g_idle_windows = 0U;
    }

    bw_reset_window(now);
}

/* ── 接收回调 ──────────────────────────────────────────────────────────── */

static void bw_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                    const ip_addr_t *addr, u16_t port)
{
    uint8_t hdr[BW_HDR_LEN];

    (void)arg;
    (void)pcb;
    (void)port;

    if (!p)
        return;

    if (p->tot_len < BW_HDR_LEN) {
        pbuf_free(p);
        return;
    }

    /* pbuf 可能是链式的，用 pbuf_copy_partial 取头，不假设内存连续 */
    pbuf_copy_partial(p, hdr, BW_HDR_LEN, 0);

    if (bw_rd32le(hdr) != BW_MAGIC) {
        pbuf_free(p);
        return;
    }

    uint32_t seq = bw_rd32le(hdr + 4);

    if (!g_running) {
        g_running      = true;
        g_idle_windows = 0U;
        bw_reset_window(timer_get_uptime_ms());
        kprintf("[bwtest] 开始统计：来自 %s\n",
                addr ? ipaddr_ntoa(addr) : "?");
    }

    if (g_win_pkts == 0U) {
        g_win_first_seq = seq;
        g_win_max_seq   = seq;
    } else if (seq > g_win_max_seq) {
        g_win_max_seq = seq;
    } else {
        g_win_ooo++;
    }

    g_win_pkts++;
    g_win_bytes += p->tot_len;
    g_total_pkts++;
    g_total_bytes += p->tot_len;

    pbuf_free(p);
}

/* ── 初始化 ────────────────────────────────────────────────────────────── */

void bwtest_init(void)
{
    g_pcb = udp_new();
    if (!g_pcb) {
        KLOG_ERROR("[bwtest] udp_new failed\n");
        return;
    }

    err_t err = udp_bind(g_pcb, IP_ADDR_ANY, BW_PORT);
    if (err != ERR_OK) {
        KLOG_ERROR("[bwtest] udp_bind(%u) failed err=%d\n", BW_PORT, err);
        udp_remove(g_pcb);
        g_pcb = NULL;
        return;
    }

    udp_recv(g_pcb, bw_recv, NULL);
    KLOG_INFO("[bwtest] UDP sink listening on 0.0.0.0:%u\n", BW_PORT);
}
