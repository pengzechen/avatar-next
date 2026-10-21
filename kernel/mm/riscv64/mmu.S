/*
 * kernel/mm/riscv64/mmu.S
 *
 * 最小 Sv39 初始化。内核低地址恒等映射与 KERNEL_VMA 高半区别名
 * **都从 include/riscv64/mem_layout.h 的 RV_KERNEL_MAP_LIST 生成**，
 * 不再在这里手写 0x00000000/0x40000000/0x80000000/0xC0000000 和
 * 0x100..0x103 的 L1 下标。
 *
 * 同一份列表也被 satp_utils.h 用来复制内核高半区条目到用户页表，
 * 所以 mmu.S 和用户页表不会再出现"一边加了一条、另一边漏了"的漂移
 * （历史上 0x103 就这么漏过，症状是 syscall 期间 Store PF）。
 */

#include "riscv64/mem_layout.h"

/* 构造 1GB 叶子项：输入寄存器里的物理基地址需 1GB 对齐 */
.macro MAKE_GIGA_PTE out, pa_reg, flags
    srli    \out, \pa_reg, PAGE_SHIFT
    slli    \out, \out, RV_PTE_PPN_SHIFT
    ori     \out, \out, \flags
.endm

.macro ADD_PTE_ATTR pte, attr
    li      t6, \attr
    or      \pte, \pte, t6
.endm

/*
 * ADD_KERNEL_MAP pa, attr
 *   - 低地址恒等映射：L2[pa >> 30]
 *   - 高半区别名：L2[RV_KERNEL_L1_BASE_INDEX + (pa >> 30)]
 */
.macro ADD_KERNEL_MAP pa, attr
    li      t1, \pa
    MAKE_GIGA_PTE t2, t1, RV_PTE_LEAF_RWX
    ADD_PTE_ATTR t2, \attr

    li      t3, ((\pa) >> 30) * 8
    add     t3, t0, t3
    sd      t2, 0(t3)

    li      t3, (RV_KERNEL_L1_BASE_INDEX + ((\pa) >> 30)) * 8
    add     t3, t0, t3
    sd      t2, 0(t3)
.endm

.section .text.boot
.align 2
.global mmu_init
mmu_init:
    /* 清零根页表 */
    la      t0, rv_l2_root
    li      t1, PAGE_SIZE / 8
1:
    sd      zero, 0(t0)
    addi    t0, t0, 8
    addi    t1, t1, -1
    bnez    t1, 1b

    la      t0, rv_l2_root

#define RV_EMIT_KERNEL_MAP(pa, attr) ADD_KERNEL_MAP pa, attr
    RV_KERNEL_MAP_LIST(RV_EMIT_KERNEL_MAP)
#undef RV_EMIT_KERNEL_MAP

    /* satp = Sv39 | PPN(root) */
    la      t0, rv_l2_root
    srli    t0, t0, PAGE_SHIFT
    li      t1, RV_SATP_MODE_SV39
    or      t0, t0, t1
    csrw    satp, t0
    sfence.vma
    fence.i
    ret

.global mmu_disable
mmu_disable:
    csrw    satp, zero
    sfence.vma
    fence.i
    ret

.section .bss.boot
.align 12
.global rv_l2_root
rv_l2_root:
    .space PAGE_SIZE
