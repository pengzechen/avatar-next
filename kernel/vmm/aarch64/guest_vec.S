/*
 * kernel/vmm/aarch64/guest_vec.S — Guest trap 向量表
 *
 * 当 vCPU 运行时（el2_enter_guest 安装此表到 VBAR_EL2），
 * guest EL1 发生的所有陷入 / 中断 / 异常均跳转到 el2_trap_exit。
 *
 * AArch64 向量表格式（ARM DDI 0487, D1.10.2）：
 *   表基地址对齐到 2KB（11 bit）
 *   16 个 entry，每个 128 字节（.align 7），分 4 组：
 *     0x000: Current EL, SP_EL0（SP0 模式，本内核不使用）
 *     0x200: Current EL, SP_ELx（SPx 模式，EL2 自身异常）
 *     0x400: Lower EL, AArch64（EL1/EL0 → EL2，guest 陷入在此）
 *     0x600: Lower EL, AArch32（AArch32 EL1 → EL2，暂不支持）
 *
 * 所有 entry 均直接跳转到 el2_trap_exit：
 *   - guest EL1 陷入（0x400 组）：正常 VM exit 处理
 *   - 其余 entry：理论上不应发生（对于 Phase 2 单核 cooperative VMM）
 */

.arch armv8.2-a

/* ── 每个向量 entry 的宏：.align 7 确保 128-byte 对齐 ── */
.macro GUEST_ENTRY
    .align 7
    b  el2_trap_exit
.endm

/* ── guest_vec_table：2KB 对齐的向量表 ─────────────────── */
.section .text.guest_vec
.align 11                       /* 2048-byte aligned */
.global guest_vec_table
guest_vec_table:

    /* 0x000: Current EL + SP_EL0 — Sync / IRQ / FIQ / SError */
    GUEST_ENTRY
    GUEST_ENTRY
    GUEST_ENTRY
    GUEST_ENTRY

    /* 0x200: Current EL + SP_ELx — Sync / IRQ / FIQ / SError */
    GUEST_ENTRY
    GUEST_ENTRY
    GUEST_ENTRY
    GUEST_ENTRY

    /* 0x400: Lower EL AArch64 — Sync / IRQ / FIQ / SError     ← guest EL1→EL2 */
    GUEST_ENTRY
    GUEST_ENTRY
    GUEST_ENTRY
    GUEST_ENTRY

    /* 0x600: Lower EL AArch32 — Sync / IRQ / FIQ / SError */
    GUEST_ENTRY
    GUEST_ENTRY
    GUEST_ENTRY
    GUEST_ENTRY
