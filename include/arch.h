#ifndef ARCH_H
#define ARCH_H

/*
 * 架构检测和配置
 * 用于在编译时确定目标架构
 */

#if defined(ARCH_X86_64)
    #define ARCH_NAME "x86_64"
#elif defined(ARCH_AARCH64)
    #define ARCH_NAME "aarch64"
#elif defined(ARCH_RISCV64)
    #define ARCH_NAME "riscv64"
#else
    #error "Unsupported architecture - please define ARCH_X86_64, ARCH_AARCH64 or ARCH_RISCV64"
#endif

/*
 * 架构特定的头文件路径
 */
#define ARCH_INCLUDE_PATH(arch, file) #arch "/" #file

#endif  // ARCH_H
