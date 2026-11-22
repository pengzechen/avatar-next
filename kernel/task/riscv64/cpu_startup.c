/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/task/riscv64/cpu_startup.c — RISC-V BSP / 次级核启动

   通用 CPU 管理留在 kernel/task/cpu.c；这里只放架构相关部分：
   SBI HSM hart_start(不支持时回退到 release 表 + IPI)、hartid 推导，
   以及 SMP 自检的 RISC-V 部分。
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
#include "exception.h"           /* arch_irq_enable */
#include "mm/mm_vm.h"            /* phys_to_virt */
#include "arch/riscv64/sbi.h"    /* sbi_hart_start / sbi_send_ipi(HSM/IPI) */
#include "arch/riscv64/sysreg.h" /* CSR_SET / WRITE_STVEC */

/* OpenSBI/HSM 入口传入的 boot hartid；link.ld 为 .text.boot 提供低地址别名 */
volatile uint64_t riscv64_boot_hartid;

extern void trap_vector(void);
extern const uint64_t riscv64_hart_release_phys;
extern const uint64_t riscv64_secondary_start_phys;

uint64_t arch_cpu_bsp_hw_id(void)
{
    return arch_cpu_hw_id();
}

void cpu_secondary_bootstrap(uint32_t cpu_id)
{
    cpu_t *c = &g_cpus[cpu_id];

    arch_cpu_self_set(c);
    CSR_SET(sstatus, 1UL << 18); /* SUM */
    WRITE_STVEC((uint64_t)trap_vector);

    cpu_setup_secondary_idle_task(c);
    timer_init_secondary();

    /* resched IPI：必须在 online 之前装好, 否则 BSP 发来的 kick 会丢。 */
    cpu_ipi_init();

    wmb();
    c->online = true;

    arch_irq_enable();
    for (;;) {
        sched_check_and_yield();
        arch_cpu_idle_wait();
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

/* ── SMP 自检的 RISC-V 部分 ─────────────────────────────── */

void arch_cpu_smp_test_begin(void)
{
    /* 与 AArch64 同理：调用点还在关中断的 boot 上下文里。 */
    arch_irq_enable();
}

void arch_cpu_smp_test_diag_begin(void)
{}

void arch_cpu_smp_test_diag_round(uint32_t round)
{
    (void)round;
}
