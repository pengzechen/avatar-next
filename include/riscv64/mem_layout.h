#ifndef RISCV64_MEM_LAYOUT_H
#define RISCV64_MEM_LAYOUT_H

/*
 * include/riscv64/mem_layout.h — 内核 1GiB 映射的单一来源
 *
 * 同一份列表同时用于：
 *   - kernel/mm/riscv64/mmu.S 的启动页表（低地址恒等 + 高半区别名）；
 *   - satp_utils.h 创建用户页表时复制内核高半区条目。
 *
 * 列表项以分号结尾：C 展开成语句，汇编展开成以 ';' 分隔的指令。
 *
 * 历史教训：0x100..0x103 这组高半区 L1 下标曾在 mmu.S 和
 * satp_utils.h 各写一遍，0x103 有一边漏加，导致 PMM 分配到
 * 0xc0000000 以上的页时，在 syscall 期间（satp 指向用户页表）
 * 触发 Store PF。现在两边都从 RV_KERNEL_MAP_LIST 生成。
 */

#include "riscv64/pte.h"

/*
 * 内核高半区基址。Makefile 会以 -DRV_KERNEL_VMA_OFF=... 传入同一个值，
 * link.ld 则通过 --defsym 得到它；这里的 fallback 只为单独包含本头文件
 * 的场合（以及 IDE/静态分析）保留。
 */
#ifndef RV_KERNEL_VMA_OFF
#define RV_KERNEL_VMA_OFF 0xffffffc000000000
#endif

/*
 * KERNEL_VMA 所在的根页表 L1 下标。
 * KERNEL_VMA = 0xffffffc000000000，1GiB 对齐：
 *   (KERNEL_VMA >> 30) & 0x1FF = 0x100
 */
#define RV_KERNEL_L1_BASE_INDEX                                       \
    ((RV_KERNEL_VMA_OFF >> RV_LEVEL_SHIFT(2)) & (RV_PT_ENTRIES - 1))

#define RV_KERNEL_HIGH_INDEX(pa) \
    (RV_KERNEL_L1_BASE_INDEX + ((pa) >> RV_LEVEL_SHIFT(2)))

/*
 * 低地址两个 1GiB MMIO 窗口。QEMU virt 与 SG2002 都使用
 * 0x00000000..0x7fffffff 这段（UART/PLIC 等），保持恒等映射。
 */
#define RV_KERNEL_MAP_MMIO(X)                  \
    X(0x00000000, RV_PTE_ATTR_IOREMAP);        \
    X(0x40000000, RV_PTE_ATTR_IOREMAP);

/*
 * RAM：从平台 RAM 基址起每个 1GiB 一条。
 * QEMU virt 是 2GiB（0x80000000 + 0xC0000000），
 * SG2002 是 256MiB（只有 0x80000000）。
 */
#define RV_KERNEL_MAP_RAM0(X) \
    X(PLATFORM_MEM_RAM_BASE, RV_PTE_ATTR_NORMAL);

#if (PLATFORM_MEM_RAM_SIZE > 0x40000000)
#define RV_KERNEL_MAP_RAM1(X) \
    X(PLATFORM_MEM_RAM_BASE + 0x40000000, RV_PTE_ATTR_NORMAL);
#else
#define RV_KERNEL_MAP_RAM1(X)
#endif

#define RV_KERNEL_MAP_LIST(X) \
    RV_KERNEL_MAP_MMIO(X)     \
    RV_KERNEL_MAP_RAM0(X)     \
    RV_KERNEL_MAP_RAM1(X)

#endif /* RISCV64_MEM_LAYOUT_H */
