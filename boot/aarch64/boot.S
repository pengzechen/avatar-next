/*
 * AArch64 boot code for QEMU virt machine
 * Entry point: _start at 0x40000000
 *
 * 支持多核启动：
 * - CPU0: 执行完整启动流程，调用 kernel_main
 * - CPU1-3: 通过 PSCI CPU_ON 启动，执行 secondary_cpu_start
 */

.section .text
.global _start
.global secondary_cpu_start
.type _start, @function
.type secondary_cpu_start, @function

.extern vm_init
.extern mmu_init
.extern kernel_main
.extern cpu_init
.extern cpu_bring_up_all
.extern cpu_secondary_bootstrap
.extern g_cpus
.extern sched_schedule
.extern gic_init_secondary
.extern timer_init_secondary
.extern vm_get_kernel_pgtable
.extern exception_vector_base

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
     * 为 CPU0 初始化 TPIDR_EL1，指向 g_cpus[0]
     * 这需要在 MMU 启用后进行，因为 g_cpus 的虚拟地址需要有效。
     * 但由于我们使用 identity mapping，这里可以直接设置物理地址。
     * 暂时存储在 x20（被调用者保存寄存器）
     */
    adrp    x20, g_cpus
    add     x20, x20, :lo12:g_cpus

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
     * 为 CPU0 设置 TPIDR_EL1，指向 g_cpus[0]
     * 之前保存在 x20 中的 g_cpus 物理地址现在需要转换为虚拟地址。
     * 由于整个内核映射到 0xffff000000000000，而物理地址在 0-2GB，
     * 虚拟地址 = 0xffff000000000000 + 物理地址
     * 但更简单的做法是直接 adrp/add 高地址的 g_cpus
     */
    adrp    x0, g_cpus
    add     x0, x0, :lo12:g_cpus
    msr     tpidr_el1, x0           /* CPU0: TPIDR_EL1 = &g_cpus[0] */
    isb

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
 * ── 二级启动代码（CPU1-3 入口） ───────────────────────────────
 *
 * 当通过 PSCI CPU_ON 启动新 CPU 时，CPU 从这里开始执行。
 * 此时 MMU 已经启用（kernel_main 已配置好），但该 CPU 的
 * TPIDR_EL1 还未设置。
 */
secondary_cpu_start:
    /*
     * 获取当前 CPU 的 ID
     * 从 PSCI CPU_ON 调用时，PSCI 将 x3 设置为传递的 context 值。
     * 在 cpu_bring_up_all 中，我们传递了 CPU ID。
     * 但这里没有直接的寄存器保存，所以需要从 MPIDR_EL1 解析。
     */
    mrs     x20, mpidr_el1
    and     x19, x20, #0xFF           /* x19 = cpu_id (Aff0) */

    /*
     * 次级核 EL2/VHE 初始化（与主核一致）
     */
    mov     x1, xzr
    orr     x1, x1, #(1 << 34)        /* E2H */
    orr     x1, x1, #(1 << 31)        /* RW  */
    orr     x1, x1, #(1 << 27)        /* TGE */
    msr     hcr_el2, x1
    isb

    mrs     x1, cptr_el2
    bic     x1, x1, #(1 << 10)
    msr     cptr_el2, x1

    mov     x1, #(1 | (1 << 1) | (1 << 10))
    msr     cnthctl_el2, x1
    isb

.arch_extension pan
    msr     pan, #0
    isb

    msr     daifset, #2

    /*
     * 次级核启用 MMU（复用主核已建立的内核页表）
     */
    bl      vm_get_kernel_pgtable
    mov     x1, x0
    bl      mmu_init

    /* 跳到高虚拟地址继续执行 */
    ldr     x0, =secondary_cpu_high
    br      x0

secondary_cpu_high:

    /*
     * 设置该 CPU 的栈指针
     * 每个 CPU 需要自己的栈
     */
    adrp    x1, secondary_boot_stacks
    add     x1, x1, :lo12:secondary_boot_stacks
    mov     x2, #4096
    mul     x3, x19, x2
    add     x1, x1, x3
    add     x1, x1, x2
    mov     sp, x1

    /* 安装本核异常向量（VHE 下 vbar_el1 别名到 VBAR_EL2） */
    adrp    x0, exception_vector_base
    add     x0, x0, :lo12:exception_vector_base
    msr     vbar_el1, x0
    dsb     sy
    isb

    /*
     * 调用 cpu_init() 初始化该 CPU 的就绪队列等
     */
    mov     x0, x19                 /* arg0 = cpu_id */
    mov     x1, x20                 /* arg1 = mpidr */
    bl      cpu_secondary_bootstrap

    /* 次级核本地中断控制器和定时器初始化 */
    bl      gic_init_secondary
    bl      timer_init_secondary

    /*
     * 开启中断
     */
    msr     daifclr, #2
    isb

    /*
     * 调用调度器进入主循环
     * 由于每个 CPU 有自己的 per-cpu 就绪队列，调度器会
     * 从该队列中选择任务运行。
     */
secondary_sched_loop:
    adrp    x0, sched_schedule
    add     x0, x0, :lo12:sched_schedule
    blr     x0
    wfe
    b       secondary_sched_loop

    /*
     * 不应该到达这里；如果返回，则无限循环
     */
secondary_hang:
    wfe
    b       secondary_hang

.size secondary_cpu_start, . - secondary_cpu_start

.section .bss
.align 12
secondary_boot_stacks:
    .skip (8 * 4096)

/*
 * Stack symbols are defined in linker script
 * No need to define them here
 */
