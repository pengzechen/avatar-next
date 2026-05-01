#ifndef X86_64_HALT_ARCH_H_
#define X86_64_HALT_ARCH_H_

/*
 * x86_64 架构特定的 halt 实现
 */

#include "barrier.h"

/*
 * arch_halt - 停止 x86_64 处理器
 * 使用 HLT 指令停止处理器，直到收到中断
 * 在中断禁用的情况下，HLT 会永久停止
 */
static inline void arch_halt(void)
{
    /* 内存屏障，确保所有操作完成 */
    barrier_data();

    /* 禁用中断 */
    __asm__ volatile("cli" ::: "memory");

    /* HLT - Halt Processor */
    /* 在中断禁用的情况下，处理器会永久停止 */
    __asm__ volatile("hlt" ::: "memory");

    /* 如果 HLT 被唤醒（不应该发生），死循环 */
    while (1) {
        __asm__ volatile("hlt" ::: "memory");
    }
}

#endif  // X86_64_HALT_ARCH_H_
