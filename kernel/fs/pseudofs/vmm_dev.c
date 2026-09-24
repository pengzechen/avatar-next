/*
 * kernel/fs/pseudofs/vmm_dev.c — /dev/vmm：从宿主 shell 里控制 guest 的字符设备
 *
 * 移植自 x-kernel: virt/kvmm-api/src/device.rs（chardev /dev/kvmm-vm）。
 *
 * 设计意图：宿主 busybox shell 先起来，用户在 shell 里跑 /bin/vmm-run，
 * 由它打开本设备、启动 guest 并在 stdin/stdout 与设备之间双向搬字节。
 * 这样宿主 tty 层始终独占真实 UART（signal_check_uart），VMM 不碰硬件；
 * guest 的输入输出全部经用户态 helper 中转。
 *
 * 协议（对标 kvmm-api 的 read/write/poll/release，另加了分离/接入）：
 *   open("/dev/vmm", O_RDWR)         打开设备
 *   ioctl(fd, VMM_IOC_BOOT)          启动 guest；已在跑则接入它
 *   write(fd, bytes, n)              **永远**是 guest 的串口输入
 *   read(fd, buf, n)                 取 guest 串口输出；无数据返回 -EAGAIN
 *   poll(fd)                         guest 有输出报 EPOLLIN；guest 退出报 EPOLLHUP
 *   ioctl(fd, VMM_IOC_GET_STATUS)    出参 1 = 有 guest 在跑
 *   ioctl(fd, VMM_IOC_DETACH)        分离：本次 close 不停 guest（Ctrl+[）
 *   ioctl(fd, VMM_IOC_STOP)          停止 guest（Ctrl+]）
 *   close(fd)                        默认停止 guest；分离过则不停止
 *
 * 控制全部走 ioctl，write 永远只是数据 —— 原因见 include/pseudofs.h 里
 * VMM_IOC_* 上面的那段说明（「首次写是命令」在接入场景下有二义性，实测
 * 会把 "bootlinux" 当成输入喂给 guest）。这也是与 kvmm 唯一的有意偏离：
 * kvmm 只有 chardev 没有 ioctl，只能用「首次写是命令」。
 *
 * VMM_IOC_BOOT 是「启动**或**接入」而不是只启动：上一次 Ctrl+[ 留在后台
 * 的 guest 还在跑时，直接接入它，不重新加载镜像。于是用户只需要记住一条
 * 命令（vmm-run），内核按「有没有在跑的 guest」决定新建还是接入。
 *
 * guest 镜像路径是写死的 /guests/linux 下那三个文件，不像 kvmm 那样由
 * 参数带进来；将来要支持多 guest 的话，把参数挂在 BOOT 的 ioctl 出参上。
 *
 * 中断注入不在这里做：RX FIFO 由 vpl011 持有，置 pending 的时机在 VMM
 * 进入 guest 前的 aarch64_check_vpl011_rx()（电平触发，见其注释）。
 */

#include "pseudofs.h"                   /* VMM_IOC_* */
#include "pseudofs_internal.h"
#include "klog.h"
#include "string.h"
#include "syscall/io/epoll.h"
#include "syscall/syscall_internal.h"   /* copy_to_user_bytes（GET_STATUS 出参）*/

#if ARCH_AARCH64
#include "vmm/vmm_vpl011.h"
#include "vmm/vmm.h"
#include "guest_loader.h"
#endif

/*
 * 「有一个 guest 会话在用这台设备」：VMM_IOC_BOOT / 接入时置位，
 * 停止或收尾时清零。用来决定 poll 要不要报 EPOLLHUP、close 要不要停 guest。
 * （write 的语义不看它 —— write 永远只是 guest 输入。）
 */
static int g_vmm_booted;

/*
 * Ctrl+[ 离开时由 VMM_IOC_DETACH 置位：本次 close **不**停止 guest，
 * 让它留在后台继续跑。每次会话在收到 bootlinux 时清零，所以「分离」只影响
 * 当前这一次会话 —— 重新 attach 之后，再关掉 helper 就会正常停止 guest。
 *
 * 保留「close 默认停止」这条兜底是因为 helper 可能被 kill -9 或崩溃：
 * 那种情况下没人会去设 detached，guest 就该跟着一起收掉，而不是无声地
 * 在后台烧 CPU。
 */
static int g_vmm_detached;

/* ── 启动 / 接入 ──────────────────────────────────────────── */
#if ARCH_AARCH64
static int vmm_dev_boot(void)
{
    /*
     * 每次会话从这里开始，先清掉上一次可能残留的「分离」意图。
     * 不清的话：分离过一次之后再 attach，那次会话的 close 又会变成分离，
     * guest 就永远停不掉了。
     */
    g_vmm_detached = 0;

    /*
     * 已经有一个 guest 在跑（典型场景：上一次 Ctrl+[ 把它留在后台了）
     * → 本次会话就当它的前台，直接接入，不重新加载镜像。
     *
     * 这也是为什么不需要单独的 attach 命令：helper 只管发 VMM_IOC_BOOT，
     * 内核按「有没有在跑的 guest」决定是新建还是接入。
     */
    if (vmm_guest_running()) {
        KLOG_INFO("[vmmdev] boot: attaching to running guest\n");
        g_vmm_booted = 1;
        /* 通道应当是开着的；分离期间没关过，这里只是兜底 */
        vpl011_tx_set_enabled(1);
        return 0;
    }

    /*
     * 没有在跑的 guest，但上一个可能还没收完尾（vCPU 任务要跑到下一个安全点
     * 才退出，满载时最坏一个宿主 tick ≈ 10ms）。这里不等，让调用方重试 ——
     * 内核里做定时等待要么引 timer 依赖，要么就是个没上界的自旋循环。
     */
    KLOG_INFO("[vmmdev] boot: starting guest\n");

    /*
     * 先把 vpl011 切到「通道模式」，再启动 guest。
     *
     * 顺序有讲究：切了通道之后 VMM 就不再碰宿主 UART（vmm_console_pump 会
     * 让位给 helper），而 guest 的启动输出进 TX 缓冲等 helper 来取。反过来
     * 先启动的话，guest 的头几行会走 klog 混进宿主日志里。
     *
     * 此刻 vCPU 任务还没被调度（guest_loader_run_linux 内部才 task_create，
     * 而当前任务不会立刻让出），所以不存在「输出漏到 klog」的窗口。
     */
    vpl011_tx_set_enabled(1);

    /* guest_loader_run_linux 内部也做了重入保护，失败会返回负值 */
    int rc = guest_loader_run_linux();
    if (rc != 0) {
        KLOG_ERROR("[vmmdev] guest_loader_run_linux failed: %d\n", rc);
        vpl011_tx_set_enabled(0);
        return -PFS_EIO;
    }

    g_vmm_booted = 1;
    return 0;
}
#endif /* ARCH_AARCH64 */

/* ── 节点操作（声明见 pseudofs_internal.h，与 tpu_dev_ioctl 等同一约定）── */

int vmm_dev_write(int nid, const void *buf, size_t len)
{
    (void)nid;
    if (!buf)
        return -PFS_EINVAL;

#if !ARCH_AARCH64
    return -PFS_ENOSYS;
#else
    /*
     * write() 永远只是 guest 的串口输入 —— 启动/停止/分离全在 ioctl 里。
     * 没有单独的「命令」分支，所以也就不存在「这次的写到底是命令还是数据」
     * 这个二义性（见文件头注释）。
     */
    if (!vmm_guest_running())
        return -PFS_EIO;        /* guest 没了，写也没意义 */

    for (size_t i = 0; i < len; i++)
        vpl011_push_rx(((const uint8_t *)buf)[i]);
    return (int)len;
#endif
}

int vmm_dev_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid;
    (void)off;
    if (!buf)
        return -PFS_EINVAL;

#if !ARCH_AARCH64
    return -PFS_ENOSYS;
#else
    size_t n = 0;
    while (n < len && vpl011_tx_pop((uint8_t *)buf + n))
        n++;

    /* 一个都没有才算空。非阻塞语义：helper 先用 poll() 等可读 */
    if (n == 0)
        return -PFS_EAGAIN;
    return (int)n;
#endif
}

int vmm_dev_ioctl(int nid, uint64_t req, void *argp)
{
    (void)nid;

#if !ARCH_AARCH64
    return -PFS_ENOSYS;
#else
    switch (req) {
    case VMM_IOC_BOOT:
        return vmm_dev_boot();

    case VMM_IOC_GET_STATUS: {
        uint32_t running = vmm_guest_running() ? 1u : 0u;

        if (!argp)
            return -PFS_EINVAL;
        if (copy_to_user_bytes(&running, argp, sizeof(running)) < 0)
            return -PFS_EINVAL;
        return 0;
    }

    case VMM_IOC_DETACH:
        /*
         * Ctrl+[：把 guest 留在后台。只置标志 —— 真正的交接在 close 里，
         * 那里会跳过「停止 guest」和「关 TX 通道」两件事。
         */
        KLOG_INFO("[vmmdev] detach: guest keeps running in background\n");
        g_vmm_detached = 1;
        return 0;

    case VMM_IOC_STOP:
        KLOG_INFO("[vmmdev] stop requested by helper\n");
        if (vmm_guest_running())
            vmm_request_stop();
        g_vmm_booted = 0;
        return 0;

    default:
        return -PFS_ENOSYS;
    }
#endif
}

uint32_t vmm_dev_poll(int nid)
{
    (void)nid;

#if !ARCH_AARCH64
    return 0;
#else
    uint32_t ev = EPOLLOUT;             /* 永远收得下写 */

    if (vpl011_tx_has_data())
        ev |= EPOLLIN;

    /* guest 已经退出（比如 guest 自己 poweroff）：告诉 helper 别再等了。
     * 未启动时不算 HUP —— 那时候 helper 正要发 bootlinux。 */
    if (g_vmm_booted && !vmm_guest_running())
        ev |= EPOLLHUP;

    return ev;
#endif
}

int vmm_dev_close(int nid)
{
    (void)nid;

#if !ARCH_AARCH64
    return 0;
#else
    /*
     * Ctrl+[ 分离：guest 留在后台继续跑，这里**什么都不收拾**。
     *
     * 特别是不能关 TX 通道 —— 关了 vmm_console_pump() 就会复活，跟宿主
     * shell 抢真实 UART；也不能清 RX FIFO —— 那里面的字节是给当前这个
     * guest 的，重新 attach 时还要用。
     */
    if (g_vmm_detached) {
        g_vmm_detached = 0;
        KLOG_INFO("[vmmdev] close: detached, guest left running\n");
        return 0;
    }

    if (g_vmm_booted) {
        KLOG_INFO("[vmmdev] close: stopping guest\n");
        vmm_request_stop();
        g_vmm_booted = 0;
    }

    /*
     * 把 vpl011 打回「直打宿主控制台」模式，并清掉 RX FIFO 里的残留按键。
     * 不清的话，下一次 helper 启动 guest 时，上一轮没取走的输入会先喂给
     * 新 guest 的 getty。
     */
    vpl011_tx_set_enabled(0);
    vpl011_rx_flush();
    return 0;
#endif
}

