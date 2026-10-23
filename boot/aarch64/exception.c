/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#include "exception.h"
#include "aarch64/sysreg.h" /* READ_ESR_EL1, READ_ELR_EL1 等 */
#include "driver/irq.h"
#include "syscall/trace.h"
#include "debug/backtrace.h"
#include "klog.h"
#include "platform_ops.h"
#include "types.h"
#include "task/task.h"
#include "task/cpu.h"

extern void deliver_pending_signals(task_t *t, trap_frame_t *frame);
extern void syscall_handler(trap_frame_t *frame);

void handle_sync_exception(uint64_t *stack_pointer)
{
    trap_frame_t *el1_ctx = (trap_frame_t *)stack_pointer;

    uint64_t esr = READ_ESR_EL1();
    uint64_t far = READ_FAR_EL1();
    uint32_t ec = (esr >> 26) & 0x3F;
    uint32_t dfsc = esr & 0x3F;

    KLOG_ERROR("[el1_sync] EL1 exception: EC=0x%x, ESR=0x%llx, FAR=0x%llx\n", ec, esr, far);
    KLOG_ERROR("[el1_sync] ELR=0x%llx, SP_EL0=0x%llx, SPSR=0x%llx\n", el1_ctx->elr, el1_ctx->usp, el1_ctx->spsr);
    KLOG_ERROR("[el1_sync] DFSC=0x%x (translation=%d perm=%d)\n", dfsc, (dfsc & 0x3C) == 0x04, (dfsc & 0x3C) == 0x0C);

    (void)ec;

    /*
     打调用栈, 回答"是谁调过来的"。

     用异常帧而不是当前栈：现在已经在异常处理程序里, 当前栈是处理程序
     自己的, 看不到出错的那条路径。
       elr     = 出错的那条指令
       r[29]   = x29, 帧指针
       sp      = SAVE_REGS 之后 SP 被下移了 TRAP_FRAME_SIZE, 所以出错时的
                 SP 就是帧地址加上这个大小

     这里保持 platform_shutdown() 而不是换成 platform_panic()：本函数
     原先就是关机, 改成死循环会改变对外行为(依赖"QEMU 自行退出"的脚本
     会挂住)。先打栈, 再关机, 既保住现场又不动语义。
    */
    backtrace_dump_fault(el1_ctx->elr, el1_ctx->r[29], (uintptr_t)el1_ctx + TRAP_FRAME_SIZE, 0);

    platform_shutdown();
}

/* ── 用户态同步异常处理(系统调用, 缺页等)────────────────────── */

void handle_el0_sync_exception(uint64_t *stack_pointer)
{
    trap_frame_t *el1_ctx = (trap_frame_t *)stack_pointer;

    uint64_t esr = READ_ESR_EL1();
    uint64_t far = READ_FAR_EL1();
    uint32_t ec = (esr >> 26) & 0x3F;
    uint32_t dfsc = esr & 0x3F;

    /* EC == 0x15: SVC 指令(系统调用) */
    if (ec == 0x15) {
        /* 调用系统调用处理函数, 传入完整 trap_frame */
        syscall_handler(el1_ctx);
        return;
    }

    /* EC == 0x20: Instruction Abort from EL0
     EC == 0x24: Data Abort from EL0
     → 投递 SIGSEGV 给用户进程
    */
    if (ec == 0x20 || ec == 0x24) {
        task_t *t = task_current();
        if (t && t->is_user_process) {
            KLOG_WARN("[el0_sync] user page fault sig=SIGSEGV pid=%u pc=0x%llx va=0x%llx\n", t->id, el1_ctx->elr, far);
            /* 崩溃现场：把这个 pid 最近走过的 syscall 打出来。
         环形缓冲是常开的, 所以这里不需要事先开任何开关。
            */
            syscall_trace_dump((uint16_t)t->id, SYSCALL_TRACE_DUMP_MAX);
            task_send_signal(t, SIGSEGV);
            deliver_pending_signals(t, el1_ctx);
            return;
        }
    }

    /* 其他异常类型 */
    KLOG_ERROR("[el0_sync] Unexpected exception: EC=0x%x, ESR=0x%llx, FAR=0x%llx\n", ec, esr, far);
    KLOG_ERROR("[el0_sync] DFSC=0x%x (translation=%d perm=%d)\n", dfsc, (dfsc & 0x3C) == 0x04, (dfsc & 0x3C) == 0x0C);
    KLOG_ERROR("[el0_sync] ELR=0x%llx, SP_EL0=0x%llx, SPSR=0x%llx\n", el1_ctx->elr, el1_ctx->usp, el1_ctx->spsr);

    /* 停机 */
    platform_shutdown();
}

void handle_irq_exception(uint64_t *stack_pointer)
{
    cpu_t *cpu = cpu_current();

    cpu->irq_depth++;

    /*
       ack → handler → EOI/complete 全部由中断控制器后端完成
       (GICv2/GICv3 的 IAR/EOIR 时序差异不再泄漏到异常入口)。
    */
    irq_dispatch_frame(stack_pointer);

    cpu->irq_depth--;
}

void invalid_exception(uint64_t *stack_pointer, uint64_t kind, uint64_t source)
{
    /* 调用点(向量表里的 .Lvector_other 宏)是 SAVE_REGS 之后 `mov x0, sp`,
     所以这里拿到的是完整的 trap frame, 可以当调用栈的起点用。
    */
    trap_frame_t *el1_ctx = (trap_frame_t *)stack_pointer;

    /* 打完下一行就 panic —— panic 路径上的日志永远是 ERROR */
    KLOG_ERROR("invalid_exception: kind: %x, source: %x\n", kind, source);

    backtrace_dump_fault(el1_ctx->elr, el1_ctx->r[29], (uintptr_t)el1_ctx + TRAP_FRAME_SIZE, 0);
    platform_panic();
}
