/*
 * AArch64 boot code for QEMU virt machine
 * Entry point: _start at 0x40000000
 */

.section .text
.global _start
.type _start, @function

.extern vm_init
.extern mmu_init
.extern kernel_main

_start:
    /*
     * QEMU virt machine starts at EL1 (Exception Level 1)
     * We're already in 64-bit mode with MMU disabled
     */
    msr daifset, #2   // 关闭所有中断

    /*
     * Set stack pointer
     * Stack grows downward, so we set SP to stack_top
     */
    ldr x0, =stack_top      // 加载 _stack_top 地址
    and x0, x0, #0xFFFFFFFF  // 屏蔽高 32 位
    mov sp, x0               // 设置栈指针

//     /*
//      * Clear BSS section
//      * BSS contains zero-initialized data
//      */
//     adrp    x0, __bss_start
//     add     x0, x0, :lo12:__bss_start
//     adrp    x1, __bss_end
//     add     x1, x1, :lo12:__bss_end
//     mov     x2, #0
// clear_bss:
//     cmp     x0, x1
//     b.ge    clear_bss_done
//     str     x2, [x0], #8
//     b       clear_bss
// clear_bss_done:

    /*
     * 初始化虚拟内存管理
     * 建立页表映射（0-1GB 设备内存，1GB-2GB 普通内存）
     * 此时 MMU 未启用，使用物理地址
     */
    bl      vm_init

    /*
     * 启用 MMU
     * x0 = TTBR0（用户空间页表，暂时为 0）
     * x1 = TTBR1（内核空间页表）
     *
     * vm_init() 返回后，x1 包含内核页表物理地址
     */
    bl      vm_get_kernel_pgtable   // 获取内核页表物理地址
    // vm_get_kernel_pgtable 返回 x0 = 内核页表物理地址

    mov     x1, x0                  // x1 = TTBR1
    bl      mmu_init

    /*
     * Trampoline：从 identity-mapped 低地址跳到高虚拟地址
     *
     * mmu_init 返回时 LR 里保存的是低地址，ret 后 PC 仍在低地址。
     * TTBR0 的 identity map 让低地址代码暂时可以继续执行，但必须
     * 在这里显式跳到高 VMA，否则所有 PC 相对指令（adrp 等）都会
     * 算出低地址结果。
     *
     * ldr x0, =1f 从字面量池加载 label 1f 的链接地址（高地址）
     * br  x0       跳过去，之后 PC = 0xffff000040080000 + offset
     */
    ldr     x0, =1f
    br      x0
1:
    /* ── 以下代码在高虚拟地址（TTBR1 映射）下执行 ────────────── */

    /*
     * 重新设置栈指针（使用高地址）
     */
    ldr     x0, =stack_top
    mov     sp, x0

    /*
     * Set exception vector base address
     * VBAR_EL1 = &exception_vector_base
     */
    adrp    x0, exception_vector_base
    add     x0, x0, :lo12:exception_vector_base
    msr     vbar_el1, x0
    dsb     sy
    isb

    /*
     * Enable IRQ interrupts (clear DAIF.I bit)
     */
    msr     daifclr, #2
    dsb     sy
    isb

    /*
     * Jump to C code
     * kernel_main() never returns
     */
    adrp x0, kernel_main
    add  x0, x0, :lo12:kernel_main
    blr  x0

    /*
     * If kernel_main returns (should not happen)
     * Hang forever
     */
hang:
    wfe
    b       hang

.size _start, . - _start

/*
 * Stack symbols are defined in linker script
 * No need to define them here
 */
