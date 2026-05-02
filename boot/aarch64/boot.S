/*
 * AArch64 boot code for QEMU virt machine
 * Entry point: _start at 0x40000000
 */

.section .text.boot
.global _start
.type _start, @function

_start:
    /*
     * QEMU virt machine starts at EL1 (Exception Level 1)
     * We're already in 64-bit mode with MMU disabled
     */

    /*
     * Set stack pointer
     * Stack grows downward, so we set SP to stack_top
     */
    adrp    x0, stack_top
    add     x0, x0, :lo12:stack_top
    mov     sp, x0

    /*
     * Set exception vector base address
     * VBAR_EL1 = &exception_vector_base
     */
    adrp    x0, exception_vector_base
    add     x0, x0, :lo12:exception_vector_base
    // ldr x0, =exception_vector_base
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
     * Clear BSS section
     * BSS contains zero-initialized data
     */
    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end
    mov     x2, #0
clear_bss:
    cmp     x0, x1
    b.ge    clear_bss_done
    str     x2, [x0], #8
    b       clear_bss
clear_bss_done:

    /*
     * Jump to C code
     * kernel_main() never returns
     */
    bl      kernel_main

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
