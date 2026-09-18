#ifndef AARCH64_HALT_ARCH_H_
#define AARCH64_HALT_ARCH_H_

/*
 * AArch64 架构特定的 halt 实现
 */

#include "barrier.h"
#include "aarch64/exception_impl.h"

/*
 * arch_halt - 停止 AArch64 处理器
 * 使用 WFI (Wait For Interrupt) 指令进入低功耗状态
 * 在中断禁用的情况下，WFI 会永久停止
 */
static inline void arch_halt(void)
{
    /* 内存屏障，确保所有操作完成 */
    barrier_data();
    barrier_instr_full();

    /* 禁用中断（统一接口；这里只掩 IRQ，本内核不使用 FIQ/SError） */
    arch_irq_disable();

    /* WFI - Wait For Interrupt */
    /* 在中断禁用的情况下，处理器会永久停止 */
    __asm__ volatile("wfi" ::: "memory");

    /* 如果 WFI 被唤醒（不应该发生），死循环 */
    while (1) {
        __asm__ volatile("wfi" ::: "memory");
    }
}

#endif  // AARCH64_HALT_ARCH_H_
