#ifndef SYSCALL_ABI_H
#define SYSCALL_ABI_H

/*
 * include/syscall_abi.h — 系统调用寄存器访问公共接口
 *
 * 将 trap_frame 的架构差异封装在各架构子目录，
 * 让 kernel/syscall 代码只依赖统一 ABI 访问函数。
 */

#include "arch.h"
#include "exception.h"

#if ARCH_AARCH64
#  include "aarch64/syscall_abi.h"
#elif ARCH_RISCV64
#  include "riscv64/syscall_abi.h"
#elif ARCH_X86_64
#  include "x86_64/syscall_abi.h"
#else
#  error "Unsupported architecture for syscall ABI"
#endif

#endif /* SYSCALL_ABI_H */
