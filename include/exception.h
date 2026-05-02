#ifndef EXCEPTION_H
#define EXCEPTION_H

/*
 * include/exception.h — 异常处理公共接口
 *
 * 架构特定的 trap_frame_t / irq_handler_t 定义在各自子目录：
 *   include/aarch64/exception.h  — AArch64 专用（trap_frame, hsr, ...）
 *   include/riscv64/exception.h  — RISC-V 专用（trap_frame, scause 常量）
 *
 * 与 barrier.h / cache.h 相同的分发模式。
 */

#include "types.h"
#include "arch.h"

#if ARCH_AARCH64
#  include "aarch64/exception.h"
#elif ARCH_RISCV64
#  include "riscv64/exception.h"
#elif ARCH_X86_64
#  include "x86_64/exception.h"
#endif

/*
 * 公共接口 — irq_handler_t 在各架构头中定义
 *
 * AArch64:  void irq_install(int gic_vector, irq_handler_t h)
 * RISC-V:   void irq_install(int scause,     irq_handler_t h)
 */
void irq_install(int vector, irq_handler_t h);

#endif /* EXCEPTION_H */


