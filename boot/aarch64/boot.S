
#include "asm.h"

.section .text.boot

.extern vm_init
.extern mmu_init
.extern kernel_main

/*
 * ── EL2/VHE 初始化 ───────────────────────────────────────────────
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
     * 打开 CPACR.FPEN（[21:20] = 0b11）—— EL1/EL0 执行 FP/SIMD 不再陷入。
     *
     * 必须在下面的 `msr vbar_el1` 之前完成：SAVE_REGS 无条件保存 q0-q31，
     * 而 FPEN 的复位值是 0b00（陷入）。若向量表先生效，第一次异常的
     * 第一条 `stp q` 就会陷入**同一个向量** → 再执行 SAVE_REGS → 无限
     * 递归（每轮 sub sp, #816），表现为挂死而不是可诊断的 panic。
     *
     * VHE（E2H=1）下 EL2 访问 cpacr_el1 落在 CPACR_EL2，正是同时门控
     * EL1 与 EL0 的那一份。kernel_main() 里的 aarch64_enable_neon() 保留
     * （幂等），AP 走同一份 init_el2_vhe。
     */
    mrs     x1, cpacr_el1
    orr     x1, x1, #(3 << 20)
    msr     cpacr_el1, x1
    isb

    /*
     * 开放 EL0/EL1 定时器访问（CNTHCTL_EL2）：
     *   bit 0  EL0PCTEN = 1  不陷入 EL0 的 CNTPCT 读取
     *   bit 1  EL1PCEN  = 1  不陷入 EL1 的**物理定时器**访问
     *                        （CNTP_CTL / CNTP_CVAL / CNTP_TVAL）
     *   bit 10 EL1PTEN  = 1  不陷入 EL1 的**物理计数器**读取（CNTPCT）
     */
    mov     x1, #(1 | (1 << 1) | (1 << 10))
    msr     cnthctl_el2, x1
    isb

    msr     cntvoff_el2, xzr
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

FUNCS(_start)
    init_el2_vhe

    msr daifset, #2   // 关闭 IRQ

    ldr x0, =stack_top_phys  // 加载物理栈顶地址
    mov sp, x0               // 设置栈指针

    bl      clear_bss

    bl      vm_init

    bl      vm_get_boot_pgtable      // x0 = boot_pt0 物理地址（TTBR0 恒等映射）
    mov     x27, x0                  // 保存到 callee-saved 寄存器
    bl      vm_get_kernel_pgtable    // x0 = kernel_pt0 物理地址（TTBR1）
    mov     x1, x0                   // x1 = TTBR1 = kernel_pt0
    mov     x0, x27                  // x0 = TTBR0 = boot_pt0
    bl      mmu_init

    ldr     x0, =1f
    br      x0
1:
    /* ── 以下代码在高虚拟地址（TTBR1 映射）下执行 ────────────── */

    ldr     x0, =stack_top
    mov     sp, x0

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

FUNCE(_start)

FUNCS(_secondary_start)
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

FUNCE(_secondary_start)

/*
 * BSS 清零：按 8 字节把 [__bss_start_phys, __bss_end_phys) 写 0。
 *
 * 用链接器导出的物理别名，避免假设 PA 位于 VA 低 32 位。
 * 必须在 MMU 打开前调用（此时 PC = PA）；本文件内局部子程序，不导出符号。
 */
LFUNCS(clear_bss)
    ldr     x0, =__bss_start_phys
    ldr     x1, =__bss_end_phys
    mov     x2, #0
bss_clear_loop:
    cmp     x0, x1
    b.ge    bss_clear_done
    str     x2, [x0], #8
    b       bss_clear_loop
bss_clear_done:
    ret
LFUNCE(clear_bss)

.section .bss
.align 16
.global stack_bottom
.global stack_top

stack_bottom:
    .skip 0x8000              /* 32KB */
stack_top:

.align 12
.global secondary_boot_stacks
secondary_boot_stacks:
    .skip 0x8000              /* 32KB */
