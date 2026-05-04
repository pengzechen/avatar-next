/*
 * boot/riscv64/exception.c — RISC-V 64-bit S-mode 异常/中断 C 分发器
 *
 * 提供：
 *   exception_init()   — 设置 stvec，使能 sstatus.SIE
 *   irq_install()      — 注册 scause 对应的中断处理函数
 *   handle_exception() — 从 exception.S 调用，分发陷阱
 */

#include "exception.h"
#include "klog.h"
#include "riscv64/sysreg.h"
#include "syscall/syscall.h"

/* 中断处理函数表，索引 = scause 低位（去掉 bit63 后的中断编号）*/
#define MAX_IRQ_CAUSES  16
static irq_handler_t interrupt_handlers[MAX_IRQ_CAUSES];

/* ── 公共接口：注册中断处理函数 ────────────────────────────────── */

void irq_install(int cause, irq_handler_t h)
{
    if (cause >= 0 && cause < MAX_IRQ_CAUSES)
        interrupt_handlers[cause] = h;
}

/* ── exception_init ─────────────────────────────────────────────── */

void exception_init(void)
{
    /* trap_vector 在 exception.S 中定义（.align 4，直接模式） */
    extern void trap_vector(void);

    /* 写入 stvec，bits[1:0] = 00 表示 Direct 模式 */
    WRITE_STVEC((uint64_t)trap_vector);

    /* 使能 sstatus.SIE（全局 S 态中断开关）*/
    CSR_SET(sstatus, SSTATUS_SIE);

    /*
     * 使能 sstatus.SUM（Supervisor User Memory access）。
     * 置 1 后 S 模式可直接读写带 PTE_U 的用户页面，
     * 使 syscall 处理函数能直接解引用用户空间指针（如 write buf）。
     */
    CSR_SET(sstatus, 1UL << 18);   /* bit 18 = SUM */

    KLOG_INFO("RISC-V exception init: stvec=0x%lx, sstatus.SIE+SUM enabled\n",
              (uint64_t)trap_vector);
}

/* ── handle_exception ───────────────────────────────────────────── *
 * 由 exception.S 的 trap_vector 调用，参数为 trap_frame_t *
 */
void handle_exception(void *frame_ptr)
{
    trap_frame_t *frame = (trap_frame_t *)frame_ptr;
    uint64_t cause     = frame->scause;
    static uint32_t s_trap_log_count = 0;
    uint64_t is_interrupt = (cause & SCAUSE_INTERRUPT_BIT) ? 1 : 0;
    uint64_t code = cause & ~SCAUSE_INTERRUPT_BIT;
    uint64_t from_user = (frame->sstatus & SSTATUS_SPP) ? 0 : 1;

    if (from_user || code == CAUSE_USER_ECALL || s_trap_log_count < 64) {
        KLOG_INFO("[trap] %s code=%lu from_%s sepc=0x%lx stval=0x%lx\n",
                  is_interrupt ? "irq" : "exc",
                  code,
                  from_user ? "user" : "kernel",
                  frame->sepc,
                  frame->stval);
        if (!from_user && code != CAUSE_USER_ECALL)
            s_trap_log_count++;
    }

    if (cause & SCAUSE_INTERRUPT_BIT) {
        /* ── 中断路径 ──────────────────────────────────────────── */
        uint64_t irq = cause & ~SCAUSE_INTERRUPT_BIT;

        if (irq < MAX_IRQ_CAUSES && interrupt_handlers[irq]) {
            interrupt_handlers[irq](frame_ptr);
        } else {
            KLOG_WARN("Unhandled interrupt: scause=0x%lx (irq=%lu)\n",
                      cause, irq);
        }
    } else {
        /* ── 同步异常路径 ────────────────────────────────────── */
        if (cause == CAUSE_USER_ECALL) {
            /* RISC-V ecall: sepc 指向 ecall 本身，需手动前进到下一条 */
            frame->sepc += 4;
            syscall_handler(frame);
            return;
        }

        KLOG_ERROR("Unhandled exception: scause=0x%lx sepc=0x%lx stval=0x%lx\n",
                   cause, frame->sepc, frame->stval);
        while (1)
            __asm__ volatile("wfi");
    }
}
