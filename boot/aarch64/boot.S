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
     * ── EL2/VHE 初始化 ────────────────────────────────────────
     *
     * QEMU -machine virt,virtualization=on 从 EL2 进入。
     * 启用 VHE（Virtualization Host Extensions），让内核在 EL2 运行：
     *
     *   HCR_EL2.E2H  (bit 34) = 1  → 启用 VHE；EL1 寄存器名自动
     *                                 别名为对应 EL2 寄存器，现有代码
     *                                 零修改透明兼容。
     *   HCR_EL2.RW   (bit 31) = 1  → Guest EL1 为 AArch64
     *   HCR_EL2.TGE  (bit 27) = 1  → EL0 异常直接陷入 EL2
     *                                 （syscall/IRQ 路由不变）
     *
     * VHE 寄存器别名（硬件透明，无需修改现有汇编/C）：
     *   msr vbar_el1  → VBAR_EL2    msr elr_el1   → ELR_EL2
     *   msr spsr_el1  → SPSR_EL2    msr ttbr0_el1 → TTBR0_EL2
     *   msr ttbr1_el1 → TTBR1_EL2   msr tcr_el1   → TCR_EL2
     *   msr mair_el1  → MAIR_EL2    msr sctlr_el1 → SCTLR_EL2
     *   mrs esr_el1   → ESR_EL2     mrs far_el1   → FAR_EL2
     */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2              /* x0 = 当前 EL（数字 1 或 2）  */
    cmp     x0, #2
    b.ne    .Lskip_el2_init         /* EL1 fallback（不支持 VHE 时跳过）*/

    /* HCR_EL2: E2H(34) | RW(31) | TGE(27) */
    mov     x1, xzr
    orr     x1, x1, #(1 << 34)     /* E2H  */
    orr     x1, x1, #(1 << 31)     /* RW   */
    orr     x1, x1, #(1 << 27)     /* TGE  */
    msr     hcr_el2, x1
    isb

    /*
     * 允许 FP/NEON：清除 CPTR_EL2.TFP（bit 10）。
     * 默认值 TFP=1 会把 FP 指令陷入 EL2，导致 kernel panic。
     */
    mrs     x1, cptr_el2
    bic     x1, x1, #(1 << 10)
    msr     cptr_el2, x1

    /*
     * 开放 EL0/EL1 定时器访问（CNTHCTL_EL2）：
     *   bit 0 EL0PCTEN  = 1  不陷入 EL0 的 CNTPCT 读取
     *   bit 1 EL0VCTEN  = 1  不陷入 EL0 的 CNTVCT 读取
     *   bit 10 EL1PTEN  = 1  不陷入 EL1 物理定时器访问
     */
    mov     x1, #(1 | (1 << 1) | (1 << 10))
    msr     cnthctl_el2, x1
    isb

    /*
     * 清除 PSTATE.PAN（Privileged Access Never，ARMv8.1 FEAT_PAN）。
     * cortex-a76 支持 PAN；若不清除，EL2 内核访问 EL0 可访问的页会
     * 触发 EC=0x25 permission fault（Data Abort from current EL）。
     * 同时在 mmu.S 中将 SCTLR_EL1.SPAN=1，防止陷入 EL2 时 PAN 被
     * 自动重新置位。
     */
.arch_extension pan
    msr     pan, #0
    isb

.Lskip_el2_init:
    /* ── 以下与原 EL1 启动路径完全一致（VHE 别名透明）──────── */

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
