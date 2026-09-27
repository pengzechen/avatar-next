/*
 * kernel/vmm/riscv64/hext_run.c — RISC-V H-extension 架构钩子实现
 *
 * 实现 vmm.h 声明的四个架构钩子：
 *   vmm_arch_restore_guest_ctx  — 进入 guest 循环前初始化 VS-CSRs
 *   vmm_arch_enter_guest        — 调用 hext_enter_guest（sret → VS-mode）
 *   vmm_arch_exit_handler       — 处理 VS-mode → HS-mode 陷阱
 *   vmm_arch_save_guest_ctx     — guest 退出后清理（当前无操作）
 *
 * 以及 RISC-V VMM 初始化：
 *   vmm_arch_vm_init(vm_t *)        — 检查 H-ext 支持，配置 hedeleg/hideleg/hstatus
 *   hext_vcpu_setup(vcpu_t *, entry) — 初始化 vcpu 软件 VMCS
 *
 * 特权级关系：
 *   HS-mode (本内核) → VS-mode (guest) → VU-mode (guest user，本例不用)
 *   VS-mode ecall → scause=10（区别于 U-mode ecall = 8）
 *   WFI 在 hstatus.VTW=1 时：VS-mode 执行 WFI 触发 scause=2（虚拟指令异常）
 *     注：部分 QEMU 版本报 scause=2，另一些直接报 illegal instruction；
 *         本 exit handler 两种都处理。
 */

#include "vmm/vmm.h"
#include "vmm/vmm_console.h"
#include "klog.h"
#include "string.h"
#include "task/task.h"
#include "riscv64/hext.h"
#include "riscv64/sysreg.h"
#include "riscv64/gstage.h"
#include "vmm/vmm_mmio.h"
#include "vmm/vmm_vplic.h"
#include "mm_vm.h"      /* phys_to_virt */
#include "pmm.h"        /* pmm_get_free_pages（缺页失败时的诊断）*/

/* ── 汇编入口声明 ─────────────────────────────────────────── */
extern int hext_enter_guest(vcpu_t *vcpu);  /* hext_vcpu.S */

/* ── H-extension 支持检测 ─────────────────────────────────── */
/*
 * 直接查 misa.H（bit 7），而不是「试探性读一下 hstatus，没炸就算有」。
 *
 * 老写法在**没有** H 扩展的 CPU 上表现极差：csrr hstatus 触发 illegal
 * instruction → 走宿主异常处理 → 通常直接 panic 在启动路径上，报出来的是
 * 一条和虚拟化毫无关系的非法指令，排查方向全错。
 *
 * 实测（QEMU 8.2.2 + OpenSBI v1.3，`-machine virt` 且**不传 -cpu**）：
 * misa 里 H 位是置上的（OpenSBI 打印 "Boot HART Base ISA : rv64imafdch"），
 * 所以默认配置就能用。换 QEMU 版本/CPU 型号后如果这里报错，加
 * `-cpu rv64,h=true` 即可。
 */
static int hext_check_support(void)
{
    /*
     * 先打一行再读 —— 万一读炸了，这行就是日志里的最后一句，
     * 配合紧随其后的 backtrace（会指向 vmm_arch_vm_init）足以定位。
     * 没有 H 扩展时读 hstatus 必触发 illegal instruction，而 S-mode
     * 没有任何办法捕获它（内核的异常处理只会 panic），所以只能这样。
     */
    KLOG_INFO("[HEXT] probing H-extension (reading hstatus)...\n");

    uint64_t hstatus = READ_HSTATUS();

    /*
     * 能执行到这儿就说明 hstatus 可读 ⇒ CPU 带 H 扩展。
     * （没有 H 时 0x600 是未定义 CSR，上一条 csrr 已经陷入并 panic。）
     */
    KLOG_INFO("[HEXT] H-extension available (hstatus=0x%llx)\n",
              (unsigned long long)hstatus);
    return 1;
}

/*
 * ── 每 VM 的 MMIO 总线、虚拟设备与 guest 栈 ────────────────
 *
 * 这三样从前都是文件级 static（g_rv_mmio_bus / g_rv_uart_dev / g_rv_plic_dev
 * / g_guest_stack），整机只有一份 —— 第二个 VM 的 vmm_arch_vm_init() 会在同一个
 * 总线上重复注册设备、覆盖第一个 VM 的设备实例。现在它们都住在 vm_t 里
 * （见 include/vmm/vmm.h 的 ARCH_RISCV64 段）：
 *
 *   vm->mmio_bus_storage  总线本体        （通用字段，aarch64/x86 也有）
 *   vm->uart_dev          虚拟 16550A 设备对象（状态在 vm->uart16550）
 *   vm->plic_dev          虚拟 PLIC 设备对象  （状态在 vm->vplic）
 *   vm->guest_stack[]     给 hext_vcpu_setup 用的 guest 栈
 */

/* ── hext_vcpu_setup：初始化 vcpu 软件 VMCS ──────────────── */
int hext_vcpu_setup(vcpu_t *vcpu, void (*entry)(void))
{
    memset(vcpu->r, 0, sizeof(vcpu->r));

    /* guest PC = entry（内核虚拟地址，与 host 共享地址空间，无 hgatp）
     * pc    = 恢复 PC（sret 目标，只写回 HS sepc）
     * vsepc = guest 自己的 sepc，VMM 不碰；这里给同值只是让 guest 万一在
     *         首次陷阱前读 sepc 时不至于看到 0。*/
    vcpu->pc    = (uint64_t)entry;
    vcpu->vsepc = (uint64_t)entry;

    /* vsstatus：SPP=0（guest 运行在 VS-mode），SIE=0（中断关闭）
     * VS-mode 的 sstatus.SPP 在 vsstatus.SPP 处，bit 8 */
    vcpu->vsstatus  = 0;

    /* guest trap vector：暂用 entry 作为 guest stvec */
    vcpu->vstvec    = (uint64_t)entry;

    /* vsscratch = 0（guest 初始值）*/
    vcpu->vsscratch = 0;

    /* vsatp：与 host 共享内核页表，guest 可执行 kernel VA 地址的代码
     * vsatp=0（bare）时 guest 会把 kernel VA 当物理地址使用，立即 page fault */
    vcpu->vsatp     = CSR_READ(satp);

    /* vsie = 0：guest 中不使能任何中断 */
    vcpu->vsie      = 0;

    /*
     * guest 栈：r[2] = sp。栈从本 VM 的 vm_t 里取 —— 从前是按 vcpu_id 索引
     * 的文件级数组，两个 VM 的 vcpu0 会共用同一块栈。
     */
    vcpu->r[2]      = (uint64_t)(vcpu->vm->guest_stack[vcpu->vcpu_id] +
                                 VMM_GUEST_STACK_SIZE);

    KLOG_INFO("[HEXT] vcpu%d setup: entry=0x%llx sp=0x%llx\n",
              vcpu->vcpu_id,
              vcpu->pc,
              vcpu->r[2]);
    return 0;
}

/* ── vmm_arch_vm_init：VM 初始化（检查 H-ext，配置全局 hstatus）── */
int vmm_arch_vm_init(vm_t *vm)
{
    int nr = vm->cfg.nr_vcpus;
    int i;

    if (nr < 1 || nr > MAX_VCPUS) {
        KLOG_ERROR("[HEXT] vm_init: invalid nr_vcpus=%d\n", nr);
        return -1;
    }

    /* 1. 检查 H-ext 支持 */
    if (!hext_check_support()) {
        KLOG_ERROR("[HEXT] Hypervisor Extension not supported!\n");
        return -1;
    }

    /*
     * 2. 异常/中断委托
     *
     * hedeleg：把「VU-mode 自己该处理」的异常交给 guest 内核（VS-mode）处理。
     *   不委托的话，guest 用户进程每次缺页都会陷到 HS-mode，而我们并没有
     *   把 trap 反射回 VS 的代码 —— 结果是 guest 用户态一跑就死。
     *   Linux 的 initramfs 起来后 /init 一执行就要缺页，所以这一条是必需的。
     *
     *   HEDELEG_COMMON 里**不含** bit2（illegal instruction）：hstatus.VTW=1
     *   时 VS-mode 的 WFI 报 virtual instruction（cause 22），必须陷到 HS-mode
     *   才能被换成宿主 yield —— 委托出去就再也看不见了。
     *
     * hideleg：VS 定时器/外部中断委托给 VS-mode，这样 hvip.VSTIP/VSEIP
     *   置起后由 guest 自己的 vsie 决定何时接收，而不是每置一次就陷一次
     *   HS-mode。
     *
     * hie：置 VSTIE|VSEIE，与 x-kernel hext_init() 的
     *   `csrs hie, HIE_VSEIE | HIE_VSTIE` 一致。它管的**只**是「已委托给
     *   VS 的中断，在 guest 当时接不了时（例如它正跑在自己的陷阱处理程序
     *   入口、vsstatus.SIE=0）怎么办」——置位则升到 HS 让 VMM 抢断，
     *   VMM 让宿主跑一段后 resume。留 0 的话这类中断会一直挂在 hvip 里，
     *   guest 的定时器从此不再推进（实测 guest 完全起不来）。
     *
     *   抢断是安全的，前提是「恢复 PC」与「guest 的 vsepc」分开存放
     *   （VCPU_PC vs VCPU_VSEPC）—— 见 hext_vcpu.S 陷阱向量第 3 节。
     *
     * 历史注记：本文件曾写着「QEMU 8.2 上写这两个 CSR 会 illegal instruction」，
     * 因此长期留着默认值 0。实测是误判 —— binutils 认这两个符号名、QEMU 也
     * 正常接受写入。真正会让 guest 用户态崩溃的是不委托 hedeleg。
     */
    WRITE_HEDELEG(HEDELEG_COMMON);
    WRITE_HIDELEG(HIDELEG_COMMON);
    WRITE_HIE(HIDELEG_COMMON);
    KLOG_INFO("[HEXT] hedeleg=0x%lx hideleg=0x%lx\n",
              (unsigned long)HEDELEG_COMMON, (unsigned long)HIDELEG_COMMON);

    /*
     * 3. hcounteren：允许 guest 直接执行 rdtime/rdcycle/rdinstret。
     *    不给的话 guest 读 time 会非法指令 —— 而 Linux RISC-V 的时钟源和
     *    delay 循环都直接读 time，起不来。
     */
    WRITE_HCOUNTEREN(HCOUNTEREN_CY_TM_IR);

    /*
     * 4. hstatus.VTW=1：WFI 在 VS-mode 陷入 HS-mode
     *    （等价 AArch64 的 HCR_EL2.TWI=1），由 handle_wfi 换成宿主 yield。
     */
    uint64_t hs = READ_HSTATUS();
    hs |= HSTATUS_VTW;
    WRITE_HSTATUS(hs);
    KLOG_INFO("[HEXT] hstatus.VTW set\n");

    /* 5. 初始化 vCPU 数组 */
    for (i = 0; i < nr; i++) {
        vcpu_t *vcpu = &vm->vcpus[i];
        memset(vcpu, 0, sizeof(*vcpu));
        vcpu->vcpu_id  = i;
        vcpu->launched = 0;
        vcpu->vm       = vm;

        /*
         * hstatus_save 是「上次陷入时的 hstatus」快照，入口靠它还原
         * hstatus.SPVP 来决定 sret 回 VS 还是 VU。**第一次进入 guest 时
         * 还没有任何陷入**，快照必须是 0 —— 那会让 SPVP=0，sret 进 VU-mode，
         * guest 的内核代码在用户态取指/执行特权指令，直接全线非法指令。
         *
         * 所以这里预置成 SPVP=1（VS-mode）：第一次进入落在 guest 内核。
         * 这条与 hext_vcpu.S 第 7/8 步是配套的，改那边必看这里。
         */
        vcpu->hstatus_save = HSTATUS_SPVP;
    }
    vm->nr_vcpus = nr;

    /* 6. G-stage（hgatp Stage-2）内存隔离：仅当 VM 请求了独立 guest RAM
     *    （cfg.mem_size != 0）时启用。移植自 x-kernel gstage.rs。
     *
     *    默认（mem_size==0）保持原「共享 host 地址空间、无 hgatp」路径不变，
     *    即当前 vmm_test 使用的已验证行为，避免回归。
     *
     *    ⚠️ 建出来的是**空表**（"空表即全 trap"）：guest RAM 不再预映射，
     *    首次访问才由缺页处理分配物理页并装映射。宿主自己要写的那几块
     *    （内核映像/DTB/initrd/初始栈）由 guest_loader 用
     *    rv_gstage_map_range() 显式映射 —— 与 aarch64 完全同构。 */
    if (vm->cfg.mem_size != 0) {
        rv_gstage_vm_init(&vm->gstage, (uint32_t)vm->slot, vm->vmid,
                          vm->cfg.mem_base, vm->cfg.mem_size);

        /* 建立 MMIO 总线并注册虚拟 16550A 控制台 + 虚拟 PLIC */
        mmio_bus_init(&vm->mmio_bus_storage);
        if (vmm_console_init(vm, &vm->uart_dev, &vm->mmio_bus_storage) != 0)
            KLOG_WARN("[HEXT] console vdev registration failed\n");
        if (vplic_init(vm, &vm->plic_dev, &vm->mmio_bus_storage,
                       (uint32_t)nr) != 0)
            KLOG_WARN("[HEXT] vplic registration failed\n");
        vm->mmio_bus = &vm->mmio_bus_storage;

        KLOG_INFO("[HEXT] vm%u: G-stage isolation enabled (mem=0x%llx+0x%llx)\n",
                  vm->vmid,
                  (unsigned long long)vm->cfg.mem_base,
                  (unsigned long long)vm->cfg.mem_size);
        KLOG_INFO("[HEXT] MMIO bus ready: virtual 16550A @0x%llx, vPLIC @0x%llx\n",
                  (unsigned long long)UART16550_BASE,
                  (unsigned long long)VPLIC_BASE);
    } else {
        KLOG_INFO("[HEXT] G-stage disabled: guest shares host address space\n");
    }

    KLOG_INFO("[HEXT] vm_init: %d vCPU(s) ready\n", nr);
    return 0;
}

/* ================================================================
 * 架构钩子实现
 * ================================================================ */

/*
 * vcpu_timer_irq_on_entry — 进入 guest 前注入 VS 定时器中断
 *
 * 对标 x-kernel vdev/riscv64/timer.rs 的 RiscvTimerHook::on_entry：
 * guest 经 SBI 设置了截止时间且已到期 → 置 hvip.VSTIP，使 VS-mode 看到
 * 定时器中断挂起（无需 vCLINT/ACLINT）。
 *
 * 客人时间与宿主时间同源：两边 timebase 都是 10 MHz（见 platform.conf 的
 * counter_hz 与 guest DTB 的 timebase-frequency），所以 htimedelta 保持 0，
 * guest 的 rdtime 与宿主 READ_TIME() 直接可比。**换平台配置时要一起改**，
 * 否则 guest 的时钟会按错误的倍率走。
 */
static void vcpu_timer_irq_on_entry(vcpu_t *vcpu)
{
    uint64_t now = READ_TIME();
    int pending = (vcpu->timer_deadline != 0 && now >= vcpu->timer_deadline);

    hext_set_vs_timer_irq(pending);
}

/*
 * vcpu_external_irq_on_entry — 进入 guest 前把 vPLIC 的可投递中断同步给 hvip
 *
 * 与定时器同理，也跟 AArch64 的 aarch64_check_vpl011_rx() 同构：
 * 外部中断是**电平触发** —— 只要 PLIC 里还有「已使能、未 active、优先级
 * 高于阈值」的中断，每次入口都要重新置 hvip.VSEIP。只在设备侧 push 时置
 * 一次不行：guest claim 时 vPLIC 会清掉 pending 位，而设备侧（比如 UART 的
 * RX FIFO 里还有字节）可能仍然是有效的。
 */
static void vcpu_external_irq_on_entry(vcpu_t *vcpu)
{
    /* 设备侧先同步进 PLIC：控制台 RX 有数据/待发送 → 置对应中断源 */
    if (vmm_console_irq_asserted(vcpu->vm))
        vplic_set_pending(vcpu->vm, VMM_CONSOLE_IRQ);

    uint32_t irq = vplic_next_deliverable(vcpu->vm, (uint32_t)vcpu->vcpu_id);
    hext_set_vs_external_irq(irq != 0);
}

/*
 * hext_per_hart_csrs_ensure — 把「每 hart 一份」的 HS 级 CSR 补齐
 *
 * vmm_arch_vm_init() 是在 **helper（/bin/vmm-run）所在的 hart** 上跑的，而 vCPU
 * 任务钉在 hart0 上；SMP>1 时两者不是同一颗。下面这些 CSR 都只写了一处
 * （init），于是 vCPU 跑在另一颗 hart 上时它们全是复位值：
 *   hgatp            = Bare → **G-stage 形同关闭**（penalty：guest 直接访问
 *                      宿主物理地址，MMIO 也不再陷入）
 *   hedeleg/hideleg/hie   → guest 的缺页、VS 定时器/外部中断全不委托
 *   hcounteren       → guest 读 time 直接非法指令（Linux 的时钟源就是它）
 *   hstatus.VTW      → VS-mode 的 WFI 不再陷入，宿主 yield 路径失效
 * 症状与 x86 那两个「每核状态只在别的核上初始化」的坑完全一样：SMP=1 正常、
 * SMP>1 概率性起不来（helper 落在哪颗 hart 决定），所以必须在**每次进 guest 前**
 * 在真正跑 vCPU 的 hart 上重设一遍。
 *
 * ⚠️ 多 VM 之后这里还要多一层：hgatp 不只是 per-hart，还是**per-VM** 的。
 * 同一颗 hart 上时间片轮转两个 VM 的 vCPU 任务时，每次入口都得把 hgatp 换回
 * 本 VM 的（否则 guest 会拿着另一个 VM 的 G-stage 跑，症状是"两个 VM 互相
 * 看见对方的内存"这种最不该出现的故障）。所以本函数必须收 vcpu 而不是
 * 不收参数 —— 见下面的 rv_gstage_activate(&vcpu->vm->gstage)。
 */
static void hext_per_hart_csrs_ensure(vcpu_t *vcpu)
{
    WRITE_HEDELEG(HEDELEG_COMMON);
    WRITE_HIDELEG(HIDELEG_COMMON);
    WRITE_HIE(HIDELEG_COMMON);
    WRITE_HCOUNTEREN(HCOUNTEREN_CY_TM_IR);

    uint64_t hs = READ_HSTATUS();
    if (!(hs & HSTATUS_VTW))
        WRITE_HSTATUS(hs | HSTATUS_VTW);

    /*
     * hgatp 换成本 VM 的。幂等：已经是这个值就不写、也不刷 TLB（进 guest 的
     * 常态路径因此只有一次 csrr）。
     */
    if (vcpu->vm)
        rv_gstage_activate(&vcpu->vm->gstage);
}

void vmm_arch_restore_guest_ctx(vcpu_t *vcpu)
{
    /*
     * VS-CSRs 由 hext_enter_guest 汇编逐次恢复，这里做的是「每次入口」的
     * 两件事：每 hart 的 HS 级 CSR 补齐（含本 VM 的 hgatp），以及
     * 「设备 → 虚拟中断控制器」的同步。都必须在**每次**入口做，不能只在
     * 启动时做一次。
     */
    hext_per_hart_csrs_ensure(vcpu);

    vcpu_timer_irq_on_entry(vcpu);
    vcpu_external_irq_on_entry(vcpu);

}

/* enter_guest：执行一次 sret → VS-mode，等到 guest 陷入后返回 */
int vmm_arch_enter_guest(vcpu_t *vcpu)
{
    /*
     * 每次 task_yield 后需要重设 hstatus.SPV+SPVP 和 sstatus.SPP。
     * 这些已在 hext_enter_guest 汇编内完成，此处只需调用。
     *
     * 虚拟中断的注入不在这里：它在 vmm_arch_restore_guest_ctx()，与本函数
     * 同属「每次入口」的钩子，但由 vmm_run_vcpu 在**关中断之后**统一调用。
     */
    int ret = hext_enter_guest(vcpu);
    if (ret) {
        vcpu->launched = 1;
    }
    return ret;
}

/* ── WFI 陷阱处理（scause=2/22: virtual instruction，hstatus.VTW=1）──
 *
 * 移植自 x-kernel arch/riscv64/mod.rs handle_wfi：若 guest 通过 SBI 设置了
 * 定时器截止且已到期，直接 resume；否则让出 CPU。avatar 无
 * interruptible_sleep_until，用 task_yield 近似（定时器中断会周期性唤醒）。
 */
static int handle_wfi(vcpu_t *vcpu)
{
    /*
     * 只有 WFI 才该走这里。
     *
     * hstatus.VTW=1 时 VS-mode 的 WFI 报 virtual instruction（cause 22），
     * stval 里是 WFI 的编码 0x10500073。cause 2（illegal instruction）在
     * 老 QEMU 上也被用来报同一个陷阱，所以两者都收。
     *
     * 但**不能无条件当成 WFI 跳过 4 字节** —— 别的虚拟指令异常也会是
     * cause 22，盲目 +4 会把 guest 的 PC 挪到错的地方（而且压缩指令只有
     * 2 字节，+4 会多吃一条）。这里对不上就记一笔再照常跳过 4 字节，
     * 至少让这种事在日志里留痕。
     */
    if (vcpu->stval_save != 0x10500073ULL)
        KLOG_WARN_SAMPLE("[HEXT] cause2/22 but not WFI: stval=0x%llx sepc=0x%llx\n",
                         (unsigned long long)vcpu->stval_save,
                         (unsigned long long)vcpu->pc);

    /* 步进 guest PC 越过 WFI 指令（4 字节）*/
    vcpu->pc += 4;

    uint64_t now = READ_TIME();
    if (vcpu->timer_deadline != 0 && now >= vcpu->timer_deadline)
        return EL2_RESUME;   /* 定时器已到期，立即继续 guest */

    /* 让出 CPU，等价 AArch64 WFI→yield */
    task_yield();
    return EL2_RESUME;
}

/*
 * ── guest 控制台输出（SBI console_putchar）─────────────────
 *
 * 走与控制台 vdev 的 THR 通路相同的出口（见 vmm_console_putchar）：
 * helper 模式下必须进 TX 缓冲，否则 guest 在 8250 驱动起来之前的那几行
 * 会漏进宿主内核日志。
 */
static void sbi_console_putchar(vcpu_t *vcpu, uint8_t byte)
{
    vmm_console_putchar(vcpu->vm, byte);
}

/* ================================================================
 * MMIO 陷入模拟（移植自 x-kernel arch/riscv64/mod.rs）
 *
 * G-stage 把设备 IPA 置为无效后，guest 访问设备 → G-stage page fault
 * （cause 20/21/23），htval 给出 guest 物理地址。VMM 需：
 *   1. 从 guest PC 取指（经 guest 页表 vsatp 翻译 VA→GPA，再经 G-stage）
 *   2. 解码出访存指令的 方向/宽度/寄存器
 *   3. 交给 MMIO 总线分发，读结果写回 guest 寄存器，步进 PC
 * ================================================================ */

/*
 * ── guest 物理地址读取（GPA → HPA → 内核直接映射）────────────
 *
 * ⚠️ 翻译必须走**查表**（rv_gstage_lookup），不能再用「GPA 减窗口基址」那种
 * 算术换算。老版本那样写是因为 guest RAM 被整段预留、identity 映射，
 * GPA 与 HPA 之间有个固定偏移；改成按需分页之后每一页都是各自从 PMM 分配
 * 的，那个偏移根本不存在 —— 继续用算术换算会读到**别的 VM 或宿主内核**的
 * 物理内存（没有任何报错，只是数据是错的）。
 *
 * 查不到就是查不到：调用方（MMIO 取指/解码）要靠这个返回值决定放弃。
 * 注意这里**不会**触发缺页分配 —— 缺页只由 guest 自己的访问驱动，
 * 宿主读 guest 内存是另一回事，读到未映射地址说明 guest 的状态本身不对。
 */
static int guest_read_u64(vcpu_t *vcpu, uint64_t gpa, uint64_t *out)
{
    uint64_t hpa;
    if (!vcpu->vm || !rv_gstage_lookup(&vcpu->vm->gstage, gpa, &hpa))
        return 0;
    *out = *(volatile uint64_t *)phys_to_virt(hpa);
    return 1;
}

static int guest_read_u32(vcpu_t *vcpu, uint64_t gpa, uint32_t *out)
{
    uint64_t hpa;
    if (!vcpu->vm || !rv_gstage_lookup(&vcpu->vm->gstage, gpa, &hpa))
        return 0;
    *out = *(volatile uint32_t *)phys_to_virt(hpa);
    return 1;
}

static int guest_read_u16(vcpu_t *vcpu, uint64_t gpa, uint16_t *out)
{
    uint64_t hpa;
    if (!vcpu->vm || !rv_gstage_lookup(&vcpu->vm->gstage, gpa, &hpa))
        return 0;
    *out = *(volatile uint16_t *)phys_to_virt(hpa);
    return 1;
}

/* guest 虚拟地址 → guest 物理地址（软件遍历 vsatp 指向的 Sv39/Sv48 页表）*/
static int guest_va_to_gpa(vcpu_t *vcpu, uint64_t va, uint64_t *gpa_out)
{
    const uint64_t PTE_V = 1u << 0, PTE_R = 1u << 1, PTE_W = 1u << 2, PTE_X = 1u << 3;

    uint64_t satp = vcpu->vsatp;
    uint64_t mode = satp >> 60;
    int levels;

    switch (mode) {
    case 8:  levels = 3; break;   /* Sv39 */
    case 9:  levels = 4; break;   /* Sv48 */
    case 10: levels = 5; break;   /* Sv57 */
    default:
        *gpa_out = va;   /* 无页表：VA 即 GPA（bare 模式）*/
        return 1;
    }

    if (satp == 0) {
        *gpa_out = va;
        return 1;
    }

    uint64_t table_gpa = (satp & ((1ULL << 44) - 1)) << 12;

    for (int level = levels - 1; level >= 0; level--) {
        uint64_t vpn = (va >> (12 + level * 9)) & 0x1FF;
        uint64_t pte;

        if (!guest_read_u64(vcpu, table_gpa + vpn * 8, &pte))
            return 0;
        if ((pte & PTE_V) == 0)
            return 0;
        if ((pte & PTE_W) && !(pte & PTE_R))
            return 0;   /* 保留组合 */

        if (pte & (PTE_R | PTE_X)) {
            /* 叶项：4KiB(level0) / 2MiB / 1GiB */
            uint64_t ppn = (pte >> 10) & ((1ULL << 44) - 1);
            uint64_t page_mask = (1ULL << (12 + level * 9)) - 1;
            *gpa_out = (ppn << 12) | (va & page_mask);
            return 1;
        }
        table_gpa = ((pte >> 10) & ((1ULL << 44) - 1)) << 12;
    }
    return 0;
}

/* 一条 MMIO 访存指令的解码结果 */
typedef struct {
    int      is_write;
    uint8_t  size;      /* 字节数 */
    uint32_t reg;       /* 目标/源寄存器号 */
    uint64_t inst_len;  /* 指令长度（2=压缩, 4=普通）*/
} mmio_access_t;

static int decode_compressed_mmio(uint16_t inst, int is_store,
                                  mmio_access_t *acc)
{
    uint32_t opcode = inst & 0x3;
    uint32_t funct3 = (inst >> 13) & 0x7;

    if (!is_store) {
        if (opcode == 0 && (funct3 == 2 || funct3 == 3)) {
            acc->is_write = 0;
            acc->size = (funct3 == 2) ? 4 : 8;
            acc->reg = ((inst >> 2) & 0x7) + 8;
            acc->inst_len = 2;
            return 1;
        }
        if (opcode == 2 && (funct3 == 2 || funct3 == 3)) {
            acc->is_write = 0;
            acc->size = (funct3 == 2) ? 4 : 8;
            acc->reg = (inst >> 7) & 0x1F;
            acc->inst_len = 2;
            return 1;
        }
    } else {
        if (opcode == 0 && (funct3 == 6 || funct3 == 7)) {
            acc->is_write = 1;
            acc->size = (funct3 == 6) ? 4 : 8;
            acc->reg = ((inst >> 2) & 0x7) + 8;
            acc->inst_len = 2;
            return 1;
        }
        if (opcode == 2 && (funct3 == 6 || funct3 == 7)) {
            acc->is_write = 1;
            acc->size = (funct3 == 6) ? 4 : 8;
            acc->reg = (inst >> 2) & 0x1F;
            acc->inst_len = 2;
            return 1;
        }
    }
    return 0;
}

static int decode_mmio_access(uint64_t inst, int is_store, mmio_access_t *acc)
{
    /* 压缩指令（低 2 位 != 11）先处理 */
    if ((inst & 0x3) != 0x3)
        return decode_compressed_mmio((uint16_t)inst, is_store, acc);

    uint32_t w = (uint32_t)inst;
    uint32_t opcode = w & 0x7f;
    uint32_t funct3 = (w >> 12) & 0x7;

    if (opcode == 0x03 && !is_store) {          /* LOAD */
        acc->is_write = 0;
        switch (funct3) {
        case 0: case 4: acc->size = 1; break;
        case 1: case 5: acc->size = 2; break;
        case 2: case 6: acc->size = 4; break;
        case 3:         acc->size = 8; break;
        default: return 0;
        }
        acc->reg = (w >> 7) & 0x1F;
        acc->inst_len = 4;
        return 1;
    }
    if (opcode == 0x23 && is_store) {           /* STORE */
        acc->is_write = 1;
        switch (funct3) {
        case 0: acc->size = 1; break;
        case 1: acc->size = 2; break;
        case 2: acc->size = 4; break;
        case 3: acc->size = 8; break;
        default: return 0;
        }
        acc->reg = (w >> 20) & 0x1F;
        acc->inst_len = 4;
        return 1;
    }
    return 0;
}

/* 取 guest PC 处的指令；取指失败时回落到 htinst */
static int mmio_instruction(vcpu_t *vcpu, uint64_t *inst_out)
{
    uint64_t pc_gpa;

    if (guest_va_to_gpa(vcpu, vcpu->pc, &pc_gpa)) {
        uint16_t lo;
        if (guest_read_u16(vcpu, pc_gpa, &lo)) {
            if ((lo & 0x3) != 0x3) {
                *inst_out = lo;      /* 压缩指令 */
                return 1;
            }
            uint32_t full;
            if (guest_read_u32(vcpu, pc_gpa, &full)) {
                *inst_out = full;
                return 1;
            }
        }
    }

    /* 回退取 htinst（不是 htval！）：htval 是出错 GPA>>2，当指令解码会解出
     * 完全无关的 opcode/长度/寄存器号 —— 偶发的 guest 跑飞。*/
    if (vcpu->htinst_save != 0) {
        *inst_out = vcpu->htinst_save;
        return 1;
    }
    return 0;
}

/*
 * ── G-stage 缺页：先分 RAM / MMIO，再各自处理 ───────────────
 *
 * 出错 GPA 的取法：htval 保存的是 **GPA >> 2**，低两位拿不回来；页内偏移
 * 从 stval 取。两者拼起来是对的 —— 因为 VS-stage 与 G-stage 的翻译都是
 * 页粒度的，虚拟地址的 [11:0] 与 guest 物理地址的 [11:0] 恒等，而 stval
 * 给的正是那个虚拟地址（GVA=1 时）。
 *
 * ⚠️ 别把它和 htinst 搞混：htinst 是"触发陷阱的那条指令"，用来解码 MMIO
 * 访问；拿 htval 当指令解会解出完全无关的 opcode/长度/寄存器号（见
 * mmio_instruction 的说明）。
 */
static inline uint64_t gstage_fault_gpa(const vcpu_t *vcpu)
{
    return (vcpu->htval_save << 2) | (vcpu->stval_save & 0xFFF);
}

/*
 * handle_ram_fault — RAM 窗口内的 G-stage 缺页：分配物理页 + 装映射
 *
 * 这是**按需分页**的落点。返回 EL2_RESUME 表示映射已装好，guest 重跑同一条
 * 指令即可；失败返回 EL2_EXIT（PMM 没页 / 没有 VM）。
 *
 * ⚠️ **不推进 guest PC** —— 这正是"缺页"与"MMIO 模拟"的分野：MMIO 那条路
 * 是 VMM 替 guest 完成了这次访问，所以必须把 PC 挪过去（否则无限重复）；
 * 这里只是把内存补上，访问本身还得 guest 自己重做一次。两条路走反了的表现
 * 分别是"死循环"和"跳过一条随机指令"，都很难查。
 *
 * ⚠️ 一次装整个 2 MiB 块（512 页）而不是一页：每次缺页都要一次完整的
 * HS-mode 往返，而 guest 启动期是密集触碰内存的。aarch64 那边实测把 1447 次
 * 缺页压到 6 次。代价是最多 2 MiB 的过取。
 */
static int handle_ram_fault(vcpu_t *vcpu, uint64_t gpa, uint64_t code)
{
    vm_t *vm = vcpu->vm;
    uint64_t n;

    if (!vm)
        return EL2_EXIT;

    n = rv_gstage_map_block(&vm->gstage, gpa, 1 /* 清零，防跨 VM 数据泄漏 */);
    if (n == 0) {
        KLOG_ERROR("[HEXT] vcpu%d: RAM fault at gpa=0x%llx but PMM is out "
                   "of pages (free=%llu), stopping vm%u\n",
                   vcpu->vcpu_id, (unsigned long long)gpa,
                   (unsigned long long)pmm_get_free_pages(g_pmm),
                   vm->vmid);
        return EL2_EXIT;
    }

    vm->gstage.nr_fault += n;

    /*
     * 装完必须刷 G-stage TLB。
     *
     * aarch64 那边省掉了这一步（"刚缺页就说明这条翻译本来没缓存"），riscv
     * 这里保留：hfence.gvma 就是一条指令，而 RISC-V 规范明确允许实现缓存
     * "不可翻译"的结果 —— 省掉它就等于把"能不能跑"押在具体实现的行为上。
     */
    rv_gstage_tlb_flush(&vm->gstage);

    /* 抽样打印，便于按地址对账（首次启动的几条最有诊断价值）*/
    KLOG_INFO_SAMPLE("[HEXT] vm%u: gstage %s fault gpa=0x%llx -> block "
                     "+%llu pages (total fault=%llu)\n",
                     vm->vmid, (code == 23) ? "store" : "load",
                     (unsigned long long)gpa, (unsigned long long)n,
                     (unsigned long long)vm->gstage.nr_fault);
    return EL2_RESUME;
}

/* ── G-stage MMIO fault（cause 20/21/23）──────────────────── */
static int handle_gstage_fault(vcpu_t *vcpu, uint64_t code)
{
    uint64_t gpa = gstage_fault_gpa(vcpu);
    int is_store = (code == 23);
    uint64_t inst;
    mmio_access_t acc;

    if (!mmio_instruction(vcpu, &inst)) {
        KLOG_ERROR("[HEXT] vcpu%d: cannot fetch MMIO inst, gpa=0x%llx "
                   "pc=0x%llx htinst=0x%llx\n",
                   vcpu->vcpu_id, (unsigned long long)gpa,
                   (unsigned long long)vcpu->pc,
                   (unsigned long long)vcpu->htinst_save);
        return EL2_EXIT;
    }
    if (!decode_mmio_access(inst, is_store, &acc)) {
        /* gpa 一定要打：解不出来时第一个要问的就是"guest 在访问哪"，
         * 少了它只能看到一串像随机数的指令编码（实测踩过）。*/
        KLOG_ERROR("[HEXT] vcpu%d: undecodable MMIO inst=0x%llx cause=%llu "
                   "gpa=0x%llx pc=0x%llx\n",
                   vcpu->vcpu_id, (unsigned long long)inst,
                   (unsigned long long)code, (unsigned long long)gpa,
                   (unsigned long long)vcpu->pc);
        return EL2_EXIT;
    }

    uint64_t value = acc.is_write ? vcpu->r[acc.reg] : 0;
    uint64_t out = 0;

    if (vcpu->vm && vcpu->vm->mmio_bus &&
        mmio_bus_handle(vcpu->vm->mmio_bus, gpa, acc.is_write, acc.size,
                        value, (uint32_t)vcpu->vcpu_id, &out)) {
        if (!acc.is_write && acc.reg != 0)
            vcpu->r[acc.reg] = out;
        vcpu->pc += acc.inst_len;
        return EL2_RESUME;
    }

    KLOG_ERROR("[HEXT] vcpu%d: unhandled G-stage %s fault gpa=0x%llx pc=0x%llx\n",
               vcpu->vcpu_id, acc.is_write ? "write" : "read",
               (unsigned long long)gpa, (unsigned long long)vcpu->pc);
    return EL2_EXIT;
}

/* ── VS-mode ecall 处理（scause=10）─────────────────────────
 *
 * 移植自 x-kernel arch/riscv64/mod.rs handle_vs_ecall：
 *   - guest 测试魔数 PRINT/DONE
 *   - SBI legacy：set_timer / console_putchar / console_getchar
 *   - SBI base：spec version / impl id / probe extension
 *   - SBI TIME.set_timer / RFENCE（单 vCPU 下为 no-op 成功）
 */
static int handle_vs_ecall(vcpu_t *vcpu)
{
    uint64_t ext  = vcpu->r[17];  /* a7 = SBI EID / hypercall 号 */
    uint64_t func = vcpu->r[16];  /* a6 = SBI FID                */
    uint64_t arg0 = vcpu->r[10];  /* a0 = 第一个参数             */

    /* 步进 guest PC 越过 ecall（4 字节） */
    vcpu->pc += 4;

    switch (ext) {
    case GUEST_ECALL_PRINT:
        /* 留在 INFO（test-vmm 的通过证据），抽稀成有界输出 */
        KLOG_INFO_SAMPLE("[HEXT] ECALL_PRINT: iter=%llu (vcpu%d)\n",
                         (unsigned long long)arg0, vcpu->vcpu_id);
        return EL2_RESUME;

    case GUEST_ECALL_DONE:
        KLOG_INFO("[HEXT] ECALL_DONE: vcpu%d exiting\n", vcpu->vcpu_id);
        return EL2_VMEXIT;

    case SBI_LEGACY_SET_TIMER:
        vcpu->timer_deadline = arg0;
        return EL2_RESUME;

    case SBI_LEGACY_CONSOLE_PUTCHAR:
        sbi_console_putchar(vcpu, (uint8_t)arg0);
        return EL2_RESUME;

    case SBI_LEGACY_CONSOLE_GETCHAR:
        /* 无输入时按 SBI 约定返回 -1（不是 0 —— 0 是合法字符 '\0'）*/
        vcpu->r[10] = (uint64_t)-1;
        return EL2_RESUME;

    case SBI_LEGACY_SHUTDOWN:
        KLOG_INFO("[HEXT] guest requested shutdown (legacy SBI)\n");
        return EL2_VMEXIT;

    case SBI_EXT_BASE: {
        uint64_t value;
        switch (func) {
        case SBI_BASE_GET_SPEC_VERSION: value = 0x00000002; break; /* v0.2 */
        case SBI_BASE_GET_IMPL_ID:      value = 0x41565452; break; /* "AVTR" */
        case SBI_BASE_GET_IMPL_VERSION: value = 1;          break;
        /* Linux 会读这三个填 /proc/cpuinfo，返回 0 表示「未知」是合法的 */
        case SBI_BASE_GET_MVENDORID:
        case SBI_BASE_GET_MARCHID:
        case SBI_BASE_GET_MIMPID:       value = 0;          break;
        case SBI_BASE_PROBE_EXTENSION:
            switch (arg0) {
            case SBI_EXT_BASE:
            case SBI_EXT_TIME:
            case SBI_EXT_RFENCE:
            case SBI_EXT_SRST:
            case SBI_LEGACY_SET_TIMER:
            case SBI_LEGACY_CONSOLE_PUTCHAR:
            case SBI_LEGACY_CONSOLE_GETCHAR:
            case SBI_LEGACY_SHUTDOWN:
                value = 1; break;
            /*
             * IPI / HSM 明确报「不支持」。
             *
             * 本 VMM 只有 vCPU0 一个 hart（guest DTB 里也只有一个 cpu@0），
             * 没有 hart_start 的实现，报支持会让 guest 在 SMP 初始化时
             * 真的去启从核，然后永远等不到它上线 —— 那种挂起比「不支持」
             * 难查得多。单核 guest 不探测这两个扩展也能正常启动。
             */
            default:
                value = 0; break;
            }
            break;
        default:
            vcpu->r[10] = SBI_ERR_NOT_SUPPORTED;
            vcpu->r[11] = 0;
            return EL2_RESUME;
        }
        vcpu->r[10] = SBI_SUCCESS;
        vcpu->r[11] = value;
        return EL2_RESUME;
    }

    case SBI_EXT_TIME:
        if (func == SBI_TIME_SET_TIMER) {
            vcpu->timer_deadline = arg0;
            /* 新截止时间通常在将来，先撤掉可能还挂着的定时器中断，
             * 否则 guest 会立刻再吃一次（下一次入口会按新的 deadline 重算）*/
            hext_set_vs_timer_irq(0);
            vcpu->r[10] = SBI_SUCCESS;
            vcpu->r[11] = 0;
            return EL2_RESUME;
        }
        vcpu->r[10] = SBI_ERR_NOT_SUPPORTED;
        vcpu->r[11] = 0;
        return EL2_RESUME;

    case SBI_EXT_RFENCE:
        /* 单 vCPU 无远端 hart 需同步；报告成功以兼容会探测 RFENCE 的 guest。*/
        vcpu->r[10] = SBI_SUCCESS;
        vcpu->r[11] = 0;
        return EL2_RESUME;

    case SBI_EXT_SRST:
        /*
         * SRST（复位/关机）。Linux 在 poweroff/reboot 时调用 FID 0。
         * 收到就当作 guest 正常退出 —— 这也是 guest 里敲 `poweroff` 之后
         * 宿主能拿回控制权的路径（/dev/vmm 的 helper 会看到 EPOLLHUP）。
         *
         * 必须先回一次成功（结果寄存器已写好），再返回 VMEXIT：反过来的话
         * guest 会带着未定义的 a0 继续跑。
         */
        if (func == SBI_SRST_RESET) {
            KLOG_INFO("[HEXT] guest requested %s via SRST\n",
                      arg0 == SBI_SRST_RESET_TYPE_SHUTDOWN ? "shutdown"
                                                           : "reset");
            vcpu->r[10] = SBI_SUCCESS;
            vcpu->r[11] = 0;
            return EL2_VMEXIT;
        }
        vcpu->r[10] = SBI_ERR_NOT_SUPPORTED;
        vcpu->r[11] = 0;
        return EL2_RESUME;

    default:
        /*
         * 未实现的扩展一律回 NOT_SUPPORTED（不是崩溃）。
         * 新版 Linux 会探测 DBCN/CPPC/PMU 等，回 -2 它就安静地换别的路径。
         */
        KLOG_WARN_ONCE("[HEXT] unimplemented SBI ext=0x%llx func=0x%llx\n",
                       (unsigned long long)ext, (unsigned long long)func);
        vcpu->r[10] = SBI_ERR_NOT_SUPPORTED;
        vcpu->r[11] = 0;
        return EL2_RESUME;
    }
}

/* exit_handler：处理一次 guest → HS-mode 陷阱 */
int vmm_arch_exit_handler(vcpu_t *vcpu)
{
    uint64_t cause = vcpu->scause_save;
    int      is_int = (int)(cause >> 63);
    uint64_t code  = cause & ~(1ULL << 63);

    /* 对标 x-kernel vdev/riscv64/timer.rs 的 RiscvTimerHook::on_exit：
     * 已退出 guest，清除注入的 VS 定时器/外部中断挂起位，避免残留导致下次
     * 进入 guest 立即重复触发。两者都是电平触发，下一次入口会按当时的设备
     * 与 vPLIC 状态重新计算（见 vmm_arch_restore_guest_ctx）。*/
    hext_set_vs_timer_irq(0);
    hext_set_vs_external_irq(0);

    if (is_int) {
        /* 中断（定时器/外部）陷入 HS-mode：短暂让给宿主内核处理，然后
         * resume。对标 kvmm exit_handler 的 is_interrupt 分支。
         *
         * 注：hideleg 已把 VS 定时器/外部中断委托给 VS-mode，所以正常情况
         * 下**走不到这里** —— 中断在 guest 内部就被消化了。能到这里的是
         * 宿主自己（HS 侧）的中断，比如宿主时钟。 */
        KLOG_DEBUG("[HEXT] vcpu%d host interrupt: code=%llu\n",
                   vcpu->vcpu_id, (unsigned long long)code);
        /* 直接 yield，让 kernel 的 timer handler 运行 */
        task_yield();
        return EL2_RESUME;
    }

    switch (code) {
    case 2:
    case 22:
        /*
         * scause=2 (Illegal instruction) 或 22 (Virtual instruction)：
         * hstatus.VTW=1 时 VS-mode 的 WFI 陷入此处。不同 QEMU 版本报 2 或 22，
         * 两者都当 WFI 处理（步进 + 按定时器截止 yield/resume）。
         */
        return handle_wfi(vcpu);

    case CAUSE_VS_ECALL:   /* 10 */
        return handle_vs_ecall(vcpu);

    case 20:   /* Instruction G-stage Page Fault */
    case 21:   /* Load G-stage Page Fault      */
    case 23:   /* Store/AMO G-stage Page Fault */
    {
        uint64_t gpa = gstage_fault_gpa(vcpu);

        /*
         * ── 先分 RAM / MMIO ────────────────────────────────────
         *
         * 按需分页之后 G-stage 表初始是空的，所以 guest 取指/访存**每一次
         * 落到新页**都会走到这里。RAM 窗口内的走缺页分配，窗口外的才是
         * 设备 MMIO，交给总线模拟。
         *
         * ⚠️ 这个分支必须在最前面。老版本没有它，20/21/23 一律按 MMIO 处理
         * —— 对 RAM 地址取指会去"解码一条根本不存在的指令"，而访存则是
         * 查表未命中 → EL2_EXIT → **整个 VM 停机**。缺页因此表现为
         * "guest 启动到一半毫无征兆地没了"，而不是一个缺页。
         */
        if (vcpu->vm && rv_gstage_ipa_is_ram(&vcpu->vm->gstage, gpa))
            return handle_ram_fault(vcpu, gpa, code);

        /*
         * 非 RAM 的取指：本 VMM 不做取指 MMIO 模拟（没有需要执行代码段的
         * 设备），所以这一定是 guest 跑飞了 —— 跳进了设备地址或未映射区间。
         * 报出地址比让 handle_gstage_fault 去解码「一条根本不存在的指令」
         * 有用得多。
         */
        if (code == 20) {
            KLOG_ERROR("[HEXT] vcpu%d: guest fetched from unmapped GPA 0x%llx "
                       "(vsepc=0x%llx)\n",
                       vcpu->vcpu_id, (unsigned long long)gpa,
                       (unsigned long long)vcpu->pc);
            return EL2_EXIT;
        }

        /* 设备 MMIO：G-stage 未映射 → 陷入模拟（移植自 kvmm mod.rs）*/
        return handle_gstage_fault(vcpu, code);
    }

    case 12:   /* Instruction Page Fault（guest 自己的页表缺项）*/
    case 13:   /* Load Page Fault */
    case 15:   /* Store Page Fault */
        /*
         * VS-mode 自己取指/访存缺页 —— guest 内核页表的问题，VMM 帮不上忙。
         *
         * 从 VU-mode 来的缺页**不该**出现在这里：hedeleg 已经把 12/13/15
         * 委托给 VS-mode，由 guest 内核自己填页表。真在这儿看到它们，通常
         * 意味着 guest 内核在 VS-mode 里踩了没映射的地址。
         */
        KLOG_ERROR("[HEXT] vcpu%d guest page fault: cause=%llu vsepc=0x%llx "
                   "stval=0x%llx\n",
                   vcpu->vcpu_id,
                   (unsigned long long)code,
                   (unsigned long long)vcpu->pc,
                   (unsigned long long)vcpu->stval_save);
        return EL2_EXIT;

    default:
        KLOG_ERROR("[HEXT] vcpu%d unhandled exit: cause=%llu vsepc=0x%llx\n",
                   vcpu->vcpu_id,
                   (unsigned long long)code,
                   (unsigned long long)vcpu->pc);
        return EL2_EXIT;
    }
}

/* save_guest_ctx：guest 退出后保存上下文（当前无额外操作）*/
void vmm_arch_save_guest_ctx(vcpu_t *vcpu)
{
    (void)vcpu;
}
