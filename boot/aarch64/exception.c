
#include "types.h"
#include "exception.h"
#include "aarch64/sysreg.h"    /* READ_ESR_EL1, READ_ELR_EL1 等 */
#include "platform.h"
#include "irq/irq.h"
#include "klog.h"

irq_handler_t g_handler_vec[512] = {0};
uint64_t print_flag = 0;

void irq_install(int vector, void (*h)(uint64_t *))
{
    g_handler_vec[vector] = h;
}

void handle_sync_exception(uint64_t *stack_pointer)
{
    trap_frame_t *el1_ctx = (trap_frame_t *)stack_pointer;

    int el1_esr = READ_ESR_EL1();

    int ec = ((el1_esr >> 26) & 0b111111);

    KLOG_INFO("el1 esr: %x\n", el1_esr);
    KLOG_INFO("ec: %x\n", ec);

    KLOG_INFO("This is handle_sync_exception: \n");
    for (int i = 0; i < NUM_REGS; i++)
    {
        uint64_t value = el1_ctx->r[i];
        kprintf("General-purpose register: 0x%d, value: 0x%llx\n", i, value);
    }

    uint64_t elr_el1_value = el1_ctx->elr;
    uint64_t usp_value = el1_ctx->usp;
    uint64_t spsr_value = el1_ctx->spsr;

    KLOG_INFO("usp: 0x%llx, elr: 0x%llx, spsr: 0x%llx\n", usp_value, elr_el1_value, spsr_value);

    /* Skip the faulting instruction to avoid infinite loop */
    /* AArch64 instructions are 4 bytes */
    el1_ctx->elr += 4;

    KLOG_INFO("Exception handled, skipping instruction. New ELR: 0x%llx\n", el1_ctx->elr);

    do_platform_panic();
}

void handle_irq_exception(uint64_t *stack_pointer)
{
    trap_frame_t *el1_ctx = (trap_frame_t *)stack_pointer;

    (void)el1_ctx;  // Suppress unused parameter warning

    // KLOG_INFO("IRQ exception occurred\n");

    /* Read IAR to acknowledge the interrupt */
    int iar = irq_ack();
    int vector = iar & 0x3FF;  /* Extract IRQ number */

    // KLOG_INFO("IRQ vector: %d, handler: %p\n", vector, g_handler_vec[vector]);

    /* Call the handler if registered */
    if (vector < 512 && g_handler_vec[vector] != 0) {
        // KLOG_INFO("Calling handler for IRQ %d\n", vector);
        g_handler_vec[vector](stack_pointer);
    } else {
        KLOG_WARN("No handler for IRQ %d\n", vector);
    }

    /* End of interrupt */
    irq_eoi(iar);
    extern void gic_write_dir(uint32_t irqstat);
    gic_write_dir(iar);
}

void invalid_exception(uint64_t *stack_pointer, uint64_t kind, uint64_t source)
{
    trap_frame_t *el1_ctx = (trap_frame_t *)stack_pointer;

    (void)el1_ctx;  // Suppress unused parameter warning

    KLOG_INFO("This is invalid_exception: kind: %x, source: %x\n", kind, source);
    do_platform_panic();
}