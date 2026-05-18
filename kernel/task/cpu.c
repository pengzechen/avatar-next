/*
 * kernel/task/cpu.c — Per-CPU 控制块 + BSP / 二级核启动骨架
 *
 * Phase 0：仅 BSP 初始化。
 * Phase 2：aarch64 通过 PSCI CPU_ON 拉起次级核，次级核进入
 *          cpu_secondary_bootstrap() 后只完成 per-CPU 寄存器 +
 *          GIC CPU interface 初始化，然后 wfi idle（不参与调度，
 *          调度集成留给 Phase 3+）。
 *
 * riscv64 / x86_64 SMP 拉起在后续 Phase 实现，本文件保留 stub。
 */

#include "task/cpu.h"
#include "task/task.h"
#include "task/sched.h"
#include "task/switch.h"     /* arch_irq_enable */
#include "klog.h"
#include "string.h"
#include "arch.h"
#include "timer/timer.h"     /* timer_delay_ms (供 SMP 自检使用，所有架构通用) */

#if ARCH_AARCH64
#include "mm_vm.h"          /* KERNEL_VMA / virt_to_phys */
#include "barrier.h"
#include "aarch64/cpu.h"    /* aarch64_enable_neon (per-core CPACR_EL1.FPEN) */
#include "irq/irq.h"        /* irq_init_secondary() */
#endif

/* ── 全局 CPU 池 ────────────────────────────────────────── */
cpu_t   g_cpus[AVATAR_MAX_CPUS];
uint32_t g_num_cpus = 1;     /* Phase 0：恒为 1；Phase 2 起会增长 */

/*
 * klog_cpu_id - 给 klog.h 宏使用，返回当前逻辑 CPU 编号。
 *
 * 早期 boot（BSP 还没装 per-CPU 寄存器）arch_cpu_self_get 返回 0，
 * cpu_current() 退化到 &g_cpus[0]，cpu_id=0（BSS 默认值），正确。
 */
uint32_t
klog_cpu_id(void)
{
    return cpu_current()->cpu_id;
}

/*
 * cpu_bump_local_ticks - timer ISR 每次调用，让本核累计 tick 计数器 +1。
 * 用于 SMP 健康检查：cpu_smp_timer_test 会读所有核的计数器，确认都在动。
 */
void
cpu_bump_local_ticks(void)
{
    cpu_current()->local_ticks++;
}

/* ── BSP 初始化 ─────────────────────────────────────────── */
void
cpu_init_bsp(void)
{
    cpu_t *c = &g_cpus[0];
    memset(c, 0, sizeof(*c));

    c->cpu_id       = 0;
    c->hw_id        = arch_cpu_hw_id();
    c->online       = true;
    c->current_task = NULL;
    c->idle_task    = NULL;
    c->need_resched = false;
    list_init(&c->run_queue);
    spinlock_irq_init(&c->rq_lock);

    /* 安装 per-CPU 寄存器（TPIDR_EL1 / tp / GS_BASE） */
    arch_cpu_self_set(c);

    g_num_cpus = 1;

    KLOG_INFO("[cpu] BSP initialized: cpu_id=0 hw_id=0x%llx\n",
              (unsigned long long)c->hw_id);
}

/* ── 二级核启动 ─────────────────────────────────────────── */

#if ARCH_AARCH64

/* boot/aarch64/boot.S 中定义的次级核入口（链接地址在高 VA） */
extern void _secondary_start(void);

/* boot/aarch64/boot.S 中定义的次级核启动栈：每核 4KB，共享供 idle 复用 */
extern uint8_t secondary_boot_stacks[];
#define SECONDARY_STACK_SIZE  4096U

/* 每个次级核的 idle 任务控制块（CPU0 用 task.c 里的 g_idle_task）。 */
static task_t g_secondary_idle[AVATAR_MAX_CPUS - 1];

/* GIC CPU interface 初始化（每核都要做一次；distributor 由 BSP 初始化） */
extern void gic_init_secondary(void);

/* PSCI 0.2+ 功能号：SMC64 CPU_ON */
#define PSCI_CPU_ON_AARCH64        0xC4000003UL
#define PSCI_AFFINITY_INFO_AARCH64 0xC4000004UL

static int64_t
psci_cpu_on(uint64_t mpidr, uint64_t entry_phys, uint64_t context_id)
{
    register uint64_t x0 __asm__("x0") = PSCI_CPU_ON_AARCH64;
    register uint64_t x1 __asm__("x1") = mpidr;
    register uint64_t x2 __asm__("x2") = entry_phys;
    register uint64_t x3 __asm__("x3") = context_id;

    __asm__ volatile("smc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3)
                     : "memory");
    return (int64_t)x0;
}

/*
 * PSCI_AFFINITY_INFO 查询指定核状态：
 *   0 = ON
 *   1 = OFF
 *   2 = ON_PENDING
 *   <0 = error (例如 -2 INVALID_PARAMS 表示 ATF 不认该 mpidr)
 */
static int64_t
psci_affinity_info(uint64_t mpidr)
{
    register uint64_t x0 __asm__("x0") = PSCI_AFFINITY_INFO_AARCH64;
    register uint64_t x1 __asm__("x1") = mpidr;
    register uint64_t x2 __asm__("x2") = 0; /* lowest_affinity_level = 0 */

    __asm__ volatile("smc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2)
                     : "memory");
    return (int64_t)x0;
}

/*
 * setup_secondary_idle_task - 初始化次级核 idle 任务控制块
 *
 * 复用 secondary_boot_stacks[cpu_id] 作为 idle 的内核栈（次级核此刻
 * 的 SP 就指向那块 4KB；切换到自己时只是恢复同一区域）。
 *
 * 必须在第一次允许中断 / 第一次调用 sched_schedule 之前完成，
 * 否则 sched_check_and_yield 会读到 NULL current_task 而崩溃。
 */
static void
setup_secondary_idle_task(cpu_t *c)
{
    task_t *idle = &g_secondary_idle[c->cpu_id - 1];
    memset(idle, 0, sizeof(*idle));

    idle->state        = TASK_RUNNING;
    idle->id           = 0xFFFF0000U | c->cpu_id; /* 用高位段避免与 g_task_id_cnt 冲突 */
    idle->priority     = 255;
    idle->stack_base   = &secondary_boot_stacks[c->cpu_id * SECONDARY_STACK_SIZE];
    idle->entry        = NULL;
    idle->arg          = NULL;
    idle->cpu_affinity = c->cpu_id;
    idle->fs_base      = 0;
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

    c->idle_task    = idle;
    c->current_task = idle;
}

/*
 * cpu_secondary_bootstrap - 次级核 C 入口
 *
 * 由 _secondary_start 在以下条件成立后调用：
 *   - EL2/VHE 已配置（与 BSP 一致）
 *   - MMU 已开启，共享 BSP 的内核 PGD
 *   - VBAR_EL1 已指向 exception_vector_base
 *   - DAIF.I 仍为关闭
 *
 * Phase 3：
 *   1. 安装 per-CPU 指针 + GIC CPU interface
 *   2. 建立本核 idle 任务（current_task = idle_task）
 *   3. 启用本地 timer PPI（CNTP_CTL_EL0）
 *   4. 通知 BSP online，开 IRQ，进入 idle 循环（task_yield + wfi）
 *
 * 此阶段次级核的 run_queue 始终为空：sched_schedule 会一直 pick 到自己
 * 的 idle，行为退化为带 IRQ 的 WFI。跨核 enqueue / IPI 留给 Phase 4+。
 */
void
cpu_secondary_bootstrap(uint32_t cpu_id)
{
    cpu_t *c = &g_cpus[cpu_id];

#if ARCH_AARCH64
    /* 每核独立寄存器：必须在跑任何 FP/SIMD 之前打开 CPACR_EL1.FPEN，
     * 否则 EL0 上 musl 启动早期访问 NEON 会触发 EC=0x7 trap。
     * BSP 在 kernel_main 入口已做同样操作；AP 必须在此处补上。 */
    aarch64_enable_neon();
#endif

    /* 安装 TPIDR_EL1，使 cpu_current() 在本核返回正确指针 */
    arch_cpu_self_set(c);
    c->hw_id = arch_cpu_hw_id();

    /* GIC CPU interface（PMR / CTLR / GICH 等，per-core 寄存器） */
    irq_init_secondary();
    // KLOG_WARN(">>>>");

    /*
     * Phase 3.1/3.2：必须在开中断之前装好 current_task，否则首个
     * timer ISR 进入 sched_check_and_yield 会读到 NULL。
     */
    setup_secondary_idle_task(c);

    /* Phase 3.3：启用本地 timer PPI（CNTP_TVAL/CTL 与 GICD ISENABLER0）。*/
    timer_init_secondary();

    timer_enable();

    /* 通知 BSP 本核就绪。wmb 确保前面所有写对其他核可见。 */
    wmb();
    c->online = true;

    /* Phase 3.4：开 IRQ；之后 timer 会驱动本核的 sched_check_and_yield。 */
    arch_irq_enable();

    /*
     * Idle 循环：主动 yield 一次再 wfi。yield 在 run_queue 空时是空操作
     * （sched_schedule 选回自己），但为后续 Phase 4 跨核唤醒留好钩子：
     * 那时 wake-up IPI 会先令 need_resched=true，wfi 被打断后 yield 即可
     * 拿到 IPI 投放进来的任务。
     */
    for (;;) {
        sched_check_and_yield();
        __asm__ volatile("wfi");
    }
}

void
cpu_bring_up_all(void)
{
    if (CONFIG_SMP_CPUS <= 1U) {
        return;
    }

    /* _secondary_start 链接在高 VA；PSCI 需要物理入口地址。 */
    uint64_t entry_phys = virt_to_phys((uint64_t)(uintptr_t)&_secondary_start);

    KLOG_INFO("[cpu] SMP bringup: CONFIG_SMP_CPUS=%u entry_phys=0x%llx\n",
              (unsigned)CONFIG_SMP_CPUS, (unsigned long long)entry_phys);

    for (uint32_t i = 1; i < CONFIG_SMP_CPUS && i < AVATAR_MAX_CPUS; i++) {
        cpu_t *c = &g_cpus[i];
        memset(c, 0, sizeof(*c));
        c->cpu_id = i;
#if defined(PLATFORM_RK3588)
        /* RK3588: Aff0 恒为 0，各核用 Aff1 区分，MPIDR = cpu_idx << 8 */
        c->hw_id  = (uint64_t)i << 8;
#else
        c->hw_id  = i;             /* QEMU virt: MPIDR.Aff0 = CPU 序号 */
#endif
        c->online = false;
        list_init(&c->run_queue);
        spinlock_irq_init(&c->rq_lock);

        KLOG_INFO("[cpu] PSCI CPU_ON cpu=%u mpidr=0x%llx\n",
                  i, (unsigned long long)c->hw_id);

        int64_t ret = psci_cpu_on(/*mpidr=*/c->hw_id, entry_phys,
                                  /*context_id=*/i);
        KLOG_INFO("[cpu] PSCI CPU_ON cpu=%u ret=%lld\n",
                  i, (long long)ret);
        if (ret != 0) {
            KLOG_ERROR("[cpu] PSCI CPU_ON cpu=%u failed: ret=%lld\n",
                       i, (long long)ret);
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
            KLOG_INFO("[cpu] cpu%u online (hw_id=0x%llx)\n",
                      i, (unsigned long long)c->hw_id);
        }
    }

    KLOG_WARN("[cpu] SMP bringup done: %u CPU(s) online\n",
              (unsigned)g_num_cpus);
}

#else  /* !ARCH_AARCH64 */

void
cpu_bring_up_all(void)
{
    /* riscv64 / x86_64 SMP 拉起留待后续 Phase 实现。 */
    if (CONFIG_SMP_CPUS > 1U) {
        KLOG_WARN("[cpu] CONFIG_SMP_CPUS=%u but SMP bringup not yet "
                  "implemented on this architecture. Running on CPU0 only.\n",
                  (unsigned)CONFIG_SMP_CPUS);
    }
}

#endif /* ARCH_AARCH64 */

/* ── SMP 自检：验证所有在线核的 timer ISR 都在动 ──────────
 *
 * 流程：BSP 用 timer_delay_ms 忙等若干轮（不依赖调度器），每轮快照
 * g_cpus[i].local_ticks，对比上一轮 → 全核都增长则 PASS，否则 FAIL。
 *
 * 必须在 timer_set_tick_cb 之后调用（次级核需要 g_tick_cb 才会进入
 * sched_check_and_yield，但这里其实不依赖回调，只看 local_ticks 增长，
 * 所以严格来说 timer_init_secondary 完成即可）。
 *
 * 全核必须共享一致的时间基准；aarch64 是 CNTPCT_EL0，per-core 但同源
 * 振荡器，timer_delay_ms 在任何核上读数一致。
 */
extern volatile uint64_t g_system_ticks;  /* driver/timer/timer.c */

#if ARCH_AARCH64
/* 诊断：读 DAIF、CNTP_CTL_EL0、CNTPCT_EL0 */
static inline uint64_t dbg_read_daif(void)
{
    uint64_t v; __asm__ volatile("mrs %0, daif" : "=r"(v)); return v;
}
static inline uint64_t dbg_read_cntp_ctl(void)
{
    uint64_t v; __asm__ volatile("mrs %0, cntp_ctl_el0" : "=r"(v)); return v;
}
static inline uint64_t dbg_read_cntpct(void)
{
    uint64_t v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v;
}
#endif

void
cpu_smp_timer_test(uint32_t rounds, uint32_t ms_per_round)
{
    /* 即使单核也跑诊断：先确认 BSP 自己的 timer 是否在 tick。 */

    /*
     * 该函数在 task_switch_to_idle_stack() 之前被调用，此时 DAIF.I=1（IRQ 关）。
     * 不先开 IRQ，BSP 就永远收不到 timer 中断，local_ticks 一定为 0。
     */
#if ARCH_AARCH64
    arch_irq_enable();
#endif

#if ARCH_AARCH64
    {
        uint64_t daif = dbg_read_daif();
        uint64_t ctl  = dbg_read_cntp_ctl();
        uint64_t pct  = dbg_read_cntpct();
        KLOG_INFO("[smp-test] BSP DAIF=0x%llx CNTP_CTL=0x%llx CNTPCT=%llu\n",
                  (unsigned long long)daif,
                  (unsigned long long)ctl,
                  (unsigned long long)pct);

        /* GIC 寄存器 dump：定位 IRQ 卡在哪一层 */
#if DRIVER_GIC_V2
        extern uintptr_t gicv2_gicd_base;
        extern uintptr_t gicv2_gicc_base;
        uintptr_t gicd_b = gicv2_gicd_base;
        uintptr_t gicc_b = gicv2_gicc_base;
        KLOG_INFO("[smp-test] GIC base: gicd=0x%llx gicc=0x%llx\n",
                  (unsigned long long)gicd_b, (unsigned long long)gicc_b);
        uint32_t gicd_ctlr   = *(volatile uint32_t *)(gicd_b + 0x000);
        uint32_t gicd_isen0  = *(volatile uint32_t *)(gicd_b + 0x100);
        uint32_t gicd_ispend = *(volatile uint32_t *)(gicd_b + 0x200);
        uint32_t gicd_pri26  = *(volatile uint32_t *)(gicd_b + 0x400 + (26/4)*4);
        uint32_t gicc_ctlr   = *(volatile uint32_t *)(gicc_b + 0x000);
        uint32_t gicc_pmr    = *(volatile uint32_t *)(gicc_b + 0x004);
        uint32_t gicc_hppir  = *(volatile uint32_t *)(gicc_b + 0x018);
        uint32_t gicc_rpr    = *(volatile uint32_t *)(gicc_b + 0x014);
        KLOG_INFO("[smp-test] GICD CTLR=0x%x ISENABLER0=0x%x ISPENDR0=0x%x IPRIORITY[26..]=0x%x\n",
                  gicd_ctlr, gicd_isen0, gicd_ispend, gicd_pri26);
        KLOG_INFO("[smp-test] GICC CTLR=0x%x PMR=0x%x HPPIR=0x%x RPR=0x%x\n",
                  gicc_ctlr, gicc_pmr, gicc_hppir, gicc_rpr);
#elif DRIVER_GIC_V3
        extern uintptr_t gicv3_gicd_base;
        extern uintptr_t gicv3_gicr_base;
        KLOG_INFO("[smp-test] GIC base: gicd=0x%llx gicr=0x%llx (GICv3)\n",
                  (unsigned long long)gicv3_gicd_base,
                  (unsigned long long)gicv3_gicr_base);
        uint32_t gicd_ctlr  = *(volatile uint32_t *)(gicv3_gicd_base + 0x000);
        uint32_t gicd_isen0 = *(volatile uint32_t *)(gicv3_gicd_base + 0x100);
        KLOG_INFO("[smp-test] GICD CTLR=0x%x ISENABLER0=0x%x\n",
                  gicd_ctlr, gicd_isen0);
        /* PPI 实际开关 / 组配置 / 优先级在 GICR SGI frame，不在 GICD */
        uintptr_t sgi = gicv3_gicr_base + 0x10000ULL;
        uint32_t r_isen0 = *(volatile uint32_t *)(sgi + 0x100);
        uint32_t r_grp0  = *(volatile uint32_t *)(sgi + 0x080);
        uint32_t r_pri_ppi26 = *(volatile uint32_t *)(sgi + 0x400 + (26/4)*4);
        uint64_t icc_pmr, icc_igrpen1, icc_ctlr;
        __asm__ volatile("mrs %0, S3_0_C4_C6_0"  : "=r"(icc_pmr));      /* ICC_PMR_EL1 */
        __asm__ volatile("mrs %0, S3_0_C12_C12_7": "=r"(icc_igrpen1));  /* ICC_IGRPEN1_EL1 */
        __asm__ volatile("mrs %0, S3_0_C12_C12_4": "=r"(icc_ctlr));     /* ICC_CTLR_EL1 */
        KLOG_INFO("[smp-test] GICR SGI ISENABLER0=0x%x IGROUPR0=0x%x IPRI[26..29]=0x%x\n",
                  r_isen0, r_grp0, r_pri_ppi26);
        KLOG_INFO("[smp-test] ICC PMR=0x%llx IGRPEN1=0x%llx CTLR=0x%llx\n",
                  (unsigned long long)icc_pmr,
                  (unsigned long long)icc_igrpen1,
                  (unsigned long long)icc_ctlr);
#endif
    }
#endif

    KLOG_INFO("[smp-test] timer health: %u cpu(s), %u rounds x %u ms\n",
              (unsigned)g_num_cpus, (unsigned)rounds, (unsigned)ms_per_round);

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
        KLOG_INFO("[smp-test] round=%u CNTPCT=%llu CNTP_CTL=0x%llx\n",
                  (unsigned)r,
                  (unsigned long long)pct_now,
                  (unsigned long long)ctl_now);
#endif

        uint64_t sys_now = g_system_ticks;
        KLOG_INFO("[smp-test] round=%u system_ticks=%llu (+%llu)\n",
                  (unsigned)r,
                  (unsigned long long)sys_now,
                  (unsigned long long)(sys_now - prev_sys));
        prev_sys = sys_now;

        for (uint32_t i = 0; i < g_num_cpus; i++) {
            uint64_t now  = g_cpus[i].local_ticks;
            uint64_t diff = now - prev[i];
            KLOG_INFO("[smp-test] round=%u cpu%u local_ticks=%llu (+%llu)\n",
                      (unsigned)r, (unsigned)i,
                      (unsigned long long)now,
                      (unsigned long long)diff);
            if (diff == 0ULL) {
                all_ok = false;
            }
            prev[i] = now;
        }
    }

    if (all_ok) {
        KLOG_INFO("[smp-test] PASS: all %u cpu(s) timer ticking\n",
                  (unsigned)g_num_cpus);
    } else {
        KLOG_ERROR("[smp-test] FAIL: some cpu(s) timer stuck\n");
    }
}
