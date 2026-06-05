#include "irq/plic.h"
#include "exception.h"
#include "klog.h"
#include "mmio.h"
#include "platform_cfg.h"
#include "riscv64/sysreg.h"

#define PLIC_MAX_IRQS      128U
#define PLIC_CONTEXT_S0    1U

#define PLIC_PRIORITY_BASE 0x000000UL
#define PLIC_ENABLE_BASE   0x002000UL
#define PLIC_ENABLE_STRIDE 0x80UL
#define PLIC_CONTEXT_BASE  0x200000UL
#define PLIC_CONTEXT_STRIDE 0x1000UL
#define PLIC_THRESHOLD_OFF 0x0UL
#define PLIC_CLAIM_OFF     0x4UL

typedef struct {
    plic_irq_handler_t handler;
    void              *ctx;
} plic_handler_slot_t;

static uintptr_t g_plic_base;
static plic_handler_slot_t g_plic_handlers[PLIC_MAX_IRQS];
static volatile uint64_t g_plic_irq_count;

static bool plic_irq_log_sample(uint64_t n)
{
    return n <= 16 || (n <= 4096 && (n & (n - 1)) == 0);
}

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

static void plic_external_irq(void *frame)
{
    (void)frame;
    uint32_t irq = plic_read32(plic_context_base() + PLIC_CLAIM_OFF);
    if (irq == 0)
        return;

    g_plic_irq_count++;
    if (plic_irq_log_sample(g_plic_irq_count)) {
        KLOG_WARN("[PLIC] external IRQ #%llu claim=%u handled=%u\n",
                  g_plic_irq_count, irq,
                  (irq < PLIC_MAX_IRQS && g_plic_handlers[irq].handler) ? 1U : 0U);
    }

    if (irq < PLIC_MAX_IRQS && g_plic_handlers[irq].handler) {
        g_plic_handlers[irq].handler(irq, g_plic_handlers[irq].ctx);
    } else {
        KLOG_WARN("[PLIC] unhandled irq=%u count=%llu\n", irq, g_plic_irq_count);
    }

    plic_write32(plic_context_base() + PLIC_CLAIM_OFF, irq);
}

void plic_init(void)
{
    if (g_plic_base != 0)
        return;

    g_plic_base = platform_get_mmio("irq", "plic");
    if (g_plic_base == 0) {
        KLOG_WARN("[PLIC] base is 0, external IRQ disabled\n");
        return;
    }

    plic_write32(plic_context_base() + PLIC_THRESHOLD_OFF, 0);
    irq_install(CAUSE_SUPERVISOR_EXTERNAL, plic_external_irq);
    SET_SIE(SIE_SEIE);

    KLOG_INFO("[PLIC] initialized base=0x%lx context=%u SEIE enabled\n",
              (unsigned long)g_plic_base, PLIC_CONTEXT_S0);
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

    uintptr_t off = PLIC_ENABLE_BASE + PLIC_ENABLE_STRIDE * PLIC_CONTEXT_S0
                  + (irq / 32U) * 4UL;
    uint32_t val = plic_read32(off);
    val |= (1U << (irq % 32U));
    plic_write32(off, val);

    KLOG_INFO("[PLIC] enabled irq=%u priority=%u enable=0x%08x\n",
              irq, priority, plic_read32(off));
}

void plic_disable_irq(uint32_t irq)
{
    if (g_plic_base == 0 || irq == 0 || irq >= PLIC_MAX_IRQS)
        return;

    uintptr_t off = PLIC_ENABLE_BASE + PLIC_ENABLE_STRIDE * PLIC_CONTEXT_S0
                  + (irq / 32U) * 4UL;
    uint32_t val = plic_read32(off);
    val &= ~(1U << (irq % 32U));
    plic_write32(off, val);
}

void plic_install(uint32_t irq, plic_irq_handler_t handler, void *ctx)
{
    if (irq == 0 || irq >= PLIC_MAX_IRQS)
        return;
    g_plic_handlers[irq].handler = handler;
    g_plic_handlers[irq].ctx = ctx;
}
