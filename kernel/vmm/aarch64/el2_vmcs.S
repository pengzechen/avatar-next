/*
 * kernel/vmm/aarch64/el2_vmcs.S — AArch64 VHE "VMCS" 等价汇编模块
 *
 * 与 ref/bare-vm/arch/aarch64/el2_vmcs.S 的对应关系：
 *   el2_enter_guest(vcpu_t *vcpu)  ←→  el2_enter_guest(&failure)
 *   el2_trap_exit                  ←→  el2_trap_exit（异常向量调用）
 *
 * VHE 相对于 bare-EL2 的关键差异：
 *   1. HCR_EL2 在进入 guest 前：clear TGE(27)，set VM(0)
 *      退出后恢复：set TGE，clear VM
 *   2. msr elr_el2 / msr spsr_el2 直接使用（无别名问题）
 *   3. vcpu 指针存在 tpidr_el2，供 el2_trap_exit 恢复
 *   4. guest trap 向量在进入前安装（guest_vec_table），退出后恢复 host VBAR
 *
 * vcpu_t 内存布局（与 include/vmm.h 中的 VCPU_* 宏一致）：
 *   Offset   0 : r[0..30]    31×8 B    x0-x30 guest GPRs
 *   Offset 248 : sp_el1      8 B       guest SP_EL1
 *   Offset 256 : elr         8 B       guest PC
 *   Offset 264 : spsr        8 B       guest PSTATE
 *   Offset 272 : host_ctx[13] 104 B   x19-x28, x29(fp), x30(lr), sp
 *
 * 控制流：
 *   C → el2_enter_guest(vcpu)
 *     → 保存 host 上下文到 vcpu->host_ctx
 *     → 切换 HCR_EL2 到 guest 模式（TGE=0, VM=1）
 *     → 安装 guest_vec_table 到 VBAR_EL2（via vbar_el1 VHE 别名）
 *     → 加载 guest 状态（elr_el2, spsr_el2, sp_el1, x0-x30）
 *     → eret → guest EL1
 *   guest EL1 陷入 EL2 → guest_vec_table → el2_trap_exit
 *     → 保存 guest x0-x30 / elr_el2 / spsr_el2 / sp_el1 到 vcpu
 *     → 恢复 HCR_EL2（TGE=1, VM=0）
 *     → 恢复 host VBAR（从 g_host_vbar 读取）
 *     → 恢复 host 上下文（x19-x30, sp）从 vcpu->host_ctx
 *     → ret（返回给 el2_enter_guest 的调用者）
 */

.arch armv8.2-a

/* ─────────────────────────────────────────────────────────────────────
 * el2_enter_guest — 进入 EL1 guest（VHE 版）
 *
 * 签名：void el2_enter_guest(vcpu_t *vcpu)
 *   x0 = vcpu 指针
 *
 * 不返回值（void）；当 guest 陷入时由 el2_trap_exit ret 返回给调用者。
 * ───────────────────────────────────────────────────────────────────── */
.section .text
.global el2_enter_guest
.type el2_enter_guest, @function
el2_enter_guest:
    /*
     * 1. 将 vcpu 指针存入 tpidr_el2
     *    el2_trap_exit 通过 mrs x0, tpidr_el2 取回指针。
     *    tpidr_el2 在 VHE 中不被 E2H 别名（tpidr_el1→tpidr_el2 仅来自 EL1 访问）。
     */
    msr  tpidr_el2, x0

    /*
     * 2. 保存 host callee-saved 寄存器 + SP + LR 到 vcpu->host_ctx (offset 272)
     */
    add  x1, x0, #272          /* x1 = &vcpu->host_ctx */
    stp  x19, x20, [x1, #0]
    stp  x21, x22, [x1, #16]
    stp  x23, x24, [x1, #32]
    stp  x25, x26, [x1, #48]
    stp  x27, x28, [x1, #64]
    stp  x29, x30, [x1, #80]   /* fp (x29), lr (x30) */
    mov  x2,  sp
    str  x2,       [x1, #96]   /* sp */

    /*
     * 3. 加载 guest 系统寄存器（先于 GPR，因为要用 x0 做基址）
     *    sp_el1 / elr_el2 / spsr_el2 直接使用对应 EL2 寄存器——
     *    VHE 不对这些寄存器名做别名。
     */
    ldr  x2, [x0, #248]        /* vcpu->sp_el1   */
    msr  sp_el1,  x2
    ldr  x2, [x0, #256]        /* vcpu->elr      */
    msr  elr_el2, x2
    ldr  x2, [x0, #264]        /* vcpu->spsr     */
    msr  spsr_el2, x2
    isb

    /*
     * 4. 保存 host VBAR_EL2 到 vcpu->host_vbar（偏移 VCPU_HOST_VBAR=528）
     *    SMP 下每个 vCPU 独立保存，el2_trap_exit 通过 tpidr_el2 取 vcpu 读取，
     *    避免全局 g_host_vbar 的 SMP 竞争问题。
     */
    mrs  x2, vbar_el1
    str  x2, [x0, #528]         /* vcpu->host_vbar */

    /*
     * 5. 修改 HCR_EL2 进入 guest 模式（VHE 核心差异）：
     *    - 清除 TGE (bit 27)：允许 EL1 成为真正的 guest EL1
     *      （不清除 TGE 时 EL1 被视为 host EL1，EL0 异常直接路由到 EL2）
     *    - 置位 VM  (bit  0)：启用 Stage-2 MMU（IPA→PA 转换）
     *    - 置位 TWI (bit 13)：陷入 WFI（用于 VMM 协同调度）
     *    - 保留 E2H (bit 34) / RW (bit 31) 不变
     */
    mrs  x2, hcr_el2
    bic  x2, x2, #(1 << 27)    /* clear TGE  */
    orr  x2, x2, #(1 << 0)     /* set   VM   */
    orr  x2, x2, #(1 << 13)    /* set   TWI  */
    msr  hcr_el2, x2
    isb

    /*
     * 6. 安装 guest trap 向量表到 VBAR_EL2
     *    在 VHE（E2H=1）下，`msr vbar_el1` 写入 VBAR_EL2。
     *    guest EL1 → EL2 陷入时跳转到 guest_vec_table 中对应的 entry，
     *    所有 entry 均 b el2_trap_exit。
     */
    adrp x2, guest_vec_table
    add  x2, x2, :lo12:guest_vec_table
    msr  vbar_el1, x2
    isb

    /*
     * 7. 加载 guest GPR x1-x30（x0 暂时保持 vcpu 基址）
     */
    ldr  x1,       [x0, #8]
    ldp  x2,  x3,  [x0, #16]
    ldp  x4,  x5,  [x0, #32]
    ldp  x6,  x7,  [x0, #48]
    ldp  x8,  x9,  [x0, #64]
    ldp  x10, x11, [x0, #80]
    ldp  x12, x13, [x0, #96]
    ldp  x14, x15, [x0, #112]
    ldp  x16, x17, [x0, #128]
    ldp  x18, x19, [x0, #144]
    ldp  x20, x21, [x0, #160]
    ldp  x22, x23, [x0, #176]
    ldp  x24, x25, [x0, #192]
    ldp  x26, x27, [x0, #208]
    ldp  x28, x29, [x0, #224]
    ldr  x30,      [x0, #240]
    ldr  x0,       [x0, #0]    /* guest x0 最后加载（覆盖 vcpu 指针）*/

    /* 8. eret → EL1 guest（SPSR_EL2.M=EL1h=5，TGE=0，VM=1）*/
    eret
    /* 不再执行，guest 陷入后由 el2_trap_exit 恢复 host 并 ret。 */


/* ─────────────────────────────────────────────────────────────────────
 * el2_trap_exit — guest EL1 陷入 EL2 后由 guest_vec_table entry 调用
 *
 * 入口条件（由 CPU 保证）：
 *   x0-x30 = guest 陷入时的通用寄存器值
 *   ELR_EL2  = guest 陷入时的 PC
 *   SPSR_EL2 = guest 陷入时的 PSTATE
 *   SP_EL1   = guest EL1 栈指针（EL2 栈 SP_EL2 仍是 host 栈，完整）
 *   SP_EL2   = host 内核栈（el2_enter_guest 调用时的 host SP）
 *
 * 功能：
 *   1. 保存 guest 状态 → vcpu->r[] / vcpu->elr / vcpu->spsr / vcpu->sp_el1
 *   2. 恢复 HCR_EL2 到 host 模式（TGE=1, VM=0, TWI=0）
 *   3. 恢复 host VBAR_EL2（从 g_host_vbar）
 *   4. 恢复 host 上下文（x19-x30, sp）从 vcpu->host_ctx
 *   5. ret（返回给 el2_enter_guest 的 C 调用者）
 * ───────────────────────────────────────────────────────────────────── */
.global el2_trap_exit
.type el2_trap_exit, @function
el2_trap_exit:
    /* 暂存 guest x0, x1 到 EL2 栈（SP_EL2 有效，host 栈）*/
    sub  sp, sp, #16
    stp  x0, x1, [sp, #0]

    /* 从 tpidr_el2 取回 vcpu 指针 */
    mrs  x0, tpidr_el2

    /* 保存 guest x2-x30 */
    stp  x2,  x3,  [x0, #16]
    stp  x4,  x5,  [x0, #32]
    stp  x6,  x7,  [x0, #48]
    stp  x8,  x9,  [x0, #64]
    stp  x10, x11, [x0, #80]
    stp  x12, x13, [x0, #96]
    stp  x14, x15, [x0, #112]
    stp  x16, x17, [x0, #128]
    stp  x18, x19, [x0, #144]
    stp  x20, x21, [x0, #160]
    stp  x22, x23, [x0, #176]
    stp  x24, x25, [x0, #192]
    stp  x26, x27, [x0, #208]
    stp  x28, x29, [x0, #224]
    str  x30,      [x0, #240]

    /* 从栈恢复 guest x0, x1 并保存到 vcpu */
    ldp  x1, x2, [sp, #0]
    add  sp, sp, #16
    str  x1, [x0, #0]          /* vcpu->r[0] = guest x0 */
    str  x2, [x0, #8]          /* vcpu->r[1] = guest x1 */

    /* 保存 guest sp_el1, elr_el2, spsr_el2 */
    mrs  x1, sp_el1
    str  x1, [x0, #248]        /* vcpu->sp_el1 */
    mrs  x1, elr_el2
    str  x1, [x0, #256]        /* vcpu->elr    */
    mrs  x1, spsr_el2
    str  x1, [x0, #264]        /* vcpu->spsr   */

    /*
     * 恢复 HCR_EL2 到 host 模式（VHE 核心差异）：
     *   - 置位 TGE (bit 27)：恢复 host EL0 模式
     *   - 清除 VM  (bit  0)：禁用 Stage-2 MMU
     *   - 清除 TWI (bit 13)
     */
    mrs  x1, hcr_el2
    orr  x1, x1, #(1 << 27)    /* set   TGE  */
    bic  x1, x1, #(1 << 0)     /* clear VM   */
    bic  x1, x1, #(1 << 13)    /* clear TWI  */
    msr  hcr_el2, x1
    isb

    /* 恢复 host VBAR_EL2（通过 vbar_el1 VHE 别名写入 VBAR_EL2）*/
    ldr  x1, [x0, #528]         /* vcpu->host_vbar */
    msr  vbar_el1, x1
    isb

    /* 从 vcpu->host_ctx (offset 272) 恢复 host x19-x30 + sp */
    add  x1, x0, #272
    ldp  x19, x20, [x1, #0]
    ldp  x21, x22, [x1, #16]
    ldp  x23, x24, [x1, #32]
    ldp  x25, x26, [x1, #48]
    ldp  x27, x28, [x1, #64]
    ldp  x29, x30, [x1, #80]   /* fp, lr */
    ldr  x2,       [x1, #96]
    mov  sp, x2

    /* ret → el2_enter_guest 的 C 调用者（vmm_run_vcpu 的主循环）*/
    ret
