/*
 * 架构检测工具
 * 用于验证编译器架构设置
 */

#include "klog.h"

void test_arch(void)
{
    KLOG_INFO("=== Architecture Detection ===");

#if defined(__x86_64__)
    KLOG_INFO("Detected Architecture: x86_64 (AMD64/Intel 64)");
    KLOG_INFO("__x86_64__ defined");
#elif defined(__aarch64__)
    KLOG_INFO("Detected Architecture: AArch64 (ARM 64-bit)");
    KLOG_INFO("__aarch64__ defined");
    #ifdef __ARM_ARCH
    KLOG_INFO("ARM Architecture version: %d", __ARM_ARCH);
    #endif
#elif defined(__riscv)
    KLOG_INFO("Detected Architecture: RISC-V 64-bit");
    KLOG_INFO("__riscv defined");
    KLOG_INFO("__riscv_xlen: %d (bit width)", __riscv_xlen);
#else
    KLOG_ERROR("Unknown Architecture!");
    KLOG_ERROR("Please compile with:");
    KLOG_ERROR("  x86_64:    gcc -D__x86_64__ ...");
    KLOG_ERROR("  AArch64:   gcc -D__aarch64__ ...");
    KLOG_ERROR("  RISC-V64:  gcc -D__riscv -march=rv64gc ...");
#endif

    KLOG_INFO("=== Compiler Information ===");
    KLOG_INFO("Pointer size: %u bytes", (unsigned int)sizeof(void *));
    KLOG_INFO("Long size: %u bytes", (unsigned int)sizeof(long));
    KLOG_INFO("Long long size: %u bytes", (unsigned int)sizeof(long long));
}

