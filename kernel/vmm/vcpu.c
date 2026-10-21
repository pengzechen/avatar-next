/*
 * kernel/vmm/vcpu.c — vCPU：执行主循环 + 内核任务
 *
 * 从 vmm.c 拆出来的（那边混了 VM 池、主循环、控制台泵和一个架构的初始化）。
 * 这里管「一个 vCPU 在内核里是一回事」：
 *   - vmm_run_vcpu     : 执行主循环，通过架构钩子抽象 eret / vmlaunch 的差异
 *   - vcpu_task_fn     : 任务体，跑主循环 + 收尾（含销毁自己那个 VM 的槽位）
 *   - vcpu_task_create : 建档，含按核摊开的亲和性设置
 *
 * 池的并发规则（"跨任务只传 vmid"、"销毁由 vCPU 任务自己做"）写在 vm.c ——
 * vcpu_task_fn 的收尾正是那条规则的执行者。
 */

#include "vmm/vmm.h"
#include "vmm/vmm_console.h" /* vmm_console_pump（直启模式的输入桥）*/
#include "klog.h"
#include "task/task.h"
#include "task/sched.h" /* sched_check_and_yield：主循环顶部主动让出 CPU */
#include "task/cpu.h"   /* g_num_cpus：vCPU 摊核时用 */
#if ARCH_AARCH64
#include "aarch64/stage2.h"    /* stage2_activate：每轮进 guest 前重写 VTTBR */
#include "vmm/vmm_irq_route.h" /* vmm_irq_route_clear_owner */
#endif

/* ── VMM 主循环（架构无关）────────────────────────────────── */
/*
 * vmm_run_vcpu — vCPU 执行主循环
 *
 * 通过架构钩子抽象 eret（AArch64）/ vmlaunch+vmresume（x86）差异。
 * 返回 0：guest 正常退出；-1：未处理 exit。
 */
int vmm_run_vcpu(vcpu_t *vcpu)
{
    KLOG_INFO("[VMM] Starting vcpu%d\n", vcpu->vcpu_id);

    while (1) {
        /*
         * 宿主请求停止（/dev/vmm 的 close）。
         *
         * 在循环顶部查而不是在别处：这里正好是「上一次 guest 已经退出、
         * HCR_EL2 已被 el2_trap_exit 还原成 host 模式（TGE=1、VM=0）」
         * 的状态，直接 return 不会把 Stage-2 或 guest 向量表留在生效状态。
         */
        if (vcpu->vm->stop_req) {
            KLOG_INFO("[VMM] vcpu%d: stop requested, leaving guest loop\n",
                      vcpu->vcpu_id);
            return 0;
        }

        /*
         * 主动让出 CPU。
         *
         * 必须有这个调用：guest 退出走的是 VMM 自己的 guest_vec_table，
         * **不经过** sched_check_and_yield_from_trap() —— 那个钩子挂在宿主
         * 正常异常向量表的返回路径上。所以 vCPU 任务在循环里从不进入调度器，
         * 宿主其它任务会被完全饿死。实测症状：宿主 shell 里跑 /bin/vmm-run，
         * helper 卡在启动 guest 的那次 write 之后就再也不动了（连它自己的
         * banner 都打不出来），因为再也抢不到 cpu0。
         *
         * 位置与上面的停止检查相同：此刻上一次 guest 已退出、HCR_EL2 已被
         * el2_trap_exit 还原成 host 模式，在这里切任务是安全的。
         * sched_check_and_yield() 自己判 need_resched 与中断上下文，没有
         * 待调度任务时只是一次廉价判断。
         */
        sched_check_and_yield();

        /*
         * 记录本颗 vCPU 当前所在的宿主 CPU（多 vCPU 视角）。
         *
         * 每次进 guest 前刷新：调度器不做运行期迁移（任务入队即粘核），
         * 所以这里读到的值就是「这颗 vCPU 被钉在哪颗核」，exit handler 里
         * 通过 vmm_vcpu_host_cpu() 能直接拿到。同时也覆盖了「任务第一次
         * 被调度」——到这一刻 host_cpu 才第一次有真实值。
         */
        vcpu->host_cpu = (int)get_current_cpu_id();

        uint64_t irq_flags = arch_irq_save();

#if ARCH_AARCH64
        /* 每轮进 guest 前重写本 VM 的 VTCR/VTTBR —— 本核可能刚跑过别的 VM
         * 的 vCPU 任务（时间片轮转），不重写就会用错页表。这也是"挂起后
         * 恢复不需要额外 stage-2 动作"的原因。*/
        stage2_activate(&vcpu->vm->s2);
#endif

        /* 恢复 guest 上下文（AArch64: EL1 sysregs；x86: no-op）*/
        vmm_arch_restore_guest_ctx(vcpu);

        /* 进入 guest（AArch64: eret; x86: vmlaunch/vmresume）*/
        int ok = vmm_arch_enter_guest(vcpu);
        if (!ok) {
            arch_irq_restore(irq_flags);
            KLOG_ERROR("[VMM] vcpu%d: guest entry failed\n", vcpu->vcpu_id);
            return -1;
        }
        vcpu->launched = 1;

        /* 保存 guest 上下文，再处理可能会阻塞/调度/打印的 VM-exit。*/
        vmm_arch_save_guest_ctx(vcpu);

        arch_irq_restore(irq_flags);

#if VMM_GUEST_LINUX_SUPPORTED
        /*
         * 每次回到宿主都顺手收一次控制台输入（见 vmm_console_pump）。
         *
         * ⚠️ 只让 BSP 泵。宿主真实 UART 的 RX FIFO 是**单消费者**资源：
         * 多 vCPU 各自从自己的任务里调 uart_rx_ready()/uart_getc()，同一个
         * 字节会被其中一颗抢走、另一颗看到空 —— 输入会随机丢。跨 vCPU 的
         * 串行化本来就要有（锁或单一 owner），BSP-only 是最简单且与
         * 「控制台属于 VM、不属于某颗 vCPU」的语义一致的做法。
         */
        if (vmm_vcpu_is_bsp(vcpu))
            vmm_console_pump(vcpu->vm);
#endif

        /* 处理 VM exit */
        int ret = vmm_arch_exit_handler(vcpu);

        switch (ret) {
        case EL2_RESUME:
            continue;
        case EL2_VMEXIT:
            KLOG_INFO("[VMM] vcpu%d: guest exited normally\n", vcpu->vcpu_id);
            return 0;
        case EL2_VMABORT:
            KLOG_WARN("[VMM] vcpu%d: guest aborted\n", vcpu->vcpu_id);
            return 0;
        case EL2_VMSKIP:
            continue;
        case EL2_EXIT:
        default:
            KLOG_ERROR("[VMM] vcpu%d: unhandled exit, stopping VMM\n",
                       vcpu->vcpu_id);
            return -1;
        }
    }
}

/* ── vCPU 任务入口（内核任务函数）────────────────────────── */
static void vcpu_task_fn(void *arg)
{
    vcpu_t *vcpu = (vcpu_t *)arg;
    vm_t *vm = vcpu->vm;

    /*
     * 任务真正跑起来之后才发布 task/state/host_cpu —— 创建者（可能在
     * task_create 返回前就与本任务并发）不写这些字段，避免和本任务竞争。
     */
    vcpu->task = task_current();
    vcpu->state = VCPU_STATE_RUNNING;
    vcpu->host_cpu = (int)get_current_cpu_id();

    KLOG_INFO("[vmm] vm%u vcpu%d task started (id=%u) on cpu%u\n", vm->vmid,
              vcpu->vcpu_id, task_current()->id, get_current_cpu_id());

    int rc = vmm_run_vcpu(vcpu);

    /*
     * 本颗 vCPU 退出 = 整个 VM 该收了。
     *
     * 正常关机（HVC_DONE / SBI shutdown）与错误退出都走这里：置 VM 级的
     * stop_req，让兄弟 vCPU 在循环顶部看到后各自退出。多 vCPU 下**不能**
     * 只让自己退出 —— 否则 VM 永远等不齐最后一颗、槽位也回收不了。
     */
    vcpu->state = VCPU_STATE_STOPPED;
    vcpu->host_cpu = -1;
    vcpu->task = NULL;
    vm_request_stop(vm);

    if (rc == 0)
        KLOG_INFO("[vmm] vcpu%d exited normally\n", vcpu->vcpu_id);
    else
        KLOG_ERROR("[vmm] vcpu%d exited with error %d\n", vcpu->vcpu_id, rc);

    /*
     * 摘掉「本 pCPU 的 vCPU 承载任务」登记。
     *
     * GICv3 下宿主 vtimer 不由 ISR 注入（见 irq_route.c 的说明），所以这纯
     * 属清账；但 GICv2 路径会用它，而这张表是按任务指针匹配的 —— 任务退出
     * 后指针会被复用，留着旧值就可能让 ISR 去 unblock / 注入一个已经不是
     * vCPU 的任务。
     *
     * irq_route.c 只在 aarch64 的 vdev 列表里，其它架构没有这个符号。
     */
#if ARCH_AARCH64
    vmm_irq_route_clear_owner();
#endif

    /*
     * 本颗 vCPU 退场，由**最后一个**退出的兄弟负责回收 VM 槽位
     * （连同 stage-2 按需页与设备槽位）。
     *
     * **必须在 task_exit() 之前调**：池的并发规则是"跨任务只传 vmid"，
     * 谁也不能在任务还活着的时候清槽位；task_exit() 之后本任务就没了，
     * 再没人能安全地做这件事。多 vCPU 下直接 vm_free() 会在第一个退出时
     * 拆掉还有兄弟在跑的 stage-2 —— 必须走 vm_vcpu_stopped()。
     */
    /*
     * 打印这个 vCPU 任务的内核栈高水位 —— 定 TASK_STACK_SIZE 的依据就是它。
     * 栈是静态数组挨着放的，溢出会先踩坏**邻居**任务而不是自己，所以这个
     * 数字比"崩没崩"可靠得多（见 task.h 里 TASK_STACK_SIZE 的说明）。
     */
    {
        size_t used = task_stack_used(task_current());
        KLOG_INFO("[vmm] vcpu%d kernel stack high-water: %zu / %u bytes\n",
                  vcpu->vcpu_id, used, (unsigned)TASK_STACK_SIZE);
    }

    vm_vcpu_stopped(vm);

    task_exit();
}

struct task *vcpu_task_create(vcpu_t *vcpu, uint8_t priority)
{
    /*
     * 任务名：vm<vmid>c<vcpu_id>。
     *
     * 曾经这里写成先 "vm<vmid>" 再 "vcpu<id>" —— 第二段把 name[0..2] 整个
     * 覆盖掉了，所以 ps 里永远显示 vcpu<id>，第一段是死代码（改了半天名字
     * 不变就是这个原因）。两段都保留：一个 VM 可以有多个 vCPU
     * （MAX_VCPUS=4），只用 vmid 会让同一 VM 的 vcpu 任务重名。
     *
     * ⚠️ vmid 必须**按十进制打出来**，不能写成 `'0' + (vmid & 0xF)`。
     * 后者在 MAX_VMS=4 时看不出问题，但 vmid 是自增到 255 才回绕的
     * （vm.c 的 g_next_vmid），只要超过 15 就折回来 —— vmid 1 和 17 会同时
     * 显示成 `vm1c0`。MAX_VMS=32 之后这是必然撞的，ps 里两个不同的 vCPU
     * 任务重名，排查时非常误导。
     * 长度上界："vm255" + "c3" = 7 字符，TASK_NAME_LEN=16 富余。
     */
    char name[TASK_NAME_LEN];
    unsigned p = 0;

    name[p++] = 'v';
    name[p++] = 'm';
    {
        char d[4]; /* vmid <= 255 → 最多 3 位 */
        unsigned n = 0, v = vcpu->vm->vmid;

        if (v == 0)
            d[n++] = '0';
        while (v > 0 && n < sizeof(d)) {
            d[n++] = (char)('0' + (v % 10));
            v /= 10;
        }
        while (n > 0)
            name[p++] = d[--n];
    }
    name[p++] = 'c';
    name[p++] = (char)('0' + (vcpu->vcpu_id % 10));
    name[p] = '\0';

    /*
     * ⚠️ 必须用 task_create_affinity()，**不能** task_create() 之后再
     * task_set_cpu_affinity()：后者有窗口 —— 任务在 task_create() 里已经
     * 入队（round-robin 可能挑中 CPU1），若 CPU1 在那两步之间把它挑走执行，
     * sched_dequeue() 就找不到它（RUNNING 不在队列里）→ 紧接着 sched_enqueue()
     * 又把它挂到指定核的队列 → 同一个 vcpu 任务被他核运行着、同时躺在那个核的
     * 队列里，被两个核同时跑，任务状态/运行队列双双写坏。
     * 实测就是 Ctrl+] 停 guest 之后再启动时崩在 sched_schedule 的 pick_next()。
     *
     * ── vCPU 落在哪颗核上：VMM 自己按核轮转（不钉死 cpu0）──────────
     *
     * 三个前提值得写清楚：
     *
     * ① **调度器没有负载均衡**（sched.c 里没有 balance/steal/migrate），
     *    所以任务一旦入队就粘在某颗核上 —— 这不是"跑着跑着被迁走"，而是
     *    "启动时摊开"。也正因如此，per-CPU 硬件状态只要在**每次进 guest 前**
     *    重设就够了（运行中迁移是另一件事，本设计不涉及；task_set_cpu_affinity
     *    的注释也写了这条）。这些重设点早就按 per-CPU 写好了：
     *      x86    : VMXON（s_vmx_on[]）+ VMPTRLD + I/O/MSR bitmap（只读）
     *      aarch64: VTTBR/VTCR + GICv3 的 LR 归属表（按 get_current_cpu_id() 索引）
     *      riscv  : hgatp + hdeleg/hideleg/hstatus（hext_per_hart_csrs_ensure）
     *
     * ② **不要把摊开这件事交给 `CPU_AFFINITY_ANY`** —— 试过，实测摊不开：
     *    sched_enqueue 的 RR 是 `__atomic_fetch_add(&g_rr_counter, 1) % n`，
     *    而每次启动一个 VM 恰好有**偶数**个 ANY 任务入队（vCPU + 另一个），
     *    奇偶恒定 ⇒ 三个 VM 的 vCPU 全落在 cpu0，等于还是钉死的。
     *    判据：日志里连着三行 `on cpu0/2`。
     *    （要试"完全交给调度器"：把下面的 want 换成 CPU_AFFINITY_ANY。）
     *
     * ③ 真摊开之后，多个 VM 的 vCPU 在**不同核上真正并发**跑，而不是挤在一颗
     *    核上时间片轮转。
     *
     * ── 曾经在这里挡路的跨核竞态：VMCS 宿主区里的 TSS 是 cpu0 的 ──────
     *
     * 摊开之后一度出现「三个 guest 都能起到 shell，但宿主 busybox 随后被
     * SIGSEGV 打死」（`User PF CR2=0x1103`，RIP 处的字节根本不是有效代码），
     * 且只在 SMP>1 且 vCPU 跑在非 BSP 核上时出现。根因不在调度器、也不在
     * 栈，而在 VMCS 的**宿主 TR**：
     *
     *   - TSS 是每核一份的（boot/x86_64/tss.c 的 g_tss[]，选择子
     *     0x30 + cpu_id*0x10），而 vmcs_init_host() 原来硬读 GDT[6] ——
     *     那是 **cpu0** 的 TSS 描述符；
     *   - VMCS 宿主区**只会在 VM-exit 时被装回、不会被保存**，所以那个
     *     一次性快照永远留在 VMCS 里，refresh 也从来没管过它；
     *   - 于是 vCPU 在 cpu1 上退出一次之后，cpu1 的 TR 就指向 g_tss[0]。
     *     x86_64 主机的 IDT 门 ist 全是 0（boot/x86_64/exception.c），
     *     ring3→ring0 一律靠 TSS.RSP0 换栈 —— 而 g_tss[0].rsp0 是
     *     **cpu0 当前任务**的内核栈顶。cpu1 上任何一个用户态中断/异常
     *     （定时器、用户缺页）都会把陷阱帧压进 cpu0 那个任务的栈里，
     *     两颗核同时写同一段栈。
     *
     * 修复：x86_tss_current() 取本核 TSS，vmcs_init_host() 和
     * vmx_refresh_host_state() 都用它（后者每次入口前重刷，见那边注释，
     * 同时留了一条不变量自检）。**钉 cpu0 时永远不会复现**，因为那时
     * cpu1 上根本不发生 VM-exit —— 老的双分表里"3 VM 全钉 cpu0 干净"
     * 那一行就是这么来的。（判据是"有没有 VM-exit 落在非 BSP 核上"，
     * 与 VM 个数无关；表里另两行的"干净"是小样本，别当成结论。）
     *
     * 复现：`VMN=3 /tmp/three_vm.sh x86_64 2`（或见 docs/vmm/X86_GUEST_LINUX.md §11.5）。
     */
    /*
     * ── 摊到哪颗核：按「VM 槽位 × MAX_VCPUS + vCPU 编号」轮转 ──────
     *
     * 从前是 `vm->slot % ncpus` —— 单 vCPU 时没问题，多 vCPU 时同一 VM 的
     * 所有兄弟会**全部落在同一颗核**，等于自己把自己串行化了，而且
     * 「每颗 vCPU 在哪颗核」这个视角也就没有意义了。
     *
     * slot*MAX_VCPUS + vcpu_id 保证同一 VM 的兄弟优先分散（只要核数够），
     * 不同 VM 之间再按槽位错开。调度器没有负载均衡，所以这就是最终落点。
     */
    uint32_t ncpu = g_num_cpus ? g_num_cpus : 1U;
    uint32_t want = (uint32_t)(((uint32_t)vcpu->vm->slot * MAX_VCPUS) +
                               (uint32_t)vcpu->vcpu_id) %
                    ncpu;

    /*
     * **先记账、再建任务**：新任务可能在 task_create 返回前就在另一颗核上
     * 跑完并退出，先建任务会让那次 vm_vcpu_stopped() 看到计数为 0、误判
     * 自己是最后一颗。state 也提前推到 RUNNING，避免新任务看到 LOADING。
     */
    vm_vcpu_started(vcpu->vm);
    vm_state_set(vcpu->vm, VM_RUNNING);

    struct task *t =
        task_create_affinity(name, vcpu_task_fn, vcpu, priority, want);
    if (!t) {
        /* 回滚计数，但**不销毁 VM** —— 调用方还要走它自己的错误路径 */
        vm_vcpu_start_abort(vcpu->vm);
        KLOG_ERROR("[vmm] failed to create vcpu%d task\n", vcpu->vcpu_id);
        return NULL;
    }

    /*
     * 不再在这里写 vcpu->task/state 或打日志 —— 本任务可能已经在另一颗核
     * 上跑起来（甚至退出并回收了 VM）。这些字段由 vcpu_task_fn 自己发布，
     * 日志也在那里打。
     *
     * ⚠️ 从前这里还有一句 `g_vmm_stop_requested = 0` —— 那在多 VM 下是
     * 个真 bug：起第二个 VM 会把第一个 VM 尚未被消费的停止请求清掉。
     * 现在停止请求是 per-VM 的（vm->stop_req），创建时就随 vm_alloc 的
     * memset 归零，不需要在这里动别人的。
     */
    return t;
}
