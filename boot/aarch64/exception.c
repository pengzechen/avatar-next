
#include "exception.h"
#include "aarch64/sysreg.h" /* READ_ESR_EL1, READ_ELR_EL1 等 */
#include "irq/irq.h"
#include "klog.h"
#include "platform_ops.h"
#include "types.h"
#include "task/task.h"
#include "task/cpu.h"

extern void deliver_pending_signals(task_t *t, trap_frame_t *frame);

#define MAX_IRQ_VECTORS 512

irq_handler_t g_handler_vec[MAX_IRQ_VECTORS] = {0};

void irq_install(int vector, void (*h)(uint64_t *)) {
  g_handler_vec[vector] = h;
}

void handle_sync_exception(uint64_t *stack_pointer) {
  trap_frame_t *el1_ctx = (trap_frame_t *)stack_pointer;

  uint64_t esr = READ_ESR_EL1();
  uint64_t far = READ_FAR_EL1();
  uint32_t ec = (esr >> 26) & 0x3F;
  uint32_t dfsc = esr & 0x3F;

  KLOG_ERROR("[el1_sync] EL1 exception: EC=0x%x, ESR=0x%llx, FAR=0x%llx\n", ec,
             esr, far);
  KLOG_ERROR("[el1_sync] ELR=0x%llx, SP_EL0=0x%llx, SPSR=0x%llx\n",
             el1_ctx->elr, el1_ctx->usp, el1_ctx->spsr);
  KLOG_ERROR("[el1_sync] DFSC=0x%x (translation=%d perm=%d)\n", dfsc,
             (dfsc & 0x3C) == 0x04, (dfsc & 0x3C) == 0x0C);

  (void)ec;

  platform_shutdown();
}

/* ── 用户态同步异常处理（系统调用、缺页等）────────────────────── */

extern void syscall_handler(trap_frame_t *frame);

void handle_el0_sync_exception(uint64_t *stack_pointer) {
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

  /* EC == 0x20: Instruction Abort from EL0
   * EC == 0x24: Data Abort from EL0
   * → 投递 SIGSEGV 给用户进程 */
  if (ec == 0x20 || ec == 0x24) {
    task_t *t = task_current();
    if (t && t->is_user_process) {
      KLOG_WARN("[el0_sync] user page fault sig=SIGSEGV pid=%u pc=0x%llx va=0x%llx\n",
                t->id, el1_ctx->elr, far);
      task_send_signal(t, SIGSEGV);
      deliver_pending_signals(t, el1_ctx);
      return;
    }
  }

  /* 其他异常类型 */
  KLOG_ERROR(
      "[el0_sync] Unexpected exception: EC=0x%x, ESR=0x%llx, FAR=0x%llx\n", ec,
      esr, far);
  KLOG_ERROR("[el0_sync] DFSC=0x%x (translation=%d perm=%d)\n", dfsc,
             (dfsc & 0x3C) == 0x04, (dfsc & 0x3C) == 0x0C);
  KLOG_ERROR("[el0_sync] ELR=0x%llx, SP_EL0=0x%llx, SPSR=0x%llx\n",
             el1_ctx->elr, el1_ctx->usp, el1_ctx->spsr);

  /* 停机 */
  platform_shutdown();
}

void handle_irq_exception(uint64_t *stack_pointer) {
  trap_frame_t *el1_ctx = (trap_frame_t *)stack_pointer;
  (void)el1_ctx; // Suppress unused parameter warning
  cpu_t *cpu = cpu_current();

  cpu->irq_depth++;

  /* Read IAR to acknowledge the interrupt */
  int iar = irq_ack();
  int vector = iar & 0x3FF; /* Extract IRQ number */

  /* Call the handler if registered */
  if (vector < 512 && g_handler_vec[vector] != 0)
    g_handler_vec[vector](stack_pointer);
  else
    KLOG_WARN("No handler for IRQ %d\n", vector);

  /* End of interrupt */
  irq_eoi(iar);
  gic_write_dir(iar);

  cpu->irq_depth--;
}

void invalid_exception(uint64_t *stack_pointer, uint64_t kind,
                       uint64_t source) {
  trap_frame_t *el1_ctx = (trap_frame_t *)stack_pointer;
  (void)el1_ctx; // Suppress unused parameter warning

  KLOG_INFO("This is invalid_exception: kind: %x, source: %x\n", kind, source);
  platform_panic();
}
