
#include "types.h"
#include "exception.h"
#include "aarch64/sysreg.h"    /* READ_ESR_EL1, READ_ELR_EL1 等 */
#include "platform.h"
#include "irq/irq.h"
#include "klog.h"

/* 前向声明，避免循环依赖 */
struct task;
extern struct task *g_current_task;

irq_handler_t g_handler_vec[512] = {0};
uint64_t print_flag = 0;

void irq_install(int vector, void (*h)(uint64_t *))
{
    g_handler_vec[vector] = h;
}

void handle_sync_exception(uint64_t *stack_pointer)
{
    trap_frame_t *el1_ctx = (trap_frame_t *)stack_pointer;

    uint64_t esr = READ_ESR_EL1();
    uint64_t far = READ_FAR_EL1();
    uint32_t ec  = (esr >> 26) & 0x3F;
    uint32_t dfsc = esr & 0x3F;

    KLOG_ERROR("[el1_sync] EL1 exception: EC=0x%x, ESR=0x%llx, FAR=0x%llx\n", ec, esr, far);
    KLOG_ERROR("[el1_sync] ELR=0x%llx, SP_EL0=0x%llx, SPSR=0x%llx\n",
               el1_ctx->elr, el1_ctx->usp, el1_ctx->spsr);
    KLOG_ERROR("[el1_sync] DFSC=0x%x (translation=%d perm=%d)\n",
               dfsc, (dfsc & 0x3C) == 0x04, (dfsc & 0x3C) == 0x0C);

    (void)ec;

    do_platform_shutdown();
}

/* ── 用户态同步异常处理（系统调用、缺页等）────────────────────── */

extern void syscall_handler(trap_frame_t *frame);

void handle_el0_sync_exception(uint64_t *stack_pointer)
{
    trap_frame_t *el1_ctx = (trap_frame_t *)stack_pointer;

    uint64_t esr = READ_ESR_EL1();
    uint64_t far = READ_FAR_EL1();
    uint32_t ec = (esr >> 26) & 0x3F;
    uint32_t dfsc = esr & 0x3F;

    /* EC == 0x15: SVC 指令（系统调用） */
    if (ec == 0x15) {
        /* 调用系统调用处理函数，传入完整 trap_frame */
        syscall_handler(el1_ctx);
        return;
    }

    /* 其他异常类型 */
    KLOG_ERROR("[el0_sync] Unexpected exception: EC=0x%x, ESR=0x%llx, FAR=0x%llx\n", ec, esr, far);
    KLOG_ERROR("[el0_sync] DFSC=0x%x (translation=%d perm=%d)\n",
               dfsc, (dfsc & 0x3C) == 0x04, (dfsc & 0x3C) == 0x0C);
    KLOG_ERROR("[el0_sync] ELR=0x%llx, SP_EL0=0x%llx, SPSR=0x%llx\n",
               el1_ctx->elr, el1_ctx->usp, el1_ctx->spsr);

    /* 停机 */
    do_platform_shutdown();
}

void handle_irq_exception(uint64_t *stack_pointer)
{
    trap_frame_t *el1_ctx = (trap_frame_t *)stack_pointer;

    (void)el1_ctx;  // Suppress unused parameter warning

    /* Read IAR to acknowledge the interrupt */
    int iar = irq_ack();
    int vector = iar & 0x3FF;  /* Extract IRQ number */

    /* Call the handler if registered */
    if (vector < 512 && g_handler_vec[vector] != 0) {
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