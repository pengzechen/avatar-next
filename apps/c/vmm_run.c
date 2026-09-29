/*
 * vmm_run.c — 在宿主 shell 里启动 / 接入 / 管理 Linux guest
 *
 * 移植自 x-kernel: uapps/kvmm-run/kvmm-run.c
 *
 * 用法（宿主 busybox shell 里）：
 *     /bin/vmm-run              启动一个 guest；已有在跑的就接入它
 *     /bin/vmm-run -a <vmid>    接入指定的那个 VM（不新建）
 *     /bin/vmm-run -n           强制新建一个 VM，即使已经有在跑
 *     /bin/vmm-run -l           列出所有 VM，不接管控制台
 *     /bin/vmm-run -k [vmid]    停止 VM，不接管控制台；不带 vmid 停全部
 *
 * 运行中（前缀键 Ctrl+T，再按一个键）：
 *     Ctrl+T ?        帮助
 *     Ctrl+T d        detach：guest 留在后台，退回宿主 shell（再跑 vmm-run 接回）
 *     Ctrl+T k        停掉当前前台 guest 并退出
 *     Ctrl+T l        列出所有 VM（编号供 Ctrl+T <n> 用）
 *     Ctrl+T n        新建一个 VM 并切过去
 *     Ctrl+T 1..8     切到 -l 列出的第 n 个 VM
 *     Ctrl+T Ctrl+T   把 Ctrl+T 本身发给 guest
 *
 * 做三件事：
 *   1. 打开 /dev/vmm，ioctl(VMM_IOC_BOOT) 让内核启动 guest —— 如果已经有
 *      guest 在跑（上次 Ctrl+T d 留下的），这个 ioctl 只是接入，不重新加载
 *      镜像；
 *   2. 把 stdin 设成 raw mode，然后在本进程 stdin/stdout 与 /dev/vmm
 *      之间双向搬字节 —— 不做任何加工，ANSI 转义序列原样透传；
 *   3. 读到前缀键就进「命令态」，按上表处理。
 *
 * ⚠️ 同一时刻只支持**一个**交互式 helper：前台 VM 是内核里的一个全局变量
 *    （g_vmm_fg_vmid），B 一 attach，A 那个终端的读写就变成了 B 的 VM ——
 *    A 的窗口会不知不觉变成另一个 guest 的窗口。`-a` 的语义是「切走前台」，
 *    不是「再开一个会话」。
 *
 * ── 为什么是 Ctrl+T ────────────────────────────────────────────
 *
 * 从前用的是 Ctrl+]（停）和 Ctrl+[（分离），两个都不好：
 *
 *   - Ctrl+] 是 telnet 的转义键；
 *   - Ctrl+[ 就是 ESC(0x1b)，而方向键/Home/End/Fn 发出的正是以 0x1b 开头的
 *     转义序列（上箭头 = \x1b[A）。宿主内核 raw mode 下 read() 每次只返回
 *     1 字节，所以 \x1b[A 会拆成三次到达 —— 见到 0x1b 就分离的话，guest 里的
 *     方向键会全部失效。当时靠一个 ~40ms 的 poll 窗口消歧（有后续字节就当
 *     转义序列），能用但很 hack。
 *
 * 换成「前缀 + 单键命令」（screen / tmux / QEMU 都是这个路子）之后不再需要
 * 那个消歧窗口：前缀之后的那个字节一定是命令，不可能是转义序列的一部分。
 * 顺带白送一个好处 —— ESC 不再被拦截，guest 里的方向键天然可用。
 *
 * 前缀选 Ctrl+T(0x14)，因为它是唯一一个既好按、又没被占用的控制字节：
 *
 *   Ctrl+A   QEMU -nographic 的转义前缀（三个架构的 Makefile 都这么启）
 *            + screen / minicom / picocom —— **根本到不了这个内核**
 *   Ctrl+B   tmux
 *   Ctrl+]   telnet
 *   Ctrl+C   必须留给 guest（SIGINT）
 *   Ctrl+^   Ctrl+_   没人用，但要 Shift+6 / Shift+-，部分键盘布局和终端
 *                      压根发不出这两个字节
 *   Ctrl+\   kermit，而且是 guest 里的 SIGQUIT
 *
 * 代价：guest 里 bash 的 transpose-chars（罕用）让位给控制台；要发字面量
 * 就按两次（Ctrl+T Ctrl+T），或者单按一次前缀、等它超时自动补发。
 *
 * 为什么要在用户态做这件事（而不是内核直接接管控制台）：宿主 tty 层
 * （signal_check_uart / uart_ringbuf）始终独占真实 UART，内核里不存在
 * 第二个消费者，也就不需要「控制台归属」仲裁。guest 的字节全部经本进程
 * 中转，内核侧只有控制台 vdev 的两个环形缓冲。
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define DEV_VMM "/dev/vmm"

/*
 * 前缀键。它之后的那个字节被当作命令解释；连按两次发送字面量。
 */
#define PREFIX_KEY 0x14 /* Ctrl+T */

/*
 * 前缀之后等命令字节的窗口。见主循环里那段说明 —— 单按一次前缀（误触）
 * 时超时会把前缀本身当字面量发给 guest，避免"下一次按键被吃掉当命令"。
 *
 * 时间是**近似**的：本进程没有时钟源（内核只提供 poll 的超时，没有
 * clock_gettime 可供参考），所以用「poll 超时算一整步、有任何事件算一小步」
 * 来累加。精度无所谓 —— 这个窗口只决定"多久之后把孤立的前缀补发出去"。
 */
#define PREFIX_STEP_MS  50
#define PREFIX_WAIT_MS  1000
#define PREFIX_EVENT_MS 1

/*
 * 等上一个 guest 收尾时的重试次数（每次 1ms，见 delay_ms）。
 * 只用于启动路径：vCPU 任务要跑到下一个安全点才退出，期间 guest_loader
 * 的重入保护会拒绝，而内核不做定时等待。
 */
#define BOOT_RETRIES 2000

/*
 * /dev/vmm 的 ioctl 号 —— 必须与内核侧 include/pseudofs.h 里的一致。
 * 这里不能包含那个头（内核头文件，会拖进 types.h / kernel_stat.h），所以
 * 独立定义了一遍；两边的 _IOC 编码都是标准 Linux 布局，只要 type/nr/方向
 * 相同就对得上。**改一边必须改另一边。**
 */
#define VMM_IOC_GET_STATUS _IOR('V', 0, uint32_t) /* 出参：1 = 有 guest 在跑 */
#define VMM_IOC_DETACH     _IO('V', 1)            /* 本次 close 不停 guest */
#define VMM_IOC_STOP       _IO('V', 2)            /* 停止前台 guest */
#define VMM_IOC_BOOT       _IO('V', 3)            /* 启动 guest；已在跑则接入 */
/* 强制新建一个 VM（多 VM）。入参 0=自动分配 vmid，出参回填实际值。*/
#define VMM_IOC_BOOT_EX _IOWR('V', 4, uint32_t)

/*
 * 下面三个是「按 vmid 寻址」那一组，必须与 include/pseudofs.h 里那份
 * **逐字节一致**（含字段顺序和 info[] 容量）。内核侧有 _Static_assert
 * 钉着尺寸，改了那边这里会因为 ioctl 号变了而在运行期失败 —— 反过来说
 * 改了这里那边什么都不会报，只会在运行期变成下面 ENOTTY 那条排障提示。
 */
#define VMM_INFO_MAX 8

struct vmm_vm_info {
    uint32_t vmid;
    uint32_t state; /* 内核 vm.c 的状态机数值：1=LOADING 2=RUNNING 4=DYING */
};
struct vmm_list_req {
    uint32_t max;     /* [in]  info[] 的容量（本文件传 VMM_INFO_MAX）*/
    uint32_t count;   /* [out] 实际条目数                          */
    uint32_t fg_vmid; /* [out] 前台 vmid；0 = 没有前台             */
    uint32_t _rsv;
    struct vmm_vm_info info[VMM_INFO_MAX];
};

#define VMM_IOC_LIST    _IOWR('V', 5, struct vmm_list_req)
#define VMM_IOC_ATTACH  _IOW('V', 6, uint32_t) /* 入参 vmid：设为前台  */
#define VMM_IOC_STOP_VM _IOW('V', 7, uint32_t) /* 入参 vmid：停指定 VM */

static struct termios g_saved_tty;
static int g_tty_saved;

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

static void restore_tty(void)
{
    if (g_tty_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_tty);
}

/*
 * 被信号打死也要把 tty 还原。
 *
 * 不还原的话宿主 shell 会**留在无回显、无行编辑**的状态 —— 而这台机器
 * 只有一个控制台，你几乎没有别的办法把它改回来（Ctrl+C 在 raw mode 下
 * 是普通字节，连信号都不是）。helper 收到的常见信号是 SIGHUP/TERM。
 * SIGKILL 拦不住，那种情况下只能重开 QEMU。
 */
static void on_fatal_signal(int sig)
{
    restore_tty();
    _exit(128 + sig);
}

static int enter_raw_mode(void)
{
    struct termios raw;

    if (tcgetattr(STDIN_FILENO, &g_saved_tty) < 0)
        return -1;
    g_tty_saved = 1;

    signal(SIGHUP, on_fatal_signal);
    signal(SIGINT, on_fatal_signal);
    signal(SIGTERM, on_fatal_signal);
    signal(SIGQUIT, on_fatal_signal);

    raw = g_saved_tty;
    cfmakeraw(&raw);
    /*
     * cfmakeraw 清掉了 ICRNL，那会让回车以 '\r' 原样送到 guest。
     * Linux 的 tty 层自己会做 ICRNL，所以这里加不加都能用；加上的好处是
     * 宿主侧先统一成 '\n'，helper 转发时不必关心用户敲的是 CR 还是 LF。
     *
     * 顺带一提：cfmakeraw 也清掉了 ISIG，所以 raw mode 下 Ctrl+C 不再变成
     * 信号、而是当普通字节透传 —— 这正是「退出键不能复用 Ctrl+C」的原因
     * （guest 需要它），也是上面那个信号处理器存在的理由（本地 Ctrl+C
     * 救不了你）。内核那边的判据在 tty.c：只有 c_lflag 的 ISIG 位置着时，
     * 0x03 才会被转成 SIGINT 而不是塞进环形缓冲。
     */
    raw.c_iflag |= ICRNL;

    return tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

/*
 * ioctl 失败时的报错。
 *
 * ⚠️ ENOTTY 是个很有误导性的症状，值得单独说：内核把不认识的 ioctl 统一
 *    回成 ENOTTY —— 设备层虽然回的是 -ENOSYS，但 syscall 层会把 ENOSYS
 *    当成"这不是本设备管的"继续往下走，最后落到 -ENOTTY
 *    （kernel/syscall/fs/ioctl.c:65）。而 _IOC 把 sizeof 编进了操作码，
 *    所以**结构体尺寸对不上**和**内核太旧没有这个 ioctl** 是同一个症状。
 *    看到它就往这两个方向查，别去怀疑设备开错没有。
 */
static void ioctl_failed(const char *what)
{
    if (errno == ENOTTY)
        fprintf(stderr,
                "vmm-run: %s: %s\r\n"
                "         内核不认这个 ioctl —— 多半是内核与 helper 版本"
                "不一致（rootfs 里的 /bin/vmm-run 是旧的），\r\n"
                "         或者 vmm_list_req 两边的结构体尺寸对不上。\r\n",
                what, strerror(errno));
    else
        fprintf(stderr, "vmm-run: %s: %s\r\n", what, strerror(errno));
}

/* ── VM 列表 ─────────────────────────────────────────────────── */

static const char *state_name(uint32_t s)
{
    /* 与 include/vmm/vmm.h 的 VM_* 对应（3 是空号，别问，问就是历史）*/
    switch (s) {
    case 1:
        return "LOADING";
    case 2:
        return "RUNNING";
    case 4:
        return "DYING";
    default:
        return "?";
    }
}

/* 取一次列表。返回 0 成功、-1 失败（errno 已置）。*/
static int fetch_list(int fd, struct vmm_list_req *req)
{
    memset(req, 0, sizeof(*req));
    req->max = VMM_INFO_MAX;
    return ioctl(fd, VMM_IOC_LIST, req) < 0 ? -1 : 0;
}

/*
 * 打印 VM 表。第 1 列的序号就是 `Ctrl+T <n>` 用的那个 n ——
 * 刻意**不用 vmid 当序号**：vmid 从 1 开始自增、到 255 回绕（vm.c 的
 * g_next_vmid），长会话里 vm9 后面接着的是 vm10 而不是「第 10 个」，
 * 两个编号混用会很难解释。序号就是列表里的位置，自己看自己数。
 *
 * ⚠️ 序号映射会随 VM 生死变化（列表按 vmid 升序），所以 Ctrl+T <n> 每次
 *    都现场重取列表，不缓存。
 */
static void print_vm_list(int fd)
{
    struct vmm_list_req req;

    if (fetch_list(fd, &req) < 0) {
        ioctl_failed("list");
        return;
    }

    if (req.count == 0) {
        printf("\r\n[vmm-run] no VM running\r\n");
        return;
    }

    printf("\r\n[vmm-run]  #   vmid  state      fg\r\n");
    for (uint32_t i = 0; i < req.count && i < VMM_INFO_MAX; i++)
        printf("           %u  %4u  %-9s  %s\r\n", i + 1, req.info[i].vmid,
               state_name(req.info[i].state),
               req.info[i].vmid == req.fg_vmid ? "*" : "");
    if (req.count > VMM_INFO_MAX)
        printf("           ... %u more (truncated)\r\n",
               req.count - VMM_INFO_MAX);
}

/* ── 控制台里的命令（前缀之后的那一键）───────────────────────── */

static void print_help(void)
{
    printf("\r\n[vmm-run] Ctrl+T prefix — press one more key:\r\n"
           "           d   detach   guest keeps running, back to host shell\r\n"
           "           k   stop     stop the foreground guest and quit\r\n"
           "           l   list     show all VMs (numbers for Ctrl+T <n>)\r\n"
           "           n   new      create another VM and switch to it\r\n"
           "           1-8 switch   attach to VM #n from the list\r\n"
           "           ?   help     this text\r\n"
           "           Ctrl+T       send a literal Ctrl+T to the guest\r\n"
           "         (a lone Ctrl+T is passed through after ~1s)\r\n");
}

/*
 * Ctrl+T n —— 新建一个 VM 并切过去。
 *
 * 不复用 main() 里那个 2000×1ms 的重试循环：那是为「上一个 guest 还没收完尾」
 * 设计的，而这里更常见的失败是**池满**（MAX_VMS=4），内核立刻回 -EBUSY，
 * 再重试 2000 次只是白卡两秒 —— 更糟的是每次重试内核都要打两行日志，而
 * klog 是持全局锁、关中断逐字节写的，115200 下就是十几秒的关中断自旋。
 * 所以 EBUSY 直接认输。
 */
static void console_new_vm(int fd)
{
    uint32_t vmid = 0;

    printf("\r\n[vmm-run] creating a new VM...\r\n");

    for (int i = 0; i < BOOT_RETRIES; i++) {
        if (ioctl(fd, VMM_IOC_BOOT_EX, &vmid) == 0) {
            printf("[vmm-run] new VM vm%u — Ctrl+T l to list\r\n", vmid);
            return;
        }
        if (errno == EBUSY) {
            printf("[vmm-run] no free VM slot (MAX_VMS=4) — stop one with "
                   "`vmm-run -k <vmid>`\r\n");
            return;
        }
        delay_ms(1);
    }
    ioctl_failed("create VM");
}

/* Ctrl+T <n> —— 切到列表里的第 n 个 VM。不重新 boot，只是改前台。*/
static void console_switch_vm(int fd, unsigned idx)
{
    struct vmm_list_req req;
    uint32_t vmid;

    if (fetch_list(fd, &req) < 0) {
        ioctl_failed("list");
        return;
    }
    if (idx < 1 || idx > req.count || idx > VMM_INFO_MAX) {
        printf("\r\n[vmm-run] no VM #%u — Ctrl+T l to list\r\n", idx);
        return;
    }

    vmid = req.info[idx - 1].vmid;
    if (ioctl(fd, VMM_IOC_ATTACH, &vmid) < 0) {
        if (errno == EBUSY)
            printf("\r\n[vmm-run] vm%u is not running yet — Ctrl+T l\r\n",
                   vmid);
        else
            ioctl_failed("attach");
        return;
    }
    printf("\r\n[vmm-run] switched to vm%u\r\n", vmid);
}

/* 处理前缀之后的那个字节。返回 1 表示「要收尾了，本块剩余字节丢弃」。*/
static int run_command(unsigned char c, int fd, int *quit, int *detach)
{
    switch (c) {
    case '?':
    case 'h':
        print_help();
        return 0;
    case 'd':
        *detach = 1;
        return 1;
    case 'k':
        *quit = 1;
        return 1;
    case 'l':
        print_vm_list(fd);
        return 0;
    case 'n':
        console_new_vm(fd);
        return 0;
    default:
        break;
    }

    if (c >= '1' && c <= '9') {
        console_switch_vm(fd, (unsigned)(c - '0'));
        return 0;
    }

    /*
     * 未绑定的键：只提示，不假装成功。注意这里**不会**把该字节转发给 guest
     * —— 前缀已经消费掉它了，想发字面量得用 Ctrl+T Ctrl+T。
     */
    printf("\r\n[vmm-run] unknown command 0x%02x — Ctrl+T ? for help\r\n", c);
    return 0;
}

/* ── 命令行 ──────────────────────────────────────────────────── */

static int parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;

    if (!s || !*s)
        return -1;
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno != 0 || !end || *end != '\0' || v > 0xFFFFFFFFUL)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

/*
 * -k：停止 VM，不接管控制台。
 *
 * 这里**不需要**再发 DETACH 兜底 —— 内核侧给 close 加了「会话 owner」守卫
 * （见 kernel/fs/pseudofs/vmm_dev.c 的 g_vmm_owner_pid）：只有做过
 * BOOT/BOOT_EX/ATTACH 的那个任务才有资格在 close 里停 VM，而 `-k` 从没
 * BOOT 过，它的 close 天然什么都不做。
 *
 * 从前那种"自己发 DETACH 保护自己"的写法反而是有害的：g_vmm_detached 是
 * 会话级全局标志，`-k` 置了它，就可能被**真正 owner 的** close 消费掉 ——
 * 用户明确要停的 guest 会被当成"分离"留在后台继续烧 CPU。
 */
static int cmd_kill(int fd, int have_vmid, uint32_t vmid)
{
    struct vmm_list_req req;

    if (have_vmid) {
        if (ioctl(fd, VMM_IOC_STOP_VM, &vmid) < 0) {
            ioctl_failed("stop VM");
            return 1;
        }
        printf("[vmm-run] stop vm%u requested\r\n", vmid);
        return 0;
    }

    /* 不带 vmid：停全部 —— 多 VM 下「后台那个 guest」已经没有明确定义了 */
    if (fetch_list(fd, &req) < 0) {
        ioctl_failed("list");
        return 1;
    }
    if (req.count == 0) {
        printf("[vmm-run] no VM running\r\n");
        return 0;
    }

    /*
     * 停止是**异步**的：vm_request_stop 只置标志，vCPU 任务跑到下一个安全点
     * 才收尾。所以这里报的是"已请求"，不是"已停"。紧接着 `-k; vmm-run`
     * 大概率会撞上"上一个还没收完尾"，那时按提示等一下即可。
     */
    for (uint32_t i = 0; i < req.count && i < VMM_INFO_MAX; i++) {
        uint32_t v = req.info[i].vmid;
        if (ioctl(fd, VMM_IOC_STOP_VM, &v) < 0)
            fprintf(stderr, "vmm-run: stop vm%u: %s\r\n", v, strerror(errno));
        else
            printf("[vmm-run] stop vm%u requested\r\n", v);
    }
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: vmm-run [-n] [-a vmid] [-l] [-k [vmid]]\r\n"
            "  (no args)  boot a guest, or attach to the running one\r\n"
            "  -a vmid    attach to that VM (do not boot)\r\n"
            "  -n         always create another VM\r\n"
            "  -l         list VMs, do not take the console\r\n"
            "  -k [vmid]  stop VMs (all if vmid omitted)\r\n"
            "console: Ctrl+T ? for help, Ctrl+T d to detach\r\n");
}

int main(int argc, char **argv)
{
    int kill_only = 0;
    int force_new = 0;
    int list_only = 0;
    int have_attach = 0;
    int have_kill_vmid = 0;
    uint32_t attach_vmid = 0;
    uint32_t kill_vmid = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-k") || !strcmp(argv[i], "--kill")) {
            kill_only = 1;
            /* -k 的 vmid 是可选的：下一个参数是纯数字就当它的目标 */
            if (i + 1 < argc && parse_u32(argv[i + 1], &kill_vmid) == 0) {
                have_kill_vmid = 1;
                i++;
            }
        } else if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "--new")) {
            force_new = 1; /* 已有 VM 在跑时也再起一个，而不是接入 */
        } else if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--list")) {
            list_only = 1;
        } else if (!strcmp(argv[i], "-a") || !strcmp(argv[i], "--attach")) {
            if (i + 1 >= argc || parse_u32(argv[i + 1], &attach_vmid) != 0) {
                fprintf(stderr, "vmm-run: -a needs a vmid\r\n");
                return 2;
            }
            have_attach = 1;
            i++;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "vmm-run: unknown option '%s'\r\n", argv[i]);
            usage();
            return 2;
        }
    }

    int fd = open(DEV_VMM, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "vmm-run: open %s: %s\r\n", DEV_VMM, strerror(errno));
        return 1;
    }

    /*
     * -l / -k 都不接管控制台：查完/停完就走。
     *
     * 它们从没 BOOT 过，所以内核侧那个 owner 守卫会让它们的 close 变成
     * 空操作 —— 这正是「`vmm-run -l` 不该把别人正在用的 guest 杀掉」的
     * 保证所在（从前没有这道守卫时，一个纯查询动作真的会把 guest 停掉）。
     */
    if (list_only) {
        print_vm_list(fd);
        close(fd);
        return 0;
    }
    if (kill_only) {
        int rc = cmd_kill(fd, have_kill_vmid, kill_vmid);
        close(fd);
        return rc;
    }

    /* ── 接入指定 VM，或启动/接入 ────────────────────────── */
    int attaching = 0;

    if (have_attach) {
        /*
         * -a 只改前台，不碰 guest 的生命周期。它绕过 vmm_dev_boot，所以也
         * 不会去重置内核里的 g_vmm_detached —— 那个标志由 owner 的 close
         * 负责复位，进到这里必然是 0。
         */
        if (ioctl(fd, VMM_IOC_ATTACH, &attach_vmid) < 0) {
            if (errno == EBUSY)
                fprintf(stderr, "vmm-run: vm%u exists but is not running\r\n",
                        attach_vmid);
            else
                ioctl_failed("attach");
            close(fd);
            return 1;
        }
        attaching = 1;
    } else {
        uint32_t running = 0;
        int ok = 0;

        if (ioctl(fd, VMM_IOC_GET_STATUS, &running) < 0)
            running = 0; /* 查不到就当没有，后面 BOOT 会给出结论 */
        attaching = running;

        for (int i = 0; i < BOOT_RETRIES; i++) {
            uint32_t vmid = 0;
            int booted = force_new ? (ioctl(fd, VMM_IOC_BOOT_EX, &vmid) == 0)
                                   : (ioctl(fd, VMM_IOC_BOOT, 0) == 0);
            if (booted) {
                ok = 1;
                break;
            }
            /*
             * 失败通常是「上一个 guest 还没收完尾」—— 它的 vCPU 任务要跑到
             * 下一个安全点才退出，期间 guest_loader 的重入保护会拒绝。内核
             * 不做定时等待，隔 1ms 重试即可。
             *
             * 但池满（MAX_VMS=4）是立刻失败、重试无意义的，早点认输 ——
             * 而且每次重试内核都打两行日志，klog 是关中断写的，重试 2000 次
             * 就是十几秒的关中断自旋（顺带把别的核堵在 klog 锁上）。
             */
            if (errno == EBUSY && force_new) {
                fprintf(stderr,
                        "vmm-run: no free VM slot (MAX_VMS=4) — stop one "
                        "first (`vmm-run -l`, `vmm-run -k <vmid>`)\r\n");
                close(fd);
                return 1;
            }
            if (errno == ENOTTY) {
                ioctl_failed("boot");
                close(fd);
                return 1;
            }
            delay_ms(1);
        }
        if (!ok) {
            fprintf(stderr, "vmm-run: boot failed: %s\r\n", strerror(errno));
            close(fd);
            return 1;
        }
    }

    if (enter_raw_mode() < 0) {
        fprintf(stderr, "vmm-run: tcsetattr: %s\r\n", strerror(errno));
        /* 不致命：没有 raw mode 就退化成行缓冲，照样能用 */
    }

    /*
     * 报一下接的是哪个 vmid —— 光看 "guest started" 分不出是 vm1 还是 vm2，
     * 而多 VM 下这正是最容易搞混的地方（banner 里那个就是内核当前认定的
     * 前台，也正是 write()/read() 会落到的那个 VM）。
     */
    {
        struct vmm_list_req req;
        /* force_new 优先：`-n` 在有 guest 在跑时 attaching 也是 1，但语义是
         * "又新建了一个"，报成 attached 会让人以为接到了老的那个。 */
        const char *what = force_new   ? "new VM"
                           : attaching ? "attached"
                                       : "guest started";

        if (fetch_list(fd, &req) == 0 && req.fg_vmid != 0)
            printf("\r\n[vmm-run] %s (vm%u) — Ctrl+T ? for help\r\n", what,
                   req.fg_vmid);
        else
            printf("\r\n[vmm-run] %s — Ctrl+T ? for help\r\n", what);
    }
    fflush(stdout);

    /* ── 双向搬运 ────────────────────────────────────────── */
    struct pollfd pfds[2];
    pfds[0].fd = STDIN_FILENO;
    pfds[0].events = POLLIN;
    pfds[1].fd = fd;
    pfds[1].events = POLLIN;

    int quit = 0;        /* Ctrl+T k：停止并退出 */
    int detach = 0;      /* Ctrl+T d：分离 */
    int gone = 0;        /* guest 自己没了（或被别的进程停掉了）*/
    int cmd_mode = 0;    /* 已看到前缀，正在等命令字节 */
    int cmd_elapsed = 0; /* 命令窗口已过去的近似毫秒数 */

    while (!quit && !detach && !gone) {
        pfds[0].revents = 0;
        pfds[1].revents = 0;

        /*
         * 等命令字节时用有限超时，其余时候一直等。
         *
         * 这个超时是**必须**的：没有它的话，单独误按一次 Ctrl+T 会把
         * **下一次**按键当成命令执行 —— 而其中 'k' 是"停掉前台 VM 并退出"。
         * screen/tmux 的做法是超时后把前缀原样发给 guest，这里照做。
         */
        int r = poll(pfds, 2, cmd_mode ? PREFIX_STEP_MS : -1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "vmm-run: poll: %s\r\n", strerror(errno));
            break;
        }

        if (cmd_mode) {
            cmd_elapsed += (r == 0) ? PREFIX_STEP_MS : PREFIX_EVENT_MS;
            if (cmd_elapsed >= PREFIX_WAIT_MS) {
                /* 孤立的前缀：当成用户想发一个字面 Ctrl+T，补发出去 */
                cmd_mode = 0;
                cmd_elapsed = 0;
                write_all(fd, "\x14", 1);
            }
        }
        if (r == 0)
            continue;

        /*
         * guest → 宿主终端。
         *
         * ⚠️ HUP/ERR 必须当成**会话结束**来处理。只把 fd 里的数据抽干就
         *    继续下一轮 monitor 的话：guest 没了的时候 read() 返回的是
         *    -EAGAIN（前台已经查不到），内层循环 break 而 quit 仍是 0，
         *    外层 poll 又会立刻报 HUP —— 于是原地打起 100% CPU 的死循环，
         *    而且 raw mode 下 ISIG 是关的、Ctrl+C 连信号都不产生，
     *    **单控制台的机器上你退不出来**。这个坑在"guest 自己 poweroff"
     *    时本来就会踩到，而 `-k` 让它变成日常路径。
         */
        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            int hup = pfds[1].revents & (POLLHUP | POLLERR);
            char buf[512];

            for (;;) {
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n > 0) {
                    write_all(STDOUT_FILENO, buf, (size_t)n);
                    if ((size_t)n < sizeof(buf))
                        break;
                    continue;
                }
                if (n < 0 && errno == EAGAIN) {
                    if (hup)
                        gone = 1; /* 取空了 + 报了 HUP ⇒ guest 真的没了 */
                    break;
                }
                if (n < 0 && errno == EINTR)
                    continue;
                /* 出错，或者 guest 已经退出 */
                gone = 1;
                break;
            }
        }
        if (gone)
            break;

        /* 宿主键盘 → guest（前缀之外的字节原样透传）*/
        if (pfds[0].revents & POLLIN) {
            unsigned char buf[256];
            unsigned char fwd[256];
            size_t fwdn = 0;
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));

            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EINTR))
                    continue;
                break; /* stdin 关了 */
            }

            for (ssize_t i = 0; i < n; i++) {
                unsigned char c = buf[i];

                if (!cmd_mode) {
                    if (c == PREFIX_KEY) {
                        cmd_mode = 1;
                        cmd_elapsed = 0;
                        continue; /* 前缀本身不转发 */
                    }
                    fwd[fwdn++] = c;
                    continue;
                }

                /*
                 * 命令态：这一个字节一定是命令。
                 *
                 * raw mode 下 read() 每次只回 1 字节（内核 file_io.c 的 raw
                 * 分支收够一个就 break），所以 n 通常是 1、cmd_mode 不会跨
                 * read 悬着。但这里仍按「一个 buf 里可能有多个字节」写，
                 * 不去依赖那个实现细节 —— 万一 tcsetattr 失败退化成行缓冲，
                 * 一整行会一次到达，这时只有前缀后的第一个字节是命令。
                 */
                cmd_mode = 0;

                if (c == PREFIX_KEY) { /* Ctrl+T Ctrl+T → 字面量 */
                    fwd[fwdn++] = PREFIX_KEY;
                    continue;
                }
                if (run_command(c, fd, &quit, &detach))
                    break; /* 收尾命令：本块剩余字节丢弃（和从前一致）*/
            }

            if (fwdn > 0 && write_all(fd, (const char *)fwd, fwdn) < 0) {
                fprintf(stderr, "vmm-run: write guest input failed\r\n");
                break;
            }
            if (quit || detach)
                break;
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

    if (gone) {
        /*
         * guest 自己结束了（poweroff），或者被另一个进程 `vmm-run -k` 停掉了。
         * 不用再发 STOP —— 那个 VM 已经不在池里了。
         */
        restore_tty();
        close(fd);
        printf("\r\n[vmm-run] guest is gone — back to the host shell\r\n");
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
