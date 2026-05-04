/*
 * kernel/mm/riscv64/mmu.S
 *
 * 最小 Sv39 初始化：
 * - 低地址恒等映射（L2 1GB 叶子）：
 *   * 0x00000000..0x3fffffff  (MMIO)
 *   * 0x80000000..0xbfffffff  (RAM)
 * - 高地址别名映射（KERNEL_VMA = 0xffffffc000000000）：
 *   * KERNEL_VMA + 0x00000000..0x3fffffff
 *   * KERNEL_VMA + 0x80000000..0xbfffffff
 */

.equ PGSZ,            4096
.equ PTE_V,           0x001
.equ PTE_R,           0x002
.equ PTE_W,           0x004
.equ PTE_X,           0x008
.equ PTE_A,           0x040
.equ PTE_D,           0x080
.equ PTE_TABLE,       PTE_V
.equ PTE_LEAF_RWX,    (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D)
.equ SATP_MODE_SV39,  (8 << 60)

/* 构造 1GB 叶子项：输入物理基地址需 1GB 对齐 */
.macro MAKE_GIGA_PTE out, pa, flags
    srli    \out, \pa, 12
    slli    \out, \out, 10
    ori     \out, \out, \flags
.endm

.section .text.boot
.align 2
.global mmu_init
mmu_init:
    /* 清零根页表 */
    la      t0, rv_l2_root
    li      t1, PGSZ / 8
1:
    sd      zero, 0(t0)
    addi    t0, t0, 8
    addi    t1, t1, -1
    bnez    t1, 1b

    /* L2[0] = 低地址 0x00000000 (1GB) */
    la      t0, rv_l2_root
    li      t1, 0x00000000
    MAKE_GIGA_PTE t2, t1, PTE_LEAF_RWX
    sd      t2, 0(t0)

    /* L2[2] = 低地址 0x80000000 (1GB) */
    li      t1, 0x80000000
    MAKE_GIGA_PTE t2, t1, PTE_LEAF_RWX
    sd      t2, (2 * 8)(t0)

    /* L2[0x100] = 高地址别名 -> 物理 0x00000000 (1GB) */
    li      t1, 0x00000000
    MAKE_GIGA_PTE t2, t1, PTE_LEAF_RWX
    li      t3, (0x100 * 8)
    add     t3, t0, t3
    sd      t2, 0(t3)

    /* L2[0x102] = 高地址别名 -> 物理 0x80000000 (1GB) */
    li      t1, 0x80000000
    MAKE_GIGA_PTE t2, t1, PTE_LEAF_RWX
    li      t3, (0x102 * 8)
    add     t3, t0, t3
    sd      t2, 0(t3)

    /* satp = Sv39 | PPN(root) */
    la      t0, rv_l2_root
    srli    t0, t0, 12
    li      t1, SATP_MODE_SV39
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
rv_l2_root:
    .space PGSZ
