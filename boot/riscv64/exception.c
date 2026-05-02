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

    KLOG_INFO("RISC-V exception init: stvec=0x%lx, sstatus.SIE enabled\n",
              (uint64_t)trap_vector);
}

/* ── handle_exception ───────────────────────────────────────────── *
 * 由 exception.S 的 trap_vector 调用，参数为 trap_frame_t *
 */
void handle_exception(void *frame_ptr)
{
    trap_frame_t *frame = (trap_frame_t *)frame_ptr;
    uint64_t cause     = frame->scause;

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
        /* ── 同步异常路径（暂时：打印信息并挂起）────────────────── */
        KLOG_ERROR("Unhandled exception: scause=0x%lx sepc=0x%lx stval=0x%lx\n",
                   cause, frame->sepc, frame->stval);
        while (1)
            __asm__ volatile("wfi");
    }
}
