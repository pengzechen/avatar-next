#ifndef AARCH64_MEM_LAYOUT_H
#define AARCH64_MEM_LAYOUT_H

/*
 * include/aarch64/mem_layout.h — AArch64 内核高半区基址的唯一定义
 *
 * Makefile 会以 -DAARCH64_KERNEL_VMA_OFF=... 传入；link.ld 通过
 * --defsym=KERNEL_VMA_OFF 得到同一个值。这里的 fallback 仅为单独
 * 包含本头文件的场合（IDE/静态分析）保留。
 *
 * 本文件只放 #define，保证 .S 也能 include。
 */
#ifndef AARCH64_KERNEL_VMA_OFF
#define AARCH64_KERNEL_VMA_OFF 0xffff000000000000
#endif

#endif /* AARCH64_MEM_LAYOUT_H */
