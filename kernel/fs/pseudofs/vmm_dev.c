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
 *   ioctl(fd, VMM_IOC_BOOT_EX)       强制新建一个 VM，出参回填 vmid
 *   ioctl(fd, VMM_IOC_LIST)          出参：所有 VM 的 {vmid,state} + 前台 vmid
 *   ioctl(fd, VMM_IOC_ATTACH)        入参 vmid：把它设为前台（不必重新 boot）
 *   ioctl(fd, VMM_IOC_STOP_VM)       入参 vmid：停指定的那个，**不要求先接入**
 *   write(fd, bytes, n)              **永远**是 guest 的串口输入
 *   read(fd, buf, n)                 取 guest 串口输出；无数据返回 -EAGAIN
 *   poll(fd)                         guest 有输出报 EPOLLIN；guest 退出报 EPOLLHUP
 *   ioctl(fd, VMM_IOC_GET_STATUS)    出参 1 = 有 guest 在跑
 *   ioctl(fd, VMM_IOC_DETACH)        分离：本次 close 不停 guest（Ctrl+T d）
 *   ioctl(fd, VMM_IOC_STOP)          停止前台 guest（Ctrl+T k）
 *   close(fd)                        默认停止 guest；分离过则不停止
 *
 * 后三个号（LIST/ATTACH/STOP_VM）是"按 vmid 寻址"那一组：在此之前要停某个
 * VM **必须先接入它**，而接入路径又只挑最小 vmid —— vm1/vm2 同时跑时你
 * 根本碰不到 vm2。详见 include/pseudofs.h 里那三个号上面的说明。
 *
 * 控制全部走 ioctl，write 永远只是数据 —— 原因见 include/pseudofs.h 里
 * VMM_IOC_* 上面的那段说明（「首次写是命令」在接入场景下有二义性，实测
 * 会把 "bootlinux" 当成输入喂给 guest）。这也是与 kvmm 唯一的有意偏离：
 * kvmm 只有 chardev 没有 ioctl，只能用「首次写是命令」。
 *
 * VMM_IOC_BOOT 是「启动**或**接入」而不是只启动：上一次 Ctrl+T d 留在后台
 * 的 guest 还在跑时，直接接入它，不重新加载镜像。于是用户只需要记住一条
 * 命令（vmm-run），内核按「有没有在跑的 guest」决定新建还是接入。
 *
 * guest 镜像路径是写死的 /guests/linux 下那三个文件，不像 kvmm 那样由
 * 参数带进来；将来要支持多 guest 的话，把参数挂在 BOOT 的 ioctl 出参上。
 *
 * 中断注入不在这里做：RX FIFO 由控制台 vdev 持有，置 pending 的时机在 VMM
 * 进入 guest 前（电平触发，见 vmm_console_irq_asserted 的注释）。
 */

#include "pseudofs.h" /* VMM_IOC_* */
#include "pseudofs_internal.h"
#include "klog.h"
#include "string.h"
#include "syscall/io/epoll.h"
#include "syscall/syscall_internal.h" /* copy_to_user_bytes（GET_STATUS 出参）*/
#include "task/task.h"                /* task_current（会话 owner 判定）*/

#include "vmm/vmm.h"
#include "vmm/vmm_console.h"

#if VMM_GUEST_LINUX_SUPPORTED
#include "guest_loader.h"
#endif

/*
 * 分离（detach）标志：由 VMM_IOC_DETACH 置位，本次 close **不**停止 guest，
 * 让它留在后台继续跑。每次会话在收到 bootlinux 时清零，所以「分离」只影响
 * 当前这一次会话 —— 重新 attach 之后，再关掉 helper 就会正常停止 guest。
 *
 * 保留「close 默认停止」这条兜底是因为 helper 可能被 kill -9 或崩溃：
 * 那种情况下没人会去设 detached，guest 就该跟着一起收掉，而不是无声地
 * 在后台烧 CPU。
 */
static int g_vmm_detached;

/*
 * 前台 VM 的 vmid（0 = 没有前台）。
 *
 * 从前这里是 g_vmm_booted —— 一个布尔，因为内核里只有一个 VM。多 VM 之后
 * "谁在用这个设备"必须说得清：write() 把输入送给前台 VM 的 RX，read() 从
 * 前台 VM 的 TX 取，STOP/close 也只停前台那一个。
 */
static int g_vmm_fg_vmid;

/*
 * 会话 owner 的 pid（0 = 没有会话）。只有它才有资格停 VM、消费 detached。
 *
 * ── 为什么需要这个 ────────────────────────────────────────────
 *
 * close() 的兜底语义是「停掉前台 VM」，而 /dev/vmm 是**全局单例**设备：
 * 任何一个进程 open 一下再 close 都会走到那个兜底分支。于是
 *
 *     vmm-run -l        ← 只是查个列表
 *
 * 会把别的 helper 正在用的 guest **停掉**。这不是竞态，是必然：`-l` 从不
 * BOOT，却照样能触发 close 的破坏性分支。同理 `vmm-run -k <非前台vmid>`
 * 停完 vm3 之后，close 还会顺手把你在用的 vm1 也停掉。
 *
 * 所以把「会话」显式记成 owner：做过 BOOT / BOOT_EX / ATTACH 的任务才是
 * 本次会话的主人。`-l`、`-k` 这类从不 BOOT 的进程于是天然退化成只读
 * （`-k` 的停止走的是显式 ioctl，不依赖 close 兜底）。
 *
 * 存 **pid 而不是 task_t\***：任务退出后指针会被复用，pid 在任务槽表里是
 * 单调分配的（kernel/task/task.c），拿它比对不会认错人。
 *
 * helper 被 kill -9 仍然兜得住：sys_exit 会关闭它所有 fd，那一刻
 * task_current() 还是它自己，owner 比对成立，guest 照常被停 —— 这条兜底
 * 正是「不能无声地把 guest 留在后台烧 CPU」的保证，别弄丢。
 */
static int g_vmm_owner_pid;

/* 调用者是不是本次会话的主人（见 g_vmm_owner_pid 的说明）。*/
static int vmm_dev_is_owner(void)
{
    task_t *t = task_current();
    return t && g_vmm_owner_pid != 0 && (int)t->id == g_vmm_owner_pid;
}

/* 把当前任务登记成会话主人。*/
static void vmm_dev_claim(void)
{
    task_t *t = task_current();
    if (t)
        g_vmm_owner_pid = (int)t->id;
}

/* ── 启动 / 接入 ──────────────────────────────────────────── */
#if VMM_GUEST_LINUX_SUPPORTED
static int vmm_dev_boot(int force_new)
{
    /*
     * 每次会话从这里开始，先清掉上一次可能残留的「分离」意图。
     * 不清的话：分离过一次之后再 attach，那次会话的 close 又会变成分离，
     * guest 就永远停不掉了。
     */
    g_vmm_detached = 0;

    /*
     * 已经有一个 guest 在跑（典型场景：上一次 Ctrl+T d 把它留在后台了）
     * → 本次会话就当它的前台，直接接入，不重新加载镜像。
     *
     * 这也是为什么不需要单独的 attach 命令：helper 只管发 VMM_IOC_BOOT，
     * 内核按「有没有在跑的 guest」决定是新建还是接入。
     */
    if (!force_new && vmm_guest_running()) {
        vm_t *fg = vm_get((uint32_t)g_vmm_fg_vmid);

        /*
         * 先试「接回上次离开的那个」（screen -r 语义）。
         *
         * 这一条是补上的，也是"从 vm2 分离后再也接不回 vm2"的直接原因：
         * 从前这里**只**按最小 vmid 扫，于是 vm1/vm2 同时跑时，从 vm2 分离
         * 之后再敲 vmm-run 会接到 vm1 —— g_vmm_fg_vmid 明明还记着 vm2，
         * 却没有任何人看它。
         *
         * ⚠️ 只认 VM_RUNNING，不能像从前那样只判 `!= VM_FREE`：vm_get 连
         * LOADING / DYING 的槽位也返回（vm.c:100），而接到一个正在收尾的
         * VM 上，随后的 write/read 就打进一个马上要被 memset 的槽位 ——
         * 表现为"attach 成功了但立刻 HUP 退出"。这正是重启回归里
         * 「没等收尾就再敲 vmm-run 只会接入」那个症状的根源。
         */
        if (!fg || fg->state != VM_RUNNING) {
            fg = NULL;
            for (uint32_t v = 1; v <= 255; v++) {
                vm_t *p = vm_get(v);
                if (p && p->state == VM_RUNNING) {
                    fg = p;
                    break;
                }
            }
        }

        if (!fg) {
            /*
             * vmm_guest_running() 为真、却挑不出一个 RUNNING 的 —— 那些 VM
             * 都还在收尾（DYING）。返回 EBUSY 让调用方按"上一个还没收完尾"
             * 重试，而不是接上一个将死的槽位。
             */
            KLOG_INFO("[vmmdev] boot: guests present but none running yet\n");
            return -PFS_EBUSY;
        }

        g_vmm_fg_vmid = (int)fg->vmid;
        vmm_dev_claim(); /* 这个任务从此是本次会话的主人（见 g_vmm_owner_pid）*/
        KLOG_INFO("[vmmdev] boot: attaching to vm%u\n", fg->vmid);

        /* 通道应当是开着的；分离期间没关过，这里只是兜底 */
        vmm_console_tx_set_enabled(fg, 1);
        return 0;
    }

    /*
     * 没有在跑的 guest，但上一个可能还没收完尾（vCPU 任务要跑到下一个安全点
     * 才退出，满载时最坏一个宿主 tick ≈ 10ms）。这里不等，让调用方重试 ——
     * 内核里做定时等待要么引 timer 依赖，要么就是个没上界的自旋循环。
     */
    KLOG_INFO("[vmmdev] boot: starting guest\n");

    /*
     * 先把控制台切到「通道模式」，再启动 guest。
     *
     * 顺序有讲究：切了通道之后 VMM 就不再碰宿主 UART（vmm_console_pump 会
     * 让位给 helper），而 guest 的启动输出进 TX 缓冲等 helper 来取。反过来
     * 先启动的话，guest 的头几行会走 klog 混进宿主日志里。
     *
     * 此刻 vCPU 任务还没被调度（guest_loader_run_linux 内部才 task_create，
     * 而当前任务不会立刻让出），所以不存在「输出漏到 klog」的窗口。
     */
    /*
     * 从 VM 池里取一个槽位，配上镜像布局，然后交给 loader。
     *
     * 从前这里是"全局唯一那个 VM"—— guest_loader 内部一个 static vm_t，
     * 于是第二个 VM 必然覆盖第一个。现在是池：每次 boot 都拿一个新槽位。
     */
    vm_t *vm = vm_alloc();
    if (!vm) {
        /* 没拿到槽位就什么都没动过 —— 控制台归属也还没切，不用回滚 */
        KLOG_ERROR("[vmmdev] boot: no free VM slot\n");
        return -PFS_EBUSY;
    }

    vm->cfg.mem_base = GUEST_LINUX_MEM_BASE;
    vm->cfg.mem_size = GUEST_LINUX_MEM_SIZE;
    vm->cfg.nr_vcpus = 1;

    /*
     * 先把控制台切到「通道模式」，再启动 guest。
     * 顺序有讲究：切了通道之后 VMM 就不再碰宿主 UART（vmm_console_pump
     * 会让位给 helper），guest 的启动输出进本 VM 的 TX 缓冲等 helper 来取。
     * 反过来先启动的话，guest 的头几行会走 klog 混进宿主日志里。
     */
    vmm_console_tx_set_enabled(vm, 1);

    /* guest_loader_run_linux 内部也做了重入保护，失败会返回负值 */
    int rc = guest_loader_run_linux(vm);
    if (rc != 0) {
        KLOG_ERROR("[vmmdev] guest_loader_run_linux failed: %d\n", rc);
        vmm_console_tx_set_enabled(vm, 0);
        vm_free(vm); /* 槽位不能漏 —— 没人在跑了 */
        return -PFS_EIO;
    }

    g_vmm_fg_vmid = (int)vm->vmid;
    vmm_dev_claim(); /* 这个任务从此是本次会话的主人（见 g_vmm_owner_pid）*/

    return 0;
}
#endif /* VMM_GUEST_LINUX_SUPPORTED */

/* ── 节点操作（声明见 pseudofs_internal.h，与 tpu_dev_ioctl 等同一约定）── */

int vmm_dev_write(int nid, const void *buf, size_t len)
{
    (void)nid;
    if (!buf)
        return -PFS_EINVAL;

#if !VMM_GUEST_LINUX_SUPPORTED
    return -PFS_ENOSYS;
#else
    /*
     * write() 永远只是 guest 的串口输入 —— 启动/停止/分离全在 ioctl 里。
     * 没有单独的「命令」分支，所以也就不存在「这次的写到底是命令还是数据」
     * 这个二义性（见文件头注释）。
     */
    vm_t *fg = vm_get((uint32_t)g_vmm_fg_vmid);
    if (!fg)
        return -PFS_EIO; /* 没有前台 VM，写也没意义 */

    for (size_t i = 0; i < len; i++)
        vmm_console_push_rx(fg, ((const uint8_t *)buf)[i]);
    return (int)len;
#endif
}

int vmm_dev_read(int nid, uint64_t off, void *buf, size_t len)
{
    (void)nid;
    (void)off;
    if (!buf)
        return -PFS_EINVAL;

#if !VMM_GUEST_LINUX_SUPPORTED
    return -PFS_ENOSYS;
#else
    vm_t *fg = vm_get((uint32_t)g_vmm_fg_vmid);
    if (!fg)
        return -PFS_EAGAIN;

    size_t n = 0;
    while (n < len && vmm_console_tx_pop(fg, (uint8_t *)buf + n))
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

#if !VMM_GUEST_LINUX_SUPPORTED
    return -PFS_ENOSYS;
#else
    switch (req) {
    case VMM_IOC_BOOT:
        return vmm_dev_boot(0 /*有在跑的就接入*/);

    case VMM_IOC_BOOT_EX: {
        /* 强制新建一个 VM；出参回填它分到的 vmid（多 VM 的入口）*/
        uint32_t want = 0;
        int rc;

        if (!argp)
            return -PFS_EINVAL;
        if (copy_from_user_bytes(argp, &want, sizeof(want)) < 0)
            return -PFS_EINVAL;

        rc = vmm_dev_boot(1 /*总是新建*/);
        if (rc != 0)
            return rc;

        want = (uint32_t)g_vmm_fg_vmid;
        if (copy_to_user_bytes(&want, argp, sizeof(want)) < 0)
            return -PFS_EINVAL;
        return 0;
    }

    case VMM_IOC_LIST: {
        struct vmm_list_req req;

        if (!argp)
            return -PFS_EINVAL;
        if (copy_from_user_bytes(argp, &req, sizeof(req)) < 0)
            return -PFS_EINVAL;

        uint32_t cap = req.max;
        if (cap > 8)
            cap = 8; /* info[] 固定 8 项（见 pseudofs.h 的说明）*/

        uint32_t n = 0;
        for (uint32_t v = 1; v <= 255; v++) {
            vm_t *p = vm_get(v);
            if (!p || p->state == VM_FREE)
                continue;
            if (n < cap) {
                req.info[n].vmid = p->vmid;
                req.info[n].state = (uint32_t)p->state;
            }
            n++;
        }

        /*
         * count 报**真实总数**（可能大于 cap），info[] 只填了前 cap 个 ——
         * 调用方据此知道被截断了。当前 MAX_VMS=4 而 cap=8，走不到截断。
         *
         * 为什么这里不做原子快照也够用：每次 vm_get 只借池锁查一遍，拿到
         * vm_t* 之后读 vmid/state 是无锁的，理论上可能读到刚被回收的槽位。
         * 对"列个表给人看"来说最坏结果是多一个/少一个条目，而下游的
         * ATTACH 会再校验一次状态，所以不值得为此引一把新锁。
         */
        req.count = n;
        req.fg_vmid = (uint32_t)g_vmm_fg_vmid;
        req._rsv = 0;

        if (copy_to_user_bytes(&req, argp, sizeof(req)) < 0)
            return -PFS_EINVAL;
        return 0;
    }

    case VMM_IOC_ATTACH: {
        uint32_t vmid = 0;
        vm_t *vm;

        if (!argp)
            return -PFS_EINVAL;
        if (copy_from_user_bytes(argp, &vmid, sizeof(vmid)) < 0)
            return -PFS_EINVAL;

        vm = vm_get(vmid);
        /*
         * 区分这两种失败，helper 才好给出一句有用的话：
         *   ENOENT —— 压根没这个 vmid（多半是列表看旧了）
         *   EBUSY  —— 有这个 VM，但还没跑起来（LOADING）或正在收尾（DYING）
         */
        if (!vm || vm->state == VM_FREE)
            return -PFS_ENOENT;
        if (vm->state != VM_RUNNING)
            return -PFS_EBUSY;

        KLOG_INFO("[vmmdev] attach: foreground = vm%u\n", vmid);
        g_vmm_fg_vmid = (int)vmid;
        vmm_dev_claim();

        /*
         * 只把新前台打开，**不要**去关旧前台的 console_owned。
         *
         * 那个标志的语义不是"谁是前台"，而是"这台 VM 的控制台由 helper
         * 接管了"，而 vmm_console_pump(vcpu->vm) 是拿**每个 VM 自己**的标志
         * 把关的（vmm_console.c:46）。把旧前台的清掉，它继续在另一颗核上跑
         * 的时候泵就会复活，去跟宿主 tty 层抢同一个真实 UART 的硬件 FIFO
         * （tty.c 的 signal_check_uart 是第一个消费者，泵是第二个，
         * 谁先跑谁拿到 —— 输入会随机丢给错误的一方）。
         */
        vmm_console_tx_set_enabled(vm, 1);
        return 0;
    }

    case VMM_IOC_STOP_VM: {
        uint32_t vmid = 0;
        vm_t *vm;

        if (!argp)
            return -PFS_EINVAL;
        if (copy_from_user_bytes(argp, &vmid, sizeof(vmid)) < 0)
            return -PFS_EINVAL;

        vm = vm_get(vmid);
        if (!vm || vm->state == VM_FREE)
            return -PFS_ENOENT;

        /*
         * 停一个**指定**的 VM —— 这是"想退出 vm 不必先 vmm-run 进去"的关键。
         * 不要求它是前台；只有它正好是前台时才顺带把前台清掉。
         *
         * 只置 stop_req 就返回：真正的回收由该 VM 的 vCPU 任务在下一个安全点
         * 自己做（vm.c 的并发规则："跨任务只传 vmid"、"销毁由 vCPU 任务自己
         * 完成"），这里不等 —— 等了就要么引 timer 依赖，要么是个没上界的自旋。
         */
        KLOG_INFO("[vmmdev] stop vm%u requested by helper\n", vmid);
        vm_request_stop(vm);
        /*
         * 同样**不要**清 g_vmm_fg_vmid，哪怕它正好是前台 —— 理由与
         * VMM_IOC_STOP 那段完全相同：清了就等于让 attach 着它的 helper
         * 永远收不到 EPOLLHUP，只能靠用户敲键盘才发现 guest 没了。
         */
        return 0;
    }

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
         * Ctrl+T d：把 guest 留在后台。只置标志 —— 真正的交接在 close 里，
         * 那里会跳过「停止 guest」和「关 TX 通道」两件事。
         *
         * ⚠️ 必须限定 owner：g_vmm_detached 是**会话级**的全局标志，而
         * close 会**消费**它。要是让一个无关进程（比如 `vmm-run -k`）也能
         * 置位，它就能把 owner 的 close 变成分离 —— 用户明确要求停掉的
         * guest 会被留在后台继续烧 CPU。
         */
        if (!vmm_dev_is_owner()) {
            KLOG_WARN("[vmmdev] detach: not the session owner, ignored\n");
            return -PFS_EINVAL;
        }
        KLOG_INFO("[vmmdev] detach: guest keeps running in background\n");
        g_vmm_detached = 1;
        return 0;

    case VMM_IOC_STOP:
        KLOG_INFO("[vmmdev] stop requested by helper\n");
        /*
         * 只停**前台**那个 VM —— 别的 VM（挂起或后台跑的）不受影响。
         *
         * ⚠️ 停完**不要**清 g_vmm_fg_vmid。清了之后 vmm_dev_poll 就再也
         * 报不出 EPOLLHUP（它的判据是 fg_vmid != 0，见那边），正在 attach
         * 这个 VM 的 helper 会**无声地卡在 poll 里** —— 写回 EIO、读回
         * EAGAIN，它却收不到任何事件，只能等用户碰一下键盘才退出。
         * 留着它，HUP 会把"这个 VM 没了"如实告诉 helper。
         * （下一句 vm_get 会拿到一个 DYING/FREE 的槽位，poll 据此报 HUP。）
         */
        vm_request_stop(vm_get((uint32_t)g_vmm_fg_vmid));
        return 0;

    default:
        return -PFS_ENOSYS;
    }
#endif
}

uint32_t vmm_dev_poll(int nid)
{
    (void)nid;

#if !VMM_GUEST_LINUX_SUPPORTED
    return 0;
#else
    uint32_t ev = EPOLLOUT; /* 永远收得下写 */

    vm_t *fg = vm_get((uint32_t)g_vmm_fg_vmid);
    if (fg && vmm_console_tx_has_data(fg))
        ev |= EPOLLIN;

    /* guest 已经退出（比如 guest 自己 poweroff）：告诉 helper 别再等了。
     * 未启动时不算 HUP —— 那时候 helper 正要发 bootlinux。 */
    /*
     * 前台 VM 已经收尾（比如 guest 自己 poweroff）才报 HUP。
     * 未启动时（fg==0）不算 —— 那时候 helper 正要发 BOOT。
     */
    if (g_vmm_fg_vmid != 0) {
        vm_t *fg = vm_get((uint32_t)g_vmm_fg_vmid);
        if (!fg || fg->state == VM_FREE || fg->state == VM_DYING)
            ev |= EPOLLHUP;
    }

    return ev;
#endif
}

int vmm_dev_close(int nid)
{
    (void)nid;

#if !VMM_GUEST_LINUX_SUPPORTED
    return 0;
#else
    /*
     * 不是本会话的主人就什么都不做 —— 见 g_vmm_owner_pid 的说明。
     * 这一条挡的是「随便 open/close 一下就停掉别人的 guest」：`vmm-run -l`
     * 是个纯查询，它从没 BOOT 过，必须让它走到这里就返回。
     */
    if (!vmm_dev_is_owner()) {
        KLOG_INFO("[vmmdev] close: not the session owner, nothing to do\n");
        return 0;
    }
    g_vmm_owner_pid = 0; /* 本次会话到此结束 */

    /*
     * Ctrl+T d 分离：guest 留在后台继续跑，这里**什么都不收拾**。
     *
     * 特别是不能关 TX 通道 —— 关了 vmm_console_pump() 就会复活，跟宿主
     * shell 抢真实 UART；也不能清 RX FIFO —— 那里面的字节是给当前这个
     * guest 的，重新 attach 时还要用。
     *
     * 注意 console_owned 的语义是「这台 VM 的控制台归 helper」而**不是**
     * 「谁是前台」，所以分离时把它留着是对的。detach 与 close 在这件事上
     * 必须一致，否则后台那台 VM 的泵会去抢宿主 UART，把用户按键推进一台
     * 他看不见的 guest。
     */
    if (g_vmm_detached) {
        g_vmm_detached = 0;
        KLOG_INFO("[vmmdev] close: detached, guest left running\n");
        return 0;
    }

    /*
     * ⚠️ fg 必须在 g_vmm_fg_vmid 清零**之前**取好。
     *
     * 从前的写法是把下面那两句清理放在清零之后、并重新
     * `vm_get(g_vmm_fg_vmid)` —— 而那时它已经是 0，vm_get(0) 恒为 NULL
     * （vmid 从 1 起发放），于是 `if (fg)` 从不成立，那两句是**死代码**。
     * 一直没爆是因为 VM 随后就被销毁，vm_free 的 memset 和下一次 boot 时
     * vpl011_init / uart16550_init 的 memset 顺带把这些状态清了。
     */
    vm_t *fg = vm_get((uint32_t)g_vmm_fg_vmid);
    g_vmm_fg_vmid = 0;

    if (!fg) {
        KLOG_INFO("[vmmdev] close: no foreground VM to stop\n");
        return 0;
    }

    /*
     * 把控制台打回「直打宿主控制台」模式，并清掉 RX FIFO 里的残留按键。
     * 不清的话，下一次 helper 启动 guest 时，上一轮没取走的输入会先喂给
     * 新 guest 的 getty；tx_set_enabled(0) 还会顺带丢弃 TX 环里没人再读的
     * 残留输出（见 uart16550_tx_set_enabled）。
     *
     * 效果其实是**余量**：close 不停的那个 VM 马上就要被销毁，vm_free 和
     * 下一次 *_init 的 memset 本来就会清掉这些状态。留着的意义是别让"关了
     * 控制台"这件事依赖"反正后面会 memset"。
     *
     * ⚠️ 顺序：必须放在 vm_request_stop 之**前**。
     * stop 只置标志，vCPU 任务可能立刻就在另一颗核上跑到 vm_free 把槽位
     * memset 掉（guest 空闲时每条 WFI 都陷入 EL2，窗口一点都不窄）。
     * vpl011_of / uart16550_of 里那道 `d->st.owner != vm` 守卫只能挡住
     * "槽位已释放"，挡不住"槽位释放后又被 vm_alloc 复用"—— 复用时间址相同、
     * owner 被重新设成同一个指针，守卫会放行，于是去关掉**另一台 VM** 的
     * 控制台并清空它的 TX 环。先收拾就完全没有这个窗口：此刻它还活着。
     */
    KLOG_INFO("[vmmdev] close: releasing vm%u console and stopping it\n",
              fg->vmid);
    vmm_console_tx_set_enabled(fg, 0);
    vmm_console_rx_flush(fg);
    vm_request_stop(fg);
    return 0;
#endif
}
