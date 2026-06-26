/*
 * AArch64 boot code for QEMU virt machine
 * Entry point: _start at 0x40000000
 */

.section .text.boot
.global _start
.type _start, @function

.extern vm_init
.extern mmu_init
.extern kernel_main

/*
 * ── EL2/VHE 初始化 ───────────────────────────────────────────────
 *
 * QEMU -machine virt,virtualization=on 从 EL2 进入；真实硬件也可能由
 * 固件移交到 EL2。启用 VHE 后，内核仍使用 EL1 寄存器名，硬件自动
 * 别名到对应 EL2 寄存器：
 *   msr vbar_el1  → VBAR_EL2    msr elr_el1   → ELR_EL2
 *   msr spsr_el1  → SPSR_EL2    msr ttbr0_el1 → TTBR0_EL2
 *   msr ttbr1_el1 → TTBR1_EL2   msr tcr_el1   → TCR_EL2
 *   msr mair_el1  → MAIR_EL2    msr sctlr_el1 → SCTLR_EL2
 *   mrs esr_el1   → ESR_EL2     mrs far_el1   → FAR_EL2
 *
 * clobber: x0, x1
 */
.macro init_el2_vhe
    mrs     x0, CurrentEL
    lsr     x0, x0, #2              /* x0 = 当前 EL（数字 1 或 2）  */
    cmp     x0, #2
    b.ne    1f                      /* EL1 fallback（不支持 VHE 时跳过）*/

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

#if defined(DRIVER_GIC_V3)
    /*
     * GICv3 CPU interface 系统寄存器访问使能（ICC_SRE_EL2）：
     *   bit 0 SRE     = 1  使用系统寄存器接口（非 memory-mapped）
     *   bit 1 DFB     = 1  禁用 FIQ 旁路
     *   bit 2 DIB     = 1  禁用 IRQ 旁路
     *   bit 3 Enable  = 1  允许 EL1 访问 ICC_SRE_EL1
     * RK3588 等真实硬件复位值不保证，必须显式设置，否则
     * EL1 写 ICC_SRE_EL1 被忽略，GIC CPU interface 永远不会激活。
     */
    mrs     x1, S3_4_C12_C9_5       /* ICC_SRE_EL2 */
    orr     x1, x1, #0xF
    msr     S3_4_C12_C9_5, x1
    isb
#endif

.arch_extension pan
    msr     pan, #0
    isb
1:
.endm

_start:
    init_el2_vhe

    /* ── 以下与原 EL1 启动路径完全一致（VHE 别名透明）──────── */

    msr daifset, #2   // 关闭 IRQ

    /*
     * Set stack pointer
     * Stack grows downward, so we set SP to stack_top
     */
    ldr x0, =stack_top_phys  // 加载物理栈顶地址
    mov sp, x0               // 设置栈指针

    /*
     * Clear BSS section（在 MMU 启用前必须完成）
     * BSS 未清零时 dw_uart_initialized/spinlock 等含 DRAM 垃圾值，
     * 导致 dw_uart_putchar 进入中断路径并在 garbage lock 上死循环。
     * 使用 link.ld 导出的物理别名，避免假设 PA 位于 VA 低 32 位。
     */
    ldr     x0, =__bss_start_phys
    ldr     x1, =__bss_end_phys
    mov     x2, #0
clear_bss:
    cmp     x0, x1
    b.ge    clear_bss_done
    str     x2, [x0], #8
    b       clear_bss
clear_bss_done:

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
    bl      vm_get_boot_pgtable      // x0 = boot_pt0 物理地址（TTBR0 恒等映射）
    mov     x27, x0                  // 保存到 callee-saved 寄存器
    bl      vm_get_kernel_pgtable    // x0 = kernel_pt0 物理地址（TTBR1）
    mov     x1, x0                   // x1 = TTBR1 = kernel_pt0
    mov     x0, x27                  // x0 = TTBR0 = boot_pt0
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
     * 此处不开中断（保持 DAIF.I=1）。
     * kernel_main 作为 boot 上下文运行，最终会变成 idle 内核线程，
     * 由 task_idle_loop / task_trampoline 在合适时机 arch_irq_enable()。
     * 在 timer/GIC 初始化完成前开中断没有意义且不安全。
     */
    // msr     daifclr, #2
    // dsb     sy
    // isb

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
 * ── 次级核启动入口（Phase 2 PSCI CPU_ON 入口）─────────────
 *
 * 由 PSCI SMC CPU_ON 进入，CPU 处于 EL2（QEMU virt + virtualization=on）。
 * 入口约定：
 *   x0 = context_id（BSP 在 psci_cpu_on() 中传入的 cpu_id，0..N-1）
 *
 * 与 _start 的区别：
 *   - 不清零 BSS（BSP 已清零，共享同一段 BSS）
 *   - 不调用 vm_init（页表已建好，secondary 直接复用 BSP 的内核 PGD）
 *   - 不使用 stack_top（BSP 专用），改用 secondary_boot_stacks[cpu_id]
 *   - 不进入 kernel_main，而是调用 cpu_secondary_bootstrap(cpu_id)
 *
 * 重要：必须在执行任何 adrp 访问高 VA 全局变量之前完成 MMU + 跳到高 VA，
 * 否则 PC-相对寻址会落到低物理地址，破坏全局数据。
 */
.global _secondary_start
.type _secondary_start, @function
_secondary_start:
    /* 用 callee-saved 寄存器保存 cpu_id，跨 mmu_init 等调用保持不变 */
    mov     x19, x0                  /* x19 = cpu_id */

    /* ── EL2/VHE 初始化（与 _start 完全一致）────────────── */
    init_el2_vhe

    msr     daifset, #2              /* 关中断 */

    /*
     * 设置启动栈：secondary_boot_stacks 是按 4KB 对齐的 BSS 数组，
     * 每核独占 4KB；栈顶 = base + (cpu_id + 1) * 4096
     * 此时 MMU 未启，使用 link.ld 导出的物理别名。
     */
    ldr     x0, =secondary_boot_stacks_phys
    add     x1, x19, #1
    lsl     x1, x1, #12              /* (cpu_id+1) * 4096 */
    add     x0, x0, x1
    mov     sp, x0

    /*
     * 启用 MMU：复用 BSP 已建好的内核 PGD。
     *
     * 关键：与 BSP（boot.S:120）一致，TTBR0 也设为 kernel_pt0 而非 0。
     * 原因：kernel_pt0 的 L0[0] 指向 kernel_pt1，kernel_pt1 用 1GB 块
     * 直接映射 0x0/0x4000_0000/0x8000_0000，正好可当低半区恒等映射用。
     * 这样 mmu_init 之后到 trampoline 跳到高 VA 之前，PC 仍在低 PA 也
     * 能继续取指；若 TTBR0=0 会导致开 MMU 后立即 translation fault。
     */
    bl      vm_get_kernel_pgtable
    mov     x1, x0                   /* x1 = TTBR1 = kernel_pt0 */
    /* x0 已是 kernel_pt0，复用为 TTBR0（低半区恒等映射） */

    bl      mmu_init

    /*
     * Trampoline 跳到高 VA（同 _start 套路）。
     */
    ldr     x0, =1f
    br      x0
1:
    /* ── 以下代码在高虚拟地址下执行 ───────────────────── */

    /* 重新设置栈为高 VA（同一物理内存的别名）*/
    ldr     x0, =secondary_boot_stacks
    add     x1, x19, #1
    lsl     x1, x1, #12
    add     x0, x0, x1
    mov     sp, x0

    /* 安装异常向量（与 BSP 同一份） */
    adrp    x0, exception_vector_base
    add     x0, x0, :lo12:exception_vector_base
    msr     vbar_el1, x0
    dsb     sy
    isb

    /*
     * 调用 cpu_secondary_bootstrap(cpu_id)
     * 注意：中断仍处于关闭状态（DAIF.I=1）；由 C 入口在合适时机打开。
     */
    mov     x0, x19

    adrp    x1, cpu_secondary_bootstrap
    add     x1, x1, :lo12:cpu_secondary_bootstrap
    blr     x1

    /* 不应返回；保险 hang */
.Lsec_hang:
    wfe
    b       .Lsec_hang

.size _secondary_start, . - _secondary_start

/*
 * BSP 启动栈（32KB，与 riscv64/x86_64 boot.S 一致放在汇编 .bss 中）
 *
 * 注意：link.ld 将整个 .bss 段放在 [__kernel_start, __kernel_end] 区间内，
 * 因此 PMM 不会把这段当作可用物理内存。
 */
.section .bss
.align 16
.global stack_bottom
.global stack_top

stack_bottom:
    .skip 0x8000              /* 32KB */
stack_top:

/*
 * 次级核启动栈
 *
 * 每核 4KB，索引 [0..AVATAR_MAX_CPUS-1]；CPU0（BSP）不使用 [0]，
 * 但保留以简化下标计算（用 (cpu_id+1)*4096 取栈顶）。
 * 仅在 cpu_secondary_bootstrap() 早期阶段使用；进入 idle 后栈基本闲置。
 *
 * 这里固定按 AVATAR_MAX_CPUS=8 预留；若调高最大核数需同步修改。
 */
.align 12
.global secondary_boot_stacks
secondary_boot_stacks:
    .skip 4096 * 8
