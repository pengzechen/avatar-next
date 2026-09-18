#ifndef RISCV64_HALT_ARCH_H_
#define RISCV64_HALT_ARCH_H_

/*
 * RISC-V 64 位架构特定的 halt 实现
 */

#include "barrier.h"
#include "riscv64/exception_impl.h"

/*
 * arch_halt - 停止 RISC-V 处理器
 * 使用 WFI (Wait For Interrupt) 指令进入低功耗状态
 * 在中断禁用的情况下，WFI 会永久停止
 */
static inline void arch_halt(void)
{
    /* 内存屏障，确保所有操作完成 */
    barrier_data();

    /*
     * 禁用中断。
     *
     * 这里原本写的是 `csrci mstatus, 0x8`（MIE）—— 两处都错：
     *   1) MIE 是 **M 模式**的中断使能位，本内核跑在 S 模式，该用 sstatus.SIE；
     *   2) S 模式访问 mstatus 会触发非法指令异常，"关中断"实际没生效。
     * 统一走 arch_irq_disable()。
     */
    arch_irq_disable();

    /* WFI - Wait For Interrupt */
    /* 在中断禁用的情况下，处理器会永久停止 */
    __asm__ volatile("wfi" ::: "memory");

    /* 如果 WFI 被唤醒（不应该发生），死循环 */
    while (1) {
        __asm__ volatile("wfi" ::: "memory");
    }
}

#endif  // RISCV64_HALT_ARCH_H_
