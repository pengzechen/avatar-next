/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/task/cpu.c — Per-CPU 控制块 + BSP / 次级核启动

   三架构的次级核拉起都在这里收敛到 cpu_secondary_bootstrap()：
     - aarch64: PSCI CPU_ON
     - riscv64: SBI HSM hart_start(不支持的平台回退到 release 表 + IPI)
     - x86_64 : INIT-SIPI + AP trampoline
   每个次级核装好 per-CPU 寄存器、idle 任务和本地 timer 后开中断，
   进入 idle 循环，与 BSP 一样参与调度和跨核唤醒。
*/

#include "task/cpu.h"
#include "task/task.h"
#include "task/sched.h"
#include "task/switch.h" /* arch_irq_enable */
#include "assert.h"
#include "klog.h"
#include "string.h"
#include "arch.h"
#include "barrier.h"
#include "driver/time.h" /* timer_delay_ms (供 SMP 自检使用, 所有架构通用) */
#include "driver/irq.h"  /* resched IPI / 控制器 per-CPU 初始化 */

#if ARCH_AARCH64
#include "mm/mm_vm.h"     /* KERNEL_VMA / virt_to_phys */
#include "aarch64/neon.h" /* aarch64_enable_neon (per-core CPACR_EL1.FPEN) */
#include "aarch64/psci.h" /* psci_cpu_on / psci_affinity_info(SMC64) */
#endif

#if ARCH_RISCV64
#include "exception.h"
#include "mm/mm_vm.h"
#include "riscv64/sbi.h" /* sbi_hart_start / sbi_send_ipi(HSM/IPI) */
#include "riscv64/sysreg.h"
extern void trap_vector(void);
volatile uint64_t riscv64_boot_hartid;
#endif

#if ARCH_X86_64
#include "exception.h"
#include "irq/lapic.h"
extern void x86_tss_init_cpu(uint32_t cpu_id, uint64_t rsp0);
#endif

/* ── 全局 CPU 池 ────────────────────────────────────────── */
cpu_t g_cpus[AVATAR_MAX_CPUS];
uint32_t g_num_cpus = 1; /* 已在线 CPU 数(BSP=1, 每成功拉起一个 AP +1) */

/* ── 跨核 resched IPI ───────────────────────────────────── */

/*
   resched_ipi_handler — 收到 IPI 只做一件事：置本核 need_resched

   真正的切换发生在中断返回路径(sched_check_and_yield_from_trap)或
   vCPU 主循环顶部(sched_check_and_yield)。在 ISR 里直接切任务是禁止的,
   见 docs/INTERRUPT_CONTEXT_SWITCH.md。

   RISC-V 的 SSIP 是电平型 pending 位、x86 的 LAPIC 需要 EOI，两者都由
   控制器后端的 dispatch 收尾(PLIC 清 SSIP / LAPIC 写 EOI)，这里只置位。
*/
static void resched_ipi_entry(void *frame, void *ctx)
{
    cpu_t *c = cpu_current();

    (void)frame;
    (void)ctx;
    c->need_resched = true;
}

void cpu_ipi_init(void)
{
    /* IPI 号/向量由控制器决定，内核不再关心 SGI 15 / scause 1 / 向量 0xF0 */
    if (irq_register_ipi(resched_ipi_entry, NULL) != 0)
        KLOG_WARN("[cpu] resched IPI registration failed\n");
}

void cpu_send_ipi(uint32_t cpu_id)
{
    if (cpu_id >= g_num_cpus || cpu_id >= AVATAR_MAX_CPUS)
        return;
    if (!g_cpus[cpu_id].online)
        return;

    /* hw_id: aarch64=MPIDR, riscv=hartid, x86=LAPIC ID */
    irq_send_ipi((uint32_t)g_cpus[cpu_id].hw_id);
}

/*
   klog_cpu_id - 给 klog.h 宏使用, 返回当前逻辑 CPU 编号。

   早期 boot(BSP 还没装 per-CPU 寄存器)arch_cpu_self_get 返回 0,
   cpu_current() 退化到 &g_cpus[0], cpu_id=0(BSS 默认值), 正确。
*/
uint32_t klog_cpu_id(void)
{
    return cpu_current()->cpu_id;
}

/*
   cpu_bump_local_ticks - timer ISR 每次调用, 让本核累计 tick 计数器 +1。
   用于 SMP 健康检查：cpu_smp_timer_test 会读所有核的计数器, 确认都在动。
*/
void cpu_bump_local_ticks(void)
{
    cpu_current()->local_ticks++;
}

/* ── BSP 初始化 ─────────────────────────────────────────── */
void cpu_init_bsp(void)
{
    cpu_t *c = &g_cpus[0];
    memset(c, 0, sizeof(*c));

    c->cpu_id = 0;
#if ARCH_X86_64
    /* x86 的 hw_id 是 LAPIC ID：arch_cpu_hw_id() 只返回 0 占位，BSP 也要
       走控制器后端读真实值，否则真实硬件上 BSP 的 APIC ID != 0 时，
       sched_resched_cpu(0) 会把 IPI 发到错误的核。 */
    c->hw_id = irq_cpu_hw_id();
#else
    c->hw_id = arch_cpu_hw_id();
#endif
    c->current_task = NULL;
    c->idle_task = NULL;
    c->need_resched = false;
    sched_rq_init(c);
    spinlock_irq_init(&c->rq_lock);

    /* 安装 per-CPU 寄存器(TPIDR_EL1 / tp / GS_BASE) */
    arch_cpu_self_set(c);

    g_num_cpus = 1;

    /* 本核的 resched IPI 就绪；必须在 online 置位(任何人都可能开始 kick)之前完成。 */
    cpu_ipi_init();

    wmb();
    c->online = true;

    KLOG_SMP("[cpu] BSP initialized: cpu_id=0 hw_id=0x%llx\n", (unsigned long long)c->hw_id);
}

/* ── 二级核启动 ─────────────────────────────────────────── */
#define SECONDARY_STACK_SIZE 16384U

/* 各架构 boot.S 中定义的次级核启动栈, 共享供 idle 复用。 */
extern uint8_t secondary_boot_stacks[];
/* 每个次级核的 idle 任务控制块(CPU0 用 task.c 里的 g_idle_task)。 */
static task_t g_secondary_idle[AVATAR_MAX_CPUS - 1];

#if ARCH_AARCH64

/* boot/aarch64/boot.S 中定义的次级核入口(链接地址在高 VA) */
extern void _secondary_start(void);

/* GIC CPU interface 初始化(每核都要做一次；distributor 由 BSP 初始化) */
extern void gic_init_secondary(void);

/* psci_cpu_on / psci_affinity_info 在 include/aarch64/psci.h(static inline) */

#endif /* ARCH_AARCH64 */

/*
   setup_secondary_idle_task - 初始化次级核 idle 任务控制块

   复用 secondary_boot_stacks[cpu_id] 作为 idle 的内核栈(次级核此刻
   的 SP 就指向那块 SECONDARY_STACK_SIZE 大小(16KB)的栈；切换到自己时
   只是恢复同一区域)。

   必须在第一次允许中断 / 第一次调用 sched_schedule 之前完成,
   否则 sched_check_and_yield 会读到 NULL current_task 而崩溃。
*/
static void setup_secondary_idle_task(cpu_t *c)
{
    task_t *idle = &g_secondary_idle[c->cpu_id - 1];
    memset(idle, 0, sizeof(*idle));

    idle->state = TASK_RUNNING;
    idle->on_cpu = true;                /* 次级核此刻就在 idle 上执行 */
    idle->id = 0xFFFF0000U | c->cpu_id; /* 用高位段避免与 g_task_id_cnt 冲突 */
    idle->priority = 255;
    idle->stack_base = &secondary_boot_stacks[c->cpu_id * SECONDARY_STACK_SIZE];
    idle->entry = NULL;
    idle->arg = NULL;
    idle->cpu_affinity = c->cpu_id;
    idle->fs_base = 0;
    /*
       idle 也需要 files/fs/mm。files_alloc() 会把 fd 表显式填成 -1 ——
       不能靠 memset 归零：0 是合法的 fd 池槽位号，fd_table_inherit()
       会把它当成「256 个都指向槽位 0 的 fd」，一次继承就把
       FD_POOL_SIZE(128) 个槽全部吃光（实测过：之后宿主里所有 open()
       都失败）。CPU0 的 idle 走 task.c 的 g_idle_task，同样在
       task_init() 里分配。
    */
    assert_always(task_objects_alloc(idle));
    list_node_init(&idle->run_node);
    list_node_init(&idle->wait_node);

    /* 名字："idle/N" */
    idle->name[0] = 'i';
    idle->name[1] = 'd';
    idle->name[2] = 'l';
    idle->name[3] = 'e';
    idle->name[4] = '/';
    idle->name[5] = '0' + (char)(c->cpu_id % 10);
    idle->name[6] = '\0';

    c->idle_task = idle;
    c->current_task = idle;
}

#if ARCH_AARCH64

/*
   cpu_secondary_bootstrap - 次级核 C 入口

   由 _secondary_start 在以下条件成立后调用：
     - EL2/VHE 已配置(与 BSP 一致)
     - MMU 已开启, 共享 BSP 的内核 PGD
     - VBAR_EL1 已指向 exception_vector_base
     - DAIF.I 仍为关闭

   启动步骤：
     1. 安装 per-CPU 指针 + GIC CPU interface
     2. 建立本核 idle 任务(current_task = idle_task)
     3. 启用本地 timer PPI(CNTP_CTL_EL0)
     4. 通知 BSP online, 开 IRQ, 进入 idle 循环(sched_check_and_yield + wfi)

   run_queue 为空时 sched_schedule 会一直选回自己的 idle；跨核 enqueue
   与 resched IPI 会把本核唤醒去跑别的任务。
*/
void cpu_secondary_bootstrap(uint32_t cpu_id)
{
    cpu_t *c = &g_cpus[cpu_id];

#if ARCH_AARCH64
    /* 每核独立寄存器：必须在跑任何 FP/SIMD 之前打开 CPACR_EL1.FPEN,
       否则 EL0 上 musl 启动早期访问 NEON 会触发 EC=0x7 trap。
       BSP 在 kernel_main 入口已做同样操作；AP 必须在此处补上。
    */
    aarch64_enable_neon();
#endif

    /* 安装 TPIDR_EL1, 使 cpu_current() 在本核返回正确指针 */
    arch_cpu_self_set(c);
    c->hw_id = arch_cpu_hw_id();

    /* 控制器 per-CPU 初始化(GIC: PMR/CTLR/GICR；PLIC/LAPIC: 无操作) */
    irq_secondary_init();

    /*
       必须在开中断之前装好 current_task, 否则首个 timer ISR 进入
       sched_check_and_yield 会读到 NULL。
    */
    setup_secondary_idle_task(c);

    /* 启用本地 timer PPI(CNTP_TVAL/CTL 与 GICD ISENABLER0)。 */
    timer_init_secondary();

    /* resched IPI：必须在 online 之前装好, 否则 BSP 发来的 kick 会丢。 */
    cpu_ipi_init();

    /* 通知 BSP 本核就绪。wmb 确保前面所有写对其他核可见。 */
    wmb();
    c->online = true;

    /* 开 IRQ；之后 timer / resched IPI 会驱动本核的 sched_check_and_yield。 */
    arch_irq_enable();

    /*
       Idle 循环：主动 yield 一次再 wfi。yield 在 run_queue 空时是空操作
       (sched_schedule 选回自己)；远端入队会先置 need_resched 并打断 wfi，
       下一轮循环即可拿到新任务。
    */
    for (;;) {
        sched_check_and_yield();
        __asm__ volatile("wfi");
    }
}

void cpu_bring_up_all(void)
{
    if (CONFIG_SMP_CPUS <= 1U) {
        return;
    }

    /* _secondary_start 链接在高 VA；PSCI 需要物理入口地址。 */
    uint64_t entry_phys = virt_to_phys((uint64_t)(uintptr_t)&_secondary_start);

    KLOG_SMP("[cpu] SMP bringup: CONFIG_SMP_CPUS=%u entry_phys=0x%llx\n", (unsigned)CONFIG_SMP_CPUS,
             (unsigned long long)entry_phys);

    for (uint32_t i = 1; i < CONFIG_SMP_CPUS && i < AVATAR_MAX_CPUS; i++) {
        cpu_t *c = &g_cpus[i];
        memset(c, 0, sizeof(*c));
        c->cpu_id = i;
#if defined(PLATFORM_RK3588)
        /* RK3588: Aff0 恒为 0, 各核用 Aff1 区分, MPIDR = cpu_idx << 8 */
        c->hw_id = (uint64_t)i << 8;
#else
        c->hw_id = i; /* QEMU virt: MPIDR.Aff0 = CPU 序号 */
#endif
        c->online = false;
        sched_rq_init(c);
        spinlock_irq_init(&c->rq_lock);

        KLOG_SMP("[cpu] PSCI CPU_ON cpu=%u mpidr=0x%llx\n", i, (unsigned long long)c->hw_id);

        int64_t ret = psci_cpu_on(/* mpidr= */ c->hw_id, entry_phys,
                                  /* context_id= */ i);
        KLOG_SMP("[cpu] PSCI CPU_ON cpu=%u ret=%lld\n", i, (long long)ret);
        if (ret != PSCI_RET_SUCCESS) {
            KLOG_ERROR("[cpu] PSCI CPU_ON cpu=%u failed: ret=%lld\n", i, (long long)ret);
            continue;
        }

        /* 自旋等待该核置 online；带超时避免死等无效 mpidr。 */
        uint64_t spin = 0;
        while (!c->online) {
            __asm__ volatile("yield");
            if (++spin > 100000000ULL) {
                int64_t aff = psci_affinity_info(c->hw_id);
                KLOG_ERROR("[cpu] timeout waiting for cpu%u online, "
                           "PSCI_AFFINITY_INFO=%lld (0=ON,1=OFF,2=PENDING)\n",
                           i, (long long)aff);
                break;
            }
        }
        if (c->online) {
            g_num_cpus++;
            KLOG_SMP("[cpu] cpu%u online (hw_id=0x%llx)\n", i, (unsigned long long)c->hw_id);
        }
    }

    KLOG_SMP("[cpu] SMP bringup done: %u CPU(s) online\n", (unsigned)g_num_cpus);
}

#elif ARCH_RISCV64

extern const uint64_t riscv64_hart_release_phys;
extern volatile uint64_t riscv64_boot_hartid;

extern const uint64_t riscv64_secondary_start_phys;

/* sbi_hart_start / sbi_send_ipi 在 include/riscv64/sbi.h(static inline) */

void cpu_secondary_bootstrap(uint32_t cpu_id)
{
    cpu_t *c = &g_cpus[cpu_id];

    arch_cpu_self_set(c);
    CSR_SET(sstatus, 1UL << 18); /* SUM */
    WRITE_STVEC((uint64_t)trap_vector);

    setup_secondary_idle_task(c);
    timer_init_secondary();

    /* resched IPI：必须在 online 之前装好, 否则 BSP 发来的 kick 会丢。 */
    cpu_ipi_init();

    wmb();
    c->online = true;

    arch_irq_enable();
    for (;;) {
        sched_check_and_yield();
        __asm__ volatile("wfi");
    }
}

void cpu_bring_up_all(void)
{
    if (CONFIG_SMP_CPUS <= 1U)
        return;

#if defined(PLATFORM_SG2002)
    KLOG_WARN("[cpu] RISC-V SMP bringup disabled on SG2002 for now\n");
    return;
#endif

    KLOG_SMP("[cpu] RISC-V SMP bringup: CONFIG_SMP_CPUS=%u boot_hart=%llu\n", (unsigned)CONFIG_SMP_CPUS,
             (unsigned long long)riscv64_boot_hartid);
    volatile uint64_t *hart_release = (volatile uint64_t *)phys_to_virt(riscv64_hart_release_phys);
    uint64_t entry_phys = riscv64_secondary_start_phys;
    bool hsm_unavailable = false;

    for (uint32_t i = 1; i < CONFIG_SMP_CPUS && i < AVATAR_MAX_CPUS; i++) {
        cpu_t *c = &g_cpus[i];
        uint64_t hartid = i - 1U;
        if (hartid == riscv64_boot_hartid)
            hartid = CONFIG_SMP_CPUS - 1U;

        memset(c, 0, sizeof(*c));
        c->cpu_id = i;
        c->hw_id = hartid;
        c->online = false;
        sched_rq_init(c);
        spinlock_irq_init(&c->rq_lock);

        KLOG_SMP("[cpu] start/release cpu=%u hart=%llu\n", i, (unsigned long long)c->hw_id);

        hart_release[c->hw_id] = (uint64_t)i + 1ULL;
        wmb();

        if (!hsm_unavailable) {
            long ret = sbi_hart_start(c->hw_id, entry_phys, i);
            KLOG_SMP("[cpu] SBI hart_start cpu=%u ret=%ld\n", i, ret);
            if (ret != SBI_SUCCESS)
                hsm_unavailable = true;
        }

        long ipi_ret = hsm_unavailable ? sbi_send_ipi(c->hw_id) : SBI_SUCCESS;
        if (hsm_unavailable && ipi_ret != SBI_SUCCESS) {
            KLOG_WARN("[cpu] SBI send_ipi hart=%llu ret=%ld\n", (unsigned long long)c->hw_id, ipi_ret);
        }

        uint64_t spin = 0;
        while (!c->online) {
            __asm__ volatile("nop");
            if (++spin > 100000000ULL) {
                KLOG_ERROR("[cpu] timeout waiting for cpu%u hart=%llu online\n", i, (unsigned long long)c->hw_id);
                break;
            }
        }
        if (c->online) {
            g_num_cpus++;
            KLOG_SMP("[cpu] cpu%u online (hart=%llu)\n", i, (unsigned long long)c->hw_id);
        }
    }

    KLOG_SMP("[cpu] RISC-V SMP bringup done: %u CPU(s) online\n", (unsigned)g_num_cpus);
}

#elif ARCH_X86_64

#define X86_AP_TRAMP_PHYS     0x00007000UL
#define X86_AP_TRAMP_VECTOR   (X86_AP_TRAMP_PHYS >> 12)
#define X86_AP_DATA_CPU_ID    ((volatile uint32_t *)0x00007f00UL)
#define X86_AP_DATA_STACK_TOP ((volatile uint64_t *)0x00007f08UL)

extern uint8_t x86_ap_trampoline_start[];
extern uint8_t x86_ap_trampoline_end[];

void cpu_secondary_bootstrap(uint32_t cpu_id)
{
    cpu_t *c = &g_cpus[cpu_id];

    arch_cpu_self_set(c);
    c->hw_id = irq_cpu_hw_id();

    exception_init_secondary();
    setup_secondary_idle_task(c);
    x86_tss_init_cpu(cpu_id, (uint64_t)(uintptr_t)&secondary_boot_stacks[(cpu_id + 1U) * SECONDARY_STACK_SIZE]);
    timer_init_secondary();

    /* resched IPI：必须在 online 之前装好, 否则 BSP 发来的 kick 会丢。 */
    cpu_ipi_init();

    wmb();
    c->online = true;

    arch_irq_enable();
    for (;;) {
        sched_check_and_yield();
        __asm__ volatile("hlt");
    }
}

void cpu_bring_up_all(void)
{
    if (CONFIG_SMP_CPUS <= 1U)
        return;

    uintptr_t tramp_size = (uintptr_t)(x86_ap_trampoline_end - x86_ap_trampoline_start);
    if (tramp_size > 0x1000U) {
        KLOG_ERROR("[cpu] x86 AP trampoline too large: %llu bytes\n", (unsigned long long)tramp_size);
        return;
    }

    volatile uint8_t *tramp_dst = (volatile uint8_t *)X86_AP_TRAMP_PHYS;
    for (uintptr_t j = 0; j < tramp_size; j++)
        tramp_dst[j] = x86_ap_trampoline_start[j];

    KLOG_SMP("[cpu] x86_64 SMP bringup: CONFIG_SMP_CPUS=%u trampoline=0x%lx size=%llu\n", (unsigned)CONFIG_SMP_CPUS,
             (unsigned long)X86_AP_TRAMP_PHYS, (unsigned long long)tramp_size);

    for (uint32_t i = 1; i < CONFIG_SMP_CPUS && i < AVATAR_MAX_CPUS; i++) {
        cpu_t *c = &g_cpus[i];
        memset(c, 0, sizeof(*c));
        c->cpu_id = i;
        c->hw_id = i;
        c->online = false;
        sched_rq_init(c);
        spinlock_irq_init(&c->rq_lock);

        *X86_AP_DATA_CPU_ID = i;
        *X86_AP_DATA_STACK_TOP = (uint64_t)(uintptr_t)&secondary_boot_stacks[(i + 1U) * SECONDARY_STACK_SIZE];
        wmb();

        KLOG_SMP("[cpu] INIT/SIPI cpu=%u apic_id=%llu\n", i, (unsigned long long)c->hw_id);
        lapic_send_init((uint32_t)c->hw_id);
        timer_delay_ms(10);
        lapic_send_sipi((uint32_t)c->hw_id, (uint8_t)X86_AP_TRAMP_VECTOR);
        timer_delay_ms(1);
        lapic_send_sipi((uint32_t)c->hw_id, (uint8_t)X86_AP_TRAMP_VECTOR);

        uint64_t spin = 0;
        while (!c->online) {
            __asm__ volatile("pause");
            if (++spin > 100000000ULL) {
                KLOG_ERROR("[cpu] timeout waiting for cpu%u apic_id=%llu online\n", i, (unsigned long long)c->hw_id);
                break;
            }
        }
        if (c->online) {
            g_num_cpus++;
            KLOG_SMP("[cpu] cpu%u online (apic_id=%llu)\n", i, (unsigned long long)c->hw_id);
        }
    }

    KLOG_SMP("[cpu] x86_64 SMP bringup done: %u CPU(s) online\n", (unsigned)g_num_cpus);
}

#endif /* ARCH_AARCH64 / ARCH_RISCV64 / ARCH_X86_64 */

/* ── SMP 自检：验证所有在线核的 timer ISR 都在动 ──────────

   流程：BSP 用 timer_delay_ms 忙等若干轮(不依赖调度器), 每轮快照
   g_cpus[i].local_ticks, 对比上一轮 → 全核都增长则 PASS, 否则 FAIL。

   只依赖次级核的本地 timer 已经启用(timer_init_secondary 完成并 online),
   不依赖 sched_tick 回调；主流程在 timer_set_tick_cb 之后调用。

   全核必须共享一致的时间基准；aarch64 是 CNTPCT_EL0, per-core 但同源
   振荡器, timer_delay_ms 在任何核上读数一致。
*/
extern volatile uint64_t g_system_ticks; /* driver/time/timer.c */

#if ARCH_AARCH64
/* 诊断：读 DAIF, CNTP_CTL_EL0, CNTPCT_EL0 */
static inline uint64_t dbg_read_cntp_ctl(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntp_ctl_el0" : "=r"(v));
    return v;
}
static inline uint64_t dbg_read_cntpct(void)
{
    return timer_read_counter();
}
#endif

void cpu_smp_timer_test(uint32_t rounds, uint32_t ms_per_round)
{
    /* 即使单核也跑诊断：先确认 BSP 自己的 timer 是否在 tick。 */

    /*
       该函数在 task_switch_to_idle_stack() 之前被调用, 此时 DAIF.I=1(IRQ 关)。
       不先开 IRQ, BSP 就永远收不到 timer 中断, local_ticks 一定为 0。
    */
#if ARCH_AARCH64 || ARCH_RISCV64
    arch_irq_enable();
#endif

#if ARCH_AARCH64
    {
        uint64_t daif = arch_irq_flags(); /* 统一的中断状态读取 */
        uint64_t ctl = dbg_read_cntp_ctl();
        uint64_t pct = dbg_read_cntpct();
        KLOG_SMP("[smp-test] BSP DAIF=0x%llx CNTP_CTL=0x%llx CNTPCT=%llu\n", (unsigned long long)daif,
                 (unsigned long long)ctl, (unsigned long long)pct);

        /* 控制器相关的诊断寄存器由后端自己 dump，内核不再知道是 GICv2 还是 GICv3 */
        irq_debug_dump();
    }
#endif

    KLOG_SMP("[smp-test] timer health: %u cpu(s), %u rounds x %u ms\n", (unsigned)g_num_cpus, (unsigned)rounds,
             (unsigned)ms_per_round);

    uint64_t prev[AVATAR_MAX_CPUS];
    for (uint32_t i = 0; i < g_num_cpus; i++) {
        prev[i] = g_cpus[i].local_ticks;
    }
    uint64_t prev_sys = g_system_ticks;

    bool all_ok = true;

    for (uint32_t r = 0; r < rounds; r++) {
        timer_delay_ms(ms_per_round);

#if ARCH_AARCH64
        uint64_t pct_now = dbg_read_cntpct();
        uint64_t ctl_now = dbg_read_cntp_ctl();
        KLOG_SMP("[smp-test] round=%u CNTPCT=%llu CNTP_CTL=0x%llx\n", (unsigned)r, (unsigned long long)pct_now,
                 (unsigned long long)ctl_now);
#endif

        uint64_t sys_now = g_system_ticks;
        KLOG_SMP("[smp-test] round=%u system_ticks=%llu (+%llu)\n", (unsigned)r, (unsigned long long)sys_now,
                 (unsigned long long)(sys_now - prev_sys));
        prev_sys = sys_now;

        for (uint32_t i = 0; i < g_num_cpus; i++) {
            uint64_t now = g_cpus[i].local_ticks;
            uint64_t diff = now - prev[i];
            KLOG_SMP("[smp-test] round=%u cpu%u local_ticks=%llu (+%llu)\n", (unsigned)r, (unsigned)i,
                     (unsigned long long)now, (unsigned long long)diff);
            if (diff == 0ULL) {
                all_ok = false;
            }
            prev[i] = now;
        }
    }

    if (all_ok) {
        KLOG_SMP("[smp-test] PASS: all %u cpu(s) timer ticking\n", (unsigned)g_num_cpus);
    } else {
        KLOG_ERROR("[smp-test] FAIL: some cpu(s) timer stuck\n");
    }
}
