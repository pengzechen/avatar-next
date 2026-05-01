#ifndef ARCH_H
#define ARCH_H

/*
 * 架构检测和配置
 * 用于在编译时确定目标架构
 */

#if defined(__x86_64__)
    #define ARCH_X86_64 1
    #define ARCH_NAME "x86_64"
#elif defined(__aarch64__)
    #define ARCH_AARCH64 1
    #define ARCH_NAME "aarch64"
#elif defined(__riscv)
    #define ARCH_RISCV64 1
    #define ARCH_NAME "riscv64"
#else
    #error "Unsupported architecture - please define __x86_64__, __aarch64__ or __riscv"
#endif

/*
 * 架构特定的头文件路径
 */
#define ARCH_INCLUDE_PATH(arch, file) #arch "/" #file

#endif  // ARCH_H
