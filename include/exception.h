/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#ifndef EXCEPTION_H
#define EXCEPTION_H

/*
   include/exception.h — 异常处理公共接口

   架构特定的 trap_frame_t / irq_handler_t 定义在各自子目录：
     include/aarch64/exception.h  — AArch64 专用(trap_frame, hsr, ...)
     include/riscv64/exception.h  — RISC-V 专用(trap_frame, scause 常量)

   与 barrier.h / cache.h 相同的分发模式。

   另外在这里统一暴露中断屏蔽原语(arch_irq_save/restore/enable/
   disable/flags/is_enabled)：它们的实现在 include/<arch>/exception_impl.h,
   全项目只有那一份 —— C 代码要关中断就用这些, 不要再自己写内联汇编。
   详见 docs/basic/INTERRUPT_MASKING.md。
*/

#include "types.h"
#include "arch.h"

#if ARCH_AARCH64
#include "aarch64/exception.h"
#include "aarch64/exception_impl.h"
#elif ARCH_RISCV64
#include "riscv64/exception.h"
#include "riscv64/exception_impl.h"
#elif ARCH_X86_64
#include "x86_64/exception.h"
#include "x86_64/exception_impl.h"
#endif

/*
   设备中断请用 include/driver/irq.h 的 irq_register()。
   RISC-V 的 scause 级钩子(PLIC 外部中断/timer/IPI)仍由
   include/riscv64/exception.h 里的 irq_install() 提供 —— 那属于异常入口,
   不是设备驱动接口。
*/

#endif /* EXCEPTION_H */
