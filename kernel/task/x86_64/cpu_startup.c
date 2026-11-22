/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

/*
   kernel/task/x86_64/cpu_startup.c — x86_64 BSP / 次级核启动

   通用 CPU 管理留在 kernel/task/cpu.c；这里只放架构相关部分：
   INIT-SIPI + AP trampoline、LAPIC ID 读取、TSS per-CPU 初始化，
   以及 SMP 自检的 x86_64 部分。
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
#include "exception.h" /* exception_init_secondary */
#include "irq/lapic.h"
#include "../../../boot/x86_64/tss.h" /* x86_tss_init_cpu */

#define X86_AP_TRAMP_PHYS     0x00007000UL
#define X86_AP_TRAMP_VECTOR   (X86_AP_TRAMP_PHYS >> 12)
#define X86_AP_DATA_CPU_ID    ((volatile uint32_t *)0x00007f00UL)
#define X86_AP_DATA_STACK_TOP ((volatile uint64_t *)0x00007f08UL)

extern uint8_t x86_ap_trampoline_start[];
extern uint8_t x86_ap_trampoline_end[];

uint64_t arch_cpu_bsp_hw_id(void)
{
    /* x86 的 hw_id 是 LAPIC ID：arch_cpu_hw_id() 只返回 0 占位，
       BSP 也必须走控制器后端读真实值，否则真实硬件上 BSP 的 APIC ID != 0 时，
       sched_resched_cpu(0) 会把 IPI 发到错误的核。 */
    return irq_cpu_hw_id();
}

void cpu_secondary_bootstrap(uint32_t cpu_id)
{
    cpu_t *c = &g_cpus[cpu_id];

    arch_cpu_self_set(c);
    c->hw_id = irq_cpu_hw_id();

    exception_init_secondary();
    cpu_setup_secondary_idle_task(c);
    x86_tss_init_cpu(cpu_id, (uint64_t)(uintptr_t)&secondary_boot_stacks[(cpu_id + 1U) * SECONDARY_STACK_SIZE]);
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

/* ── SMP 自检的 x86_64 部分 ─────────────────────────────── */

void arch_cpu_smp_test_begin(void)
{
    /* 保持原行为：x86 的 BSP 到这里已经能收 timer 中断, 不在这里重新开。 */
}

void arch_cpu_smp_test_diag_begin(void)
{}

void arch_cpu_smp_test_diag_round(uint32_t round)
{
    (void)round;
}
