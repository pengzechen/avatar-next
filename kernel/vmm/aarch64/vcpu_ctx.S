/*
 * kernel/vmm/aarch64/vcpu_ctx.S — guest EL1 系统寄存器保存/恢复（VHE 版）
 *
 * VHE（E2H=1）差异：
 *   在 EL2 访问 EL1 系统寄存器时，若不加 _el12 后缀，
 *   sctlr_el1 → SCTLR_EL2（host），ttbr0_el1 → TTBR0_EL2，……
 *   必须使用 _el12 后缀才能访问真实的 EL1 寄存器（guest 的）。
 *
 * buf 布局（每对 16 B，共 8 对 = 128 B）：
 *   Offset  0: sctlr_el12, ttbr0_el12
 *   Offset 16: ttbr1_el12, tcr_el12
 *   Offset 32: mair_el12,  amair_el12
 *   Offset 48: vbar_el12,  cpacr_el12
 *   Offset 64: tpidr_el1,  tpidr_el0   (非别名，直接访问)
 *   Offset 80: tpidrro_el0, sp_el0
 *   Offset 96: elr_el12,   spsr_el12   (guest EL1 内嵌异常用)
 *   Offset 112: (pad)
 *   Total: 128 B = VCPU_SYSREGS_SIZE
 *
 * 与 ref/bare-vm/arch/aarch64/vcpu_ctx.S 的差异：
 *   所有 _el1 寄存器名改为 _el12（访问真实 EL1 而非 EL2 别名）。
 *
 * 注：set_stage2_pgd 同时在本文件实现（原 vcpu_ctx.S 中的 set_stage2_pgd）。
 */

.arch armv8.2-a

/* ─────────────────────────────────────────────────────────────────────
 * save_sysregs_el12 — 保存 guest EL1 系统寄存器到 buf
 * @x0 = buf 指针（至少 128 B）
 * ───────────────────────────────────────────────────────────────────── */
.global save_sysregs_el12
.type save_sysregs_el12, @function
save_sysregs_el12:
    dsb ish
    isb

    /* [0]: sctlr_el12, ttbr0_el12 */
    mrs  x1, sctlr_el12
    mrs  x2, ttbr0_el12
    stp  x1, x2, [x0], #16

    /* [16]: ttbr1_el12, tcr_el12 */
    mrs  x1, ttbr1_el12
    mrs  x2, tcr_el12
    stp  x1, x2, [x0], #16

    /* [32]: mair_el12, amair_el12 */
    mrs  x1, mair_el12
    mrs  x2, amair_el12
    stp  x1, x2, [x0], #16

    /* [48]: vbar_el12, cpacr_el12 */
    mrs  x1, vbar_el12
    mrs  x2, cpacr_el12
    stp  x1, x2, [x0], #16

    /* [64]: tpidr_el1, tpidr_el0（不被 E2H 别名，直接访问）*/
    mrs  x1, tpidr_el1
    mrs  x2, tpidr_el0
    stp  x1, x2, [x0], #16

    /* [80]: tpidrro_el0, sp_el0 */
    mrs  x1, tpidrro_el0
    mrs  x2, sp_el0
    stp  x1, x2, [x0], #16

    /* [96]: elr_el12, spsr_el12（guest EL1 嵌套异常的保存寄存器）*/
    mrs  x1, elr_el12
    mrs  x2, spsr_el12
    stp  x1, x2, [x0], #16

    /* [112]: 补齐 128B（写 0 占位）*/
    stp  xzr, xzr, [x0], #16

    ret


/* ─────────────────────────────────────────────────────────────────────
 * restore_sysregs_el12 — 从 buf 恢复 guest EL1 系统寄存器
 * @x0 = buf 指针（至少 128 B）
 * ───────────────────────────────────────────────────────────────────── */
.global restore_sysregs_el12
.type restore_sysregs_el12, @function
restore_sysregs_el12:
    /* [0]: sctlr_el12, ttbr0_el12 */
    ldp  x1, x2, [x0], #16
    msr  sctlr_el12, x1
    msr  ttbr0_el12, x2

    /* [16]: ttbr1_el12, tcr_el12 */
    ldp  x1, x2, [x0], #16
    msr  ttbr1_el12, x1
    msr  tcr_el12, x2

    /* [32]: mair_el12, amair_el12 */
    ldp  x1, x2, [x0], #16
    msr  mair_el12, x1
    msr  amair_el12, x2

    /* [48]: vbar_el12, cpacr_el12 */
    ldp  x1, x2, [x0], #16
    msr  vbar_el12, x1
    msr  cpacr_el12, x2

    /* [64]: tpidr_el1, tpidr_el0 */
    ldp  x1, x2, [x0], #16
    msr  tpidr_el1, x1
    msr  tpidr_el0, x2

    /* [80]: tpidrro_el0, sp_el0 */
    ldp  x1, x2, [x0], #16
    msr  tpidrro_el0, x1
    msr  sp_el0, x2

    /* [96]: elr_el12, spsr_el12 */
    ldp  x1, x2, [x0], #16
    msr  elr_el12, x1
    msr  spsr_el12, x2

    /* [112]: 跳过填充 */
    add  x0, x0, #16

    dsb ish
    isb
    ret


/* ─────────────────────────────────────────────────────────────────────
 * set_stage2_pgd — 写入 VTTBR_EL2（Stage-2 根页表 + VMID）
 * @x0 = 根页表物理地址
 * @x1 = VMID (8-bit)
 * ───────────────────────────────────────────────────────────────────── */
.global set_stage2_pgd
.type set_stage2_pgd, @function
set_stage2_pgd:
    and  x1, x1, #0xff
    lsl  x1, x1, #48           /* VMID 放入 bits[55:48] */
    orr  x0, x0, x1
    msr  vttbr_el2, x0
    dsb  ish
    isb
    ret
