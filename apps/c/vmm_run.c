/*
 * vmm_run.c — 在宿主 shell 里启动 / 接入 / 管理一个 Linux guest
 *
 * 移植自 x-kernel: uapps/kvmm-run/kvmm-run.c
 *
 * 用法（宿主 busybox shell 里）：
 *     /bin/vmm-run          启动一个 guest；已经有在跑的就接入它
 *     /bin/vmm-run -k       只停掉后台的 guest，不接管控制台
 *
 * 运行中：
 *     Ctrl+]  (0x1d)   停止 guest 并退出（destroy）
 *     Ctrl+[  (0x1b)   把 guest 留在后台跑，退回宿主 shell
 *                      再用 /bin/vmm-run 就能接回来
 *
 * 做三件事：
 *   1. 打开 /dev/vmm，ioctl(VMM_IOC_BOOT) 让内核启动 guest —— 如果已经有
 *      guest 在跑（上次 Ctrl+[ 留下的），这个 ioctl 只是接入，不重新加载
 *      镜像；
 *   2. 把 stdin 设成 raw mode，然后在本进程 stdin/stdout 与 /dev/vmm
 *      之间双向搬字节 —— 不做任何加工，ANSI 转义序列原样透传；
 *   3. 读到上面两个转义键之一就收尾：恢复 tty、按需 ioctl(DETACH/STOP)、
 *      关闭 /dev/vmm。
 *
 * 为什么是 Ctrl+] 和 Ctrl+[：raw mode 下 Ctrl+C 是被当成普通字节转发给
 * guest 的（guest 需要它），所以不能用它退出。
 *
 * 为什么要在用户态做这件事（而不是内核直接接管控制台）：宿主 tty 层
 * （signal_check_uart / uart_ringbuf）始终独占真实 UART，内核里不存在
 * 第二个消费者，也就不需要「控制台归属」仲裁。guest 的字节全部经本
 * 进程中转，内核侧只有 vpl011 的两个环形缓冲。
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define DEV_VMM     "/dev/vmm"

/* 两个转义键。其它字节一律透传。 */
#define ESC_STOP    0x1d    /* Ctrl+] —— 停止并退出 */
#define ESC_DETACH  0x1b    /* Ctrl+[ —— 分离，guest 留在后台 */

/*
 * ESC 消歧的等待窗口（毫秒）。见 esc_is_standalone()。
 */
#define ESC_LOOKAHEAD_MS  40

/*
 * /dev/vmm 的 ioctl 号 —— 必须与内核侧 include/pseudofs.h 里的一致。
 * 这里不能包含那个头（内核头文件），所以独立定义了一遍；两边的 _IOC
 * 编码都是标准 Linux 布局，只要 type/nr/方向 相同就对得上。
 */
#define VMM_IOC_GET_STATUS  _IOR('V', 0, uint32_t)  /* 出参：1 = 有 guest 在跑 */
#define VMM_IOC_DETACH      _IO ('V', 1)            /* 本次 close 不停 guest */
#define VMM_IOC_STOP        _IO ('V', 2)            /* 停止 guest */
#define VMM_IOC_BOOT        _IO ('V', 3)            /* 启动 guest；已在跑则接入 */
/* 强制新建一个 VM（多 VM）。入参 0=自动分配 vmid，出参回填实际值。*/
#define VMM_IOC_BOOT_EX     _IOWR('V', 4, uint32_t)

/* 等上一个 guest 收尾时的重试次数（每次 1ms，见 delay_ms）*/
#define BOOT_RETRIES  2000

static struct termios g_saved_tty;
static int            g_tty_saved;

static void delay_ms(int ms)
{
    /*
     * 用 poll(NULL, 0, ms) 当睡眠：内核的 ppoll 路径已经实测可用，
     * 而我们不需要纳秒精度。避免再依赖 nanosleep/sched_yield 是否实现。
     */
    poll(NULL, 0, ms);
}

static int write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static int enter_raw_mode(void)
{
    struct termios raw;

    if (tcgetattr(STDIN_FILENO, &g_saved_tty) < 0)
        return -1;
    g_tty_saved = 1;

    raw = g_saved_tty;
    cfmakeraw(&raw);
    /*
     * cfmakeraw 清掉了 ICRNL，那会让回车以 '\r' 原样送到 guest。
     * Linux 的 tty 层自己会做 ICRNL，所以这里加不加都能用；加上的好处是
     * 宿主侧先统一成 '\n'，helper 转发时不必关心用户敲的是 CR 还是 LF。
     */
    raw.c_iflag |= ICRNL;

    return tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void restore_tty(void)
{
    if (g_tty_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_tty);
}

/*
 * esc_is_standalone — 刚读到的 0x1b 是「单独按下的 Ctrl+[」还是转义序列的开头？
 *
 * 为什么必须判断：Ctrl+[ 就是 ESC(0x1b)，而方向键/Home/End/Fn 发出的正是
 * 以 0x1b 开头的转义序列（上箭头 = \x1b[A）。更麻烦的是宿主内核在 raw
 * mode 下的 read() **每次只返回 1 字节**（见 read_handler 的 raw 分支），
 * 所以 \x1b[A 会拆成三次到达 —— 见到 0x1b 就分离的话，guest 里的方向键
 * 会全部失效。
 *
 * 做法：等一小会儿看有没有后续字节。有 → 是转义序列，0x1b 照常转发；
 * 没有 → 是单独的 Ctrl+[，执行分离。正常敲方向键时后续字节几乎同时到达，
 * 感知不到这 40ms。
 *
 * poll 返回 -1（EINTR 等）时按「不是单独 ESC」处理：宁可把字节转给 guest，
 * 也不要因为一次信号打断就把用户踢出 guest。
 */
static int esc_is_standalone(void)
{
    struct pollfd p;

    p.fd      = STDIN_FILENO;
    p.events  = POLLIN;
    p.revents = 0;

    return poll(&p, 1, ESC_LOOKAHEAD_MS) == 0;
}

int main(int argc, char **argv)
{
    int kill_only = 0;
    int force_new = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-k") || !strcmp(argv[i], "--kill"))
            kill_only = 1;
        if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "--new"))
            force_new = 1;      /* 已有 VM 在跑时也再起一个，而不是接入 */
    }

    int fd = open(DEV_VMM, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "vmm-run: open %s: %s\n", DEV_VMM, strerror(errno));
        return 1;
    }

    uint32_t running = 0;
    if (ioctl(fd, VMM_IOC_GET_STATUS, &running) < 0)
        running = 0;            /* 查不到就当没有，后面 bootlinux 会给出结论 */

    /* ── -k：只清理后台 guest，不接管控制台 ───────────────── */
    if (kill_only) {
        if (!running) {
            printf("[vmm-run] no guest running\n");
            close(fd);
            return 0;
        }
        ioctl(fd, VMM_IOC_STOP, 0);
        printf("[vmm-run] stop requested\n");
        close(fd);
        return 0;
    }

    int attaching = running;

    /* ── 启动或接入 guest ────────────────────────────────── */
    int ok = 0;
    for (int i = 0; i < BOOT_RETRIES; i++) {
        uint32_t vmid = 0;
        int booted = force_new
            ? (ioctl(fd, VMM_IOC_BOOT_EX, &vmid) == 0)
            : (ioctl(fd, VMM_IOC_BOOT, 0) == 0);
        if (booted) {
            if (force_new)
                printf("\r\n[vmm-run] new VM vmid=%u\r\n", vmid);
            ok = 1;
            break;
        }
        /*
         * 失败通常是「上一个 guest 还没收完尾」—— 它的 vCPU 任务要跑到
         * 下一个安全点才退出，期间 guest_loader 的重入保护会拒绝。内核
         * 不做定时等待，隔 1ms 重试即可。
         */
        delay_ms(1);
    }
    if (!ok) {
        fprintf(stderr, "vmm-run: boot failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    if (enter_raw_mode() < 0) {
        fprintf(stderr, "vmm-run: tcsetattr: %s\n", strerror(errno));
        /* 不致命：没有 raw mode 就退化成行缓冲，照样能用 */
    }

    printf("\r\n[vmm-run] %s — Ctrl+] stop, Ctrl+[ detach\r\n",
           attaching ? "attached to running guest" : "guest started");
    fflush(stdout);

    /* ── 双向搬运 ────────────────────────────────────────── */
    struct pollfd pfds[2];
    pfds[0].fd     = STDIN_FILENO;
    pfds[0].events = POLLIN;
    pfds[1].fd     = fd;
    pfds[1].events = POLLIN;

    int quit   = 0;     /* Ctrl+]：停止 */
    int detach = 0;     /* Ctrl+[：分离 */

    while (!quit && !detach) {
        pfds[0].revents = 0;
        pfds[1].revents = 0;

        int r = poll(pfds, 2, -1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "vmm-run: poll: %s\n", strerror(errno));
            break;
        }

        /* guest → 宿主终端 */
        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[512];
            for (;;) {
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n > 0) {
                    write_all(STDOUT_FILENO, buf, (size_t)n);
                    if ((size_t)n < sizeof(buf))
                        break;
                    continue;
                }
                if (n < 0 && errno == EAGAIN)
                    break;          /* 取空了，正常 */
                if (n < 0 && errno == EINTR)
                    continue;
                /* 出错或 guest 已经退出 */
                quit = 1;
                break;
            }
        }
        if (quit)
            break;

        /* 宿主键盘 → guest */
        if (pfds[0].revents & POLLIN) {
            unsigned char buf[256];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EINTR))
                    continue;
                break;              /* stdin 关了 */
            }

            /*
             * 找本块里的转义键。找到就把**它之前**的字节转发给 guest；
             * 转义键之后的字节丢弃（和 kvmm-run 一致 —— 那一块里剩下的
             * 内容已经没有意义了）。
             *
             * raw mode 下 n 通常是 1，所以 ESC 的消歧才能成立：
             * 每个 0x1b 都是独立一次 read 送来的。
             */
            ssize_t fwd = n;
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == ESC_STOP) {
                    fwd = i;
                    quit = 1;
                    break;
                }
                if (buf[i] == ESC_DETACH && esc_is_standalone()) {
                    fwd = i;
                    detach = 1;
                    break;
                }
            }
            if (fwd > 0 && write_all(fd, (const char *)buf, (size_t)fwd) < 0) {
                fprintf(stderr, "vmm-run: write guest input failed\n");
                break;
            }
        }
    }

    /* ── 收尾 ────────────────────────────────────────────── */
    if (detach) {
        /*
         * 先告诉内核「这次 close 不要停 guest」，再退出。
         * close 之后 vCPU 任务照常跑，输出继续进 TX 环形缓冲；下次
         * vmm-run 接入时会把缓冲里攒下的内容一并吐出来（类似 screen -r）。
         */
        ioctl(fd, VMM_IOC_DETACH, 0);
        restore_tty();
        close(fd);
        printf("\r\n[vmm-run] detached — guest keeps running; "
               "run vmm-run to reattach\r\n");
        return 0;
    }

    if (quit)
        ioctl(fd, VMM_IOC_STOP, 0);

    /*
     * close 是兜底：即使上面没来得及 ioctl(STOP)，内核侧默认也会停 guest
     * （分离标志只在 DETACH 后为真）。这里不等 guest 真的停下来：vCPU
     * 任务会在下一个安全点自己收尾，而我们要的是立刻把控制台还给宿主 shell。
     */
    restore_tty();
    close(fd);

    printf("\r\n[vmm-run] guest stopping\r\n");
    return 0;
}
