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
#include "vmm/vmm_console.h"   /* vmm_console_pump（直启模式的输入桥）*/
#include "klog.h"
#include "task/task.h"
#include "task/sched.h"    /* sched_check_and_yield：主循环顶部主动让出 CPU */
#include "task/cpu.h"      /* g_num_cpus：vCPU 摊核时用 */
#if ARCH_AARCH64
#include "aarch64/stage2.h"      /* stage2_activate：每轮进 guest 前重写 VTTBR */
#include "vmm/vmm_irq_route.h"   /* vmm_irq_route_clear_owner */
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
        /* 每次回到宿主都顺手收一次控制台输入（见 vmm_console_pump）*/
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

    KLOG_INFO("[vmm] vcpu%d task started\n", vcpu->vcpu_id);

    int rc = vmm_run_vcpu(vcpu);

    /*
     * 先把状态推到 DYING，再走收尾。
     *
     * /dev/vmm 的 bootlinux 靠 state 判断"旧 guest 是否已收尾"（从前是一个
     * 全局 running 标志）。标记成 DYING 之后才做那些可能阻塞/打印的清理，
     * 这样外部不会在收尾中途看到"还活着"的假象。
     */
    vm_t *vm = vcpu->vm;
    vm->state = VM_DYING;

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
     * 回收这个 VM 的槽位（连同它的 stage-2 按需页）。
     *
     * **必须由 vCPU 任务自己调、而且必须在 task_exit() 之前** —— 池的并发
     * 规则是"跨任务只传 vmid"，谁也不能在任务还活着的时候把这个槽位清掉；
     * 而 task_exit() 之后本任务就没了，再没人能安全地做这件事。
     */
    /*
     * 打印这个 vCPU 任务的内核栈高水位 —— 定 TASK_STACK_SIZE 的依据就是它。
     * 栈是静态数组挨着放的，溢出会先踩坏**邻居**任务而不是自己，所以这个
     * 数字比"崩没崩"可靠得多（见 task.h 里 TASK_STACK_SIZE 的说明）。
     */
    {
        extern size_t task_stack_used(const struct task *);
        size_t used = task_stack_used(task_current());
        KLOG_INFO("[vmm] vcpu%d kernel stack high-water: %zu / %u bytes\n",
                  vcpu->vcpu_id, used, (unsigned)TASK_STACK_SIZE);
    }

    vm_free(vm);

    task_exit();
}

struct task *vcpu_task_create(vcpu_t *vcpu, uint8_t priority)
{
    char name[TASK_NAME_LEN];
    name[0] = 'v'; name[1] = 'c'; name[2] = 'p'; name[3] = 'u';
    name[4] = '0' + (char)(vcpu->vcpu_id & 0xF);
    name[5] = '\0';

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
    uint32_t want = (uint32_t)vcpu->vm->slot % (g_num_cpus ? g_num_cpus : 1U);

    struct task *t = task_create_affinity(name, vcpu_task_fn, vcpu, priority,
                                          want);
    if (t) {

        /*
         * 从这里起就算「这个 VM 在跑」：/dev/vmm 的 write/poll 会立刻看到，
         * 不必等任务真正被调度上 CPU。
         *
         * ⚠️ 从前这里还有一句 `g_vmm_stop_requested = 0` —— 那在多 VM 下是
         * 个真 bug：起第二个 VM 会把第一个 VM 尚未被消费的停止请求清掉。
         * 现在停止请求是 per-VM 的（vm->stop_req），创建时就随 vm_alloc 的
         * memset 归零，不需要在这里动别人的。
         */
        vcpu->vm->state = VM_RUNNING;

        KLOG_INFO("[vmm] vcpu%d task created (id=%u) on cpu%u/%u\n",
                  vcpu->vcpu_id, t->id, t->cpu_affinity, g_num_cpus);
    } else {
        KLOG_ERROR("[vmm] failed to create vcpu%d task\n", vcpu->vcpu_id);
    }
    return t;
}
