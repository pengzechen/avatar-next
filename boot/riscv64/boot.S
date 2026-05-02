/*
 * RISC-V 64-bit boot code for QEMU virt machine
 */

.section .text.boot
.global _start
.type _start, @function

_start:

    /* Test: write to UART to indicate boot success */
    li      t0, 0x10000000  /* UART base */
    li      t1, 0x41  /* 'A' */
    sb      t1, 0(t0)

    /* Note: Running in S-mode (managed by OpenSBI), no need to disable interrupts */
    csrw sie, zero
    csrw sip, zero

    /* Initialize global pointer (gp-relative addressing optimization).
     * Must use .option norelax to prevent the assembler itself from
     * replacing this la with a gp-relative access (circular dependency). */
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop

    /* Set stack pointer */
    la      sp, stack_top

    /* Test: write to UART to indicate boot success */
    li      t0, 0x10000000  /* UART base */
    li      t1, 0x42  /* 'B' */
    sb      t1, 0(t0)

    call    clear_bss

    /* Jump to C code */
    call    kernel_main

    /* Hang if kernel_main returns */
hang:
    wfi
    j       hang


clear_bss:
    # 获取 BSS 段的起始和结束地址（由链接器提供）
    la   t0, __bss_start           # BSS 段起始地址
    la   t1, __bss_end             # BSS 段结束地址

bss_clear_loop:
    # 检查是否已经清理完毕
    bgeu t0, t1, bss_clear_done    # 如果 t0 >= t1，清理完成

    # 清零当前 8 字节（RISC-V 64位）
    sd   zero, 0(t0)               # 存储 0 到当前地址
    addi t0, t0, 8                 # 移动到下一个 8 字节
    j    bss_clear_loop            # 继续循环

bss_clear_done:
    ret                            # 返回调用者


.section .bss
.align 12                          # 按 4KB 对齐（页对齐，为将来的 MMU 做准备）
/* Stack allocation (64KB) — lwext4 mount path requires significant stack depth */
.global stack_bottom
.global stack_top

stack_bottom:
    .space 0x10000
stack_top:
