/*
 * RISC-V 64-bit boot code for QEMU virt machine
 */

.section .text.boot
.global _start
.type _start, @function

.extern mmu_init

_start:

    /* Test: write to UART to indicate boot success */
    // li      t0, 0x10000000  /* UART base */
    // li      t1, 0x41  /* 'A' */
    // sb      t1, 0(t0)

    /* Note: Running in S-mode (managed by OpenSBI), no need to disable interrupts */
    csrw sie, zero
    csrw sip, zero

    /* Enable FPU: set sstatus.FS = Initial (01) so FP instructions don't trap */
    li      t0, (1 << 13)
    csrs    sstatus, t0

    /* MMU 前使用低地址引导栈 */
    la      sp, boot_stack_top

    /* 启用 MMU（建立低地址恒等映射 + 高地址内核映射） */
    call    mmu_init

    /* 高半区偏移（与 link.ld 保持一致） */
    li      t2, 0xffffffc000000000

    /* 先清理 BSS（通过低地址别名访问物理加载区） */
    call    clear_bss

    /* MMU 开启后，再初始化高地址 GP */
    .option push
    .option norelax
    la      t0, __global_pointer_phys
    add     gp, t0, t2
    .option pop

    /* 切换到高地址内核栈 */
    la      t0, stack_top_phys
    add     sp, t0, t2

    /* 跳转到高地址 C 入口 */
    la      t0, kernel_main_phys
    add     t0, t0, t2
    jr      t0

    /* Hang if kernel_main returns */
hang:
    wfi
    j       hang


clear_bss:
    # 获取 BSS 段低地址别名（由链接器提供）
    la   t0, __bss_start_phys
    la   t1, __bss_end_phys

bss_clear_loop:
    # 检查是否已经清理完毕
    bgeu t0, t1, bss_clear_done    # 如果 t0 >= t1，清理完成

    # 清零当前 8 字节（RISC-V 64位）
    sd   zero, 0(t0)               # 存储 0 到当前地址
    addi t0, t0, 8                 # 移动到下一个 8 字节
    j    bss_clear_loop            # 继续循环

bss_clear_done:
    ret                            # 返回调用者


.section .bss.boot
.align 12
.global boot_stack_bottom
.global boot_stack_top

boot_stack_bottom:
    .space 0x4000
boot_stack_top:

.section .bss
.align 12                          # 按 4KB 对齐（页对齐）
/* Stack allocation (64KB) — lwext4 mount path requires significant stack depth */
.global stack_bottom
.global stack_top

stack_bottom:
    .space 0x10000
stack_top:
