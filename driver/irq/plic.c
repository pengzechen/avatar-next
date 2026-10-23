/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#include "irq/plic.h"
#include "exception.h"
#include "klog.h"
#include "mmio.h"
#include "platform_cfg.h"
#include "riscv64/sysreg.h"
#include "driver/driver.h"
#include "driver/irq.h"

#define PLIC_MAX_IRQS   128U
#define PLIC_CONTEXT_S0 1U

#define PLIC_PRIORITY_BASE  0x000000UL
#define PLIC_ENABLE_BASE    0x002000UL
#define PLIC_ENABLE_STRIDE  0x80UL
#define PLIC_CONTEXT_BASE   0x200000UL
#define PLIC_CONTEXT_STRIDE 0x1000UL
#define PLIC_THRESHOLD_OFF  0x0UL
#define PLIC_CLAIM_OFF      0x4UL

static uintptr_t g_plic_base;

/* 每条源线的优先级(默认 0 → 使能时按 1 处理) */
static uint32_t g_plic_prio[PLIC_MAX_IRQS];

/* 后端自有的 timer/IPI 处理器：它们不是 PLIC source，走 scause 分派 */
static irq_handler_fn g_plic_timer_fn;
static void *g_plic_timer_ctx;
static irq_handler_fn g_plic_ipi_fn;
static void *g_plic_ipi_ctx;

/* SBI IPI 扩展 */
#define PLIC_SBI_EID_IPI      0x735049UL
#define PLIC_SBI_FID_SEND_IPI 0UL

static inline uint32_t plic_read32(uintptr_t off)
{
    return read32((void *)(g_plic_base + off));
}

static inline void plic_write32(uintptr_t off, uint32_t val)
{
    write32(val, (void *)(g_plic_base + off));
}

static inline uintptr_t plic_context_base(void)
{
    return PLIC_CONTEXT_BASE + PLIC_CONTEXT_STRIDE * PLIC_CONTEXT_S0;
}

/*
   plic_dispatch - 外部中断的 claim → handler → complete

   未注册的源线由 irq_dispatch() 统一采样报警；这里必须无条件 complete,
   否则 level-high 源会把 PLIC 挂死(整机卡在 ISR 里)。
*/
static void plic_dispatch(void *frame)
{
    uint32_t irq = plic_read32(plic_context_base() + PLIC_CLAIM_OFF);
    if (irq == 0)
        return;

    irq_dispatch(irq, frame);
    plic_write32(plic_context_base() + PLIC_CLAIM_OFF, irq);
}

/* S 模式外部中断的 scause 钩子：真正的 claim/complete 在 plic_dispatch */
static void plic_external_irq(void *frame)
{
    irq_dispatch_frame(frame);
}

static void plic_ipi_entry(void *frame)
{
    if (g_plic_ipi_fn != NULL)
        g_plic_ipi_fn(frame, g_plic_ipi_ctx);

    /* SSIP 是电平型 pending 位，不清会在返回后立刻再次陷入 */
    CSR_CLEAR(sip, SIE_SSIE);
}

static void plic_timer_entry(void *frame)
{
    if (g_plic_timer_fn != NULL)
        g_plic_timer_fn(frame, g_plic_timer_ctx);
}

void plic_init(void)
{
    if (g_plic_base != 0)
        return;

    g_plic_base = platform_get_mmio("irq", "plic");
    if (g_plic_base == 0) {
        /* 平台事实, 一次性, 而且它回答了"为什么没有外部中断" —— 这是操作者
           需要看到的信息, 不是警告。
        */
        KLOG_INFO("[PLIC] base is 0, external IRQ disabled\n");
        return;
    }

    plic_write32(plic_context_base() + PLIC_THRESHOLD_OFF, 0);
    irq_install(CAUSE_SUPERVISOR_EXTERNAL, plic_external_irq);
    SET_SIE(SIE_SEIE);

    KLOG_INFO("[PLIC] initialized base=0x%lx context=%u SEIE enabled\n", (unsigned long)g_plic_base, PLIC_CONTEXT_S0);
}

void plic_enable_irq(uint32_t irq, uint32_t priority)
{
    if (g_plic_base == 0)
        plic_init();
    if (g_plic_base == 0 || irq == 0 || irq >= PLIC_MAX_IRQS)
        return;
    if (priority == 0)
        priority = 1;

    plic_write32(PLIC_PRIORITY_BASE + irq * 4UL, priority);

    uintptr_t off = PLIC_ENABLE_BASE + PLIC_ENABLE_STRIDE * PLIC_CONTEXT_S0 + (irq / 32U) * 4UL;
    uint32_t val = plic_read32(off);
    val |= (1U << (irq % 32U));
    plic_write32(off, val);

    KLOG_INFO("[PLIC] enabled irq=%u priority=%u enable=0x%08x\n", irq, priority, plic_read32(off));
}

void plic_disable_irq(uint32_t irq)
{
    if (g_plic_base == 0 || irq == 0 || irq >= PLIC_MAX_IRQS)
        return;

    uintptr_t off = PLIC_ENABLE_BASE + PLIC_ENABLE_STRIDE * PLIC_CONTEXT_S0 + (irq / 32U) * 4UL;
    uint32_t val = plic_read32(off);
    val &= ~(1U << (irq % 32U));
    plic_write32(off, val);
}

static void plic_write_priority(uint32_t irq, uint32_t prio)
{
    if (irq == 0 || irq >= PLIC_MAX_IRQS)
        return;
    if (prio == 0)
        prio = 1;
    plic_write32(PLIC_PRIORITY_BASE + irq * 4UL, prio);
}

/* ── 公共 IRQ 接口实现 ───────────────────────────────────────── */

static int plic_enable_api(unsigned irq, bool on)
{
    if (irq == 0 || irq >= PLIC_MAX_IRQS)
        return -2;

    if (!on) {
        plic_disable_irq(irq);
        return 0;
    }

    if (g_plic_base == 0)
        plic_init();
    if (g_plic_base == 0)
        return -1;

    plic_enable_irq(irq, g_plic_prio[irq]);
    return 0;
}

static int plic_set_priority_api(unsigned irq, unsigned prio)
{
    if (irq == 0 || irq >= PLIC_MAX_IRQS)
        return -2;

    if (g_plic_base == 0)
        plic_init();
    if (g_plic_base == 0)
        return -1;

    g_plic_prio[irq] = prio;
    plic_write_priority(irq, prio);
    return 0;
}

static int plic_send_ipi_api(uint32_t hw_id)
{
    if (hw_id >= 64U)
        return -2;

    register uint64_t a0 __asm__("a0") = 1ULL << hw_id;
    register uint64_t a1 __asm__("a1") = 0;
    register uint64_t a6 __asm__("a6") = PLIC_SBI_FID_SEND_IPI;
    register uint64_t a7 __asm__("a7") = PLIC_SBI_EID_IPI;
    __asm__ volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a6), "r"(a7) : "memory");
    return (a0 == 0) ? 0 : -1;
}

static int plic_register_ipi_api(irq_handler_fn fn, void *ctx)
{
    g_plic_ipi_fn = fn;
    g_plic_ipi_ctx = ctx;
    irq_install(CAUSE_SUPERVISOR_SOFTWARE, plic_ipi_entry);
    SET_SIE(SIE_SSIE);
    return 0;
}

static int plic_register_timer_api(irq_handler_fn fn, void *ctx)
{
    g_plic_timer_fn = fn;
    g_plic_timer_ctx = ctx;
    irq_install(CAUSE_SUPERVISOR_TIMER, plic_timer_entry);
    return 0;
}

static int plic_timer_enable_api(void)
{
    /* timer_arch_enable() 自己写 sie.STIE；AP 的 timer_init_secondary 也走它 */
    return 0;
}

static void plic_debug_dump_api(void)
{
    uint32_t threshold = 0;
    if (g_plic_base != 0)
        threshold = plic_read32(plic_context_base() + PLIC_THRESHOLD_OFF);
    KLOG_SMP("[smp-test] PLIC base=0x%lx threshold=%u\n", (unsigned long)g_plic_base, threshold);
}

static const irq_ops_t plic_irq_ops = {
    .dispatch = plic_dispatch,
    .enable = plic_enable_api,
    .set_priority = plic_set_priority_api,
    .send_ipi = plic_send_ipi_api,
    .register_ipi = plic_register_ipi_api,
    .register_timer = plic_register_timer_api,
    .timer_enable = plic_timer_enable_api,
    .debug_dump = plic_debug_dump_api,
    .virt_supported = false,
};

static int plic_probe(const device_t *dev)
{
    (void)dev; /* 阶段 1：plic_init() 仍直接读平台配置 */
    irq_bind(&plic_irq_ops);
    plic_init();
    return 0;
}

DRIVER_ENTRY(driver_plic) = {
    .name = "plic",
    .cls = DRV_IRQ,
    .phase = DRV_PHASE_IRQ,
    .start_phase = DRV_PHASE_COUNT,
    .probe = plic_probe,
};
