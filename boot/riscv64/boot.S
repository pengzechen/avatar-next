/*
 * RISC-V 64-bit boot code for QEMU virt machine
 */

.section .text.boot
.global _start
.type _start, @function

.extern mmu_init
.extern cpu_secondary_bootstrap
.extern riscv64_boot_hartid_phys

.equ RISCV64_KERNEL_VMA_OFF, 0xffffffc000000000
.equ RISCV64_SATP_MODE_SV39, (8 << 60)
.equ RISCV64_SECONDARY_STACK_SIZE, 16384

_start:
    /* OpenSBI may choose any boot hart. The first hart reaching _start becomes
     * BSP; the rest park until cpu_bring_up_all() writes the release table. */
    la      t0, riscv64_boot_claim
    li      t1, 1
    amoswap.w.aq t2, t1, (t0)
    bnez    t2, secondary_park
    mv      s1, a0                       /* preserve boot hartid across calls */

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
    li      t2, RISCV64_KERNEL_VMA_OFF

    /* 先清理 BSS（通过低地址别名访问物理加载区） */
    call    clear_bss

    la      t0, riscv64_boot_hartid_phys
    sd      s1, 0(t0)

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
    mv      tp, zero                 /* early klog falls back to CPU0 before cpu_init_bsp */
    jr      t0

    /* Hang if kernel_main returns */
hang:
    wfi
    j       hang

secondary_park:
    csrw sie, zero
    csrw sip, zero
    mv   s1, a0                       /* hartid */
1:
    la   t0, riscv64_hart_release
    slli t1, s1, 3
    add  t0, t0, t1
    ld   t2, 0(t0)
    bnez t2, 2f
    wfi
    j 1b
2:
    csrwi sip, 0
    addi a1, t2, -1                   /* release slot stores cpu_id + 1 */
    j    _riscv64_secondary_start

/* SBI HSM secondary entry.
 *   a0 = hartid
 *   a1 = opaque cpu_id from sbi_hart_start()
 */
.global _riscv64_secondary_start
.type _riscv64_secondary_start, @function
_riscv64_secondary_start:
    csrw    sie, zero
    csrw    sip, zero

    li      t0, (1 << 13)
    csrs    sstatus, t0

    mv      s0, a1                      /* logical cpu_id */

    la      t0, secondary_boot_stacks_phys
    li      t1, RISCV64_SECONDARY_STACK_SIZE
    mul     t2, s0, t1
    add     t0, t0, t2
    add     sp, t0, t1

    /* Reuse the BSP-built boot page table. */
    la      t0, rv_l2_root
    srli    t0, t0, 12
    li      t1, RISCV64_SATP_MODE_SV39
    or      t0, t0, t1
    csrw    satp, t0
    sfence.vma
    fence.i

    li      t3, RISCV64_KERNEL_VMA_OFF

    .option push
    .option norelax
    la      t0, __global_pointer_phys
    add     gp, t0, t3
    .option pop

    add     sp, sp, t3

    mv      a0, s0
    lla     t0, .Lcpu_secondary_bootstrap_ptr
    ld      t0, 0(t0)
    jr      t0
.size _riscv64_secondary_start, . - _riscv64_secondary_start

.align 3
.Lcpu_secondary_bootstrap_ptr:
    .dword cpu_secondary_bootstrap


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

.align 12
.global secondary_boot_stacks
secondary_boot_stacks:
    .space (8 * RISCV64_SECONDARY_STACK_SIZE)

.section .rodata
.align 3
.global riscv64_secondary_start_phys
riscv64_secondary_start_phys:
    .dword _riscv64_secondary_start

.global riscv64_hart_release_phys
riscv64_hart_release_phys:
    .dword riscv64_hart_release

.section .data.boot
.align 3
.global riscv64_boot_claim
riscv64_boot_claim:
    .word 0
    .word 0

.global riscv64_hart_release
riscv64_hart_release:
    .zero (8 * 8)
