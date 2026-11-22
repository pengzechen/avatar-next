/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/task/aarch64/cpu_startup.c — AArch64 BSP / 次级核启动

   通用 CPU 管理留在 kernel/task/cpu.c；这里只放架构相关部分：
   PSCI CPU_ON 拉起、GIC per-CPU 初始化、MPIDR→hw_id 推导，
   以及 SMP 自检里 AArch64 专有的诊断输出。
*/

#include "task/cpu.h"
#include "task/task.h"
#include "task/sched.h"
#include "assert.h"
#include "klog.h"
#include "string.h"
#include "arch.h"
#include "barrier.h"
#include "driver/time.h"
#include "driver/irq.h"
#include "exception.h"         /* arch_irq_enable / arch_irq_flags */
#include "mm/mm_vm.h"          /* virt_to_phys */
#include "arch/aarch64/neon.h" /* aarch64_enable_neon (per-core CPACR_EL1.FPEN) */
#include "arch/aarch64/psci.h" /* psci_cpu_on / psci_affinity_info(SMC64) */

/* boot/aarch64/boot.S 中定义的次级核入口(链接地址在高 VA) */
extern void _secondary_start(void);

/* GIC CPU interface 初始化(每核都要做一次；distributor 由 BSP 初始化) */
extern void gic_init_secondary(void);

uint64_t arch_cpu_bsp_hw_id(void)
{
    return arch_cpu_hw_id();
}

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
     4. 通知 BSP online, 开 IRQ, 进入 idle 循环

   run_queue 为空时 sched_schedule 会一直选回自己的 idle；跨核 enqueue
   与 resched IPI 会把本核唤醒去跑别的任务。
*/
void cpu_secondary_bootstrap(uint32_t cpu_id)
{
    cpu_t *c = &g_cpus[cpu_id];

    /* 每核独立寄存器：必须在跑任何 FP/SIMD 之前打开 CPACR_EL1.FPEN,
       否则 EL0 上 musl 启动早期访问 NEON 会触发 EC=0x7 trap。
       BSP 在 kernel_main 入口已做同样操作；AP 必须在此处补上。
    */
    aarch64_enable_neon();

    /* 安装 TPIDR_EL1, 使 cpu_current() 在本核返回正确指针 */
    arch_cpu_self_set(c);
    c->hw_id = arch_cpu_hw_id();

    /* 控制器 per-CPU 初始化(GIC: PMR/CTLR/GICR) */
    irq_secondary_init();

    /*
       必须在开中断之前装好 current_task, 否则首个 timer ISR 进入
       sched_check_and_yield 会读到 NULL。
    */
    cpu_setup_secondary_idle_task(c);

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
        arch_cpu_idle_wait();
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

/* ── SMP 自检的 AArch64 部分 ────────────────────────────── */

void arch_cpu_smp_test_begin(void)
{
    /*
       该函数在 task_switch_to_idle_stack() 之前被调用, 此时 DAIF.I=1。
       不先开 IRQ, BSP 就永远收不到 timer 中断, local_ticks 一定为 0。
    */
    arch_irq_enable();
}

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

void arch_cpu_smp_test_diag_begin(void)
{
    uint64_t daif = arch_irq_flags(); /* 统一的中断状态读取 */
    uint64_t ctl = dbg_read_cntp_ctl();
    uint64_t pct = dbg_read_cntpct();
    KLOG_SMP("[smp-test] BSP DAIF=0x%llx CNTP_CTL=0x%llx CNTPCT=%llu\n", (unsigned long long)daif,
             (unsigned long long)ctl, (unsigned long long)pct);

    /* 控制器相关的诊断寄存器由后端自己 dump，内核不再知道是 GICv2 还是 GICv3 */
    irq_debug_dump();
}

void arch_cpu_smp_test_diag_round(uint32_t round)
{
    uint64_t pct_now = dbg_read_cntpct();
    uint64_t ctl_now = dbg_read_cntp_ctl();
    KLOG_SMP("[smp-test] round=%u CNTPCT=%llu CNTP_CTL=0x%llx\n", (unsigned)round, (unsigned long long)pct_now,
             (unsigned long long)ctl_now);
}
