/*
 * 架构检测工具
 * 用于验证编译器架构设置
 *
 * 编译并运行:
 *   gcc -o arch_check arch_check.c && ./arch_check
 */

#include <stdio.h>

int main(void)
{
    printf("=== Architecture Detection ===\n\n");

#if defined(__x86_64__)
    printf("Detected Architecture: x86_64 (AMD64/Intel 64)\n");
    printf("__x86_64__ defined: %d\n", __x86_64__);
#elif defined(__aarch64__)
    printf("Detected Architecture: AArch64 (ARM 64-bit)\n");
    printf("__aarch64__ defined: %d\n", __aarch64__);
    #ifdef __ARM_ARCH
    printf("ARM Architecture version: %d\n", __ARM_ARCH);
    #endif
#elif defined(__riscv)
    printf("Detected Architecture: RISC-V 64-bit\n");
    printf("__riscv defined: %d\n", __riscv);
    printf("__riscv_xlen: %d (bit width)\n", __riscv_xlen);
#else
    printf("Unknown Architecture!\n");
    printf("Please compile with:\n");
    printf("  x86_64:    gcc -D__x86_64__ ...\n");
    printf("  AArch64:   gcc -D__aarch64__ ...\n");
    printf("  RISC-V64:  gcc -D__riscv -march=rv64gc ...\n");
#endif

    printf("\n=== Compiler Information ===\n");
    printf("__SIZEOF_POINTER__: %zu\n", sizeof(void *));
    printf("__SIZEOF_LONG__: %zu\n", sizeof(long));
    printf("__SIZEOF_LONG_LONG__: %zu\n", sizeof(long long));

    return 0;
}
