/*
 * kernel/vmm/riscv64/hext_vcpu.S — RISC-V H-extension VM 入口/出口汇编
 *
 * 提供两个符号：
 *
 * 1) hext_enter_guest(vcpu_t *a0) → int
 *    进入 VS-mode guest 执行一条路径，直到陷入 HS-mode。
 *    返回 1（a0=1）= 正常从 guest exit 返回。
 *
 * 2) hext_trap_vector
 *    guest 运行期间专属陷阱向量（Direct 模式，.align 2）。
 *    从 sscratch 原子取回 vcpu*，保存 guest GPR + VS-CSRs，
 *    恢复 host callee-saved 寄存器，ret 返回给调用者（a0=1）。
 *
 * vcpu_t 固定偏移（与 include/vmm.h VCPU_RV_* 宏完全一致）：
 *   VCPU_R0       =   0  (x0-x31, 32×8B = 256B)
 *   VCPU_VSEPC    = 256
 *   VCPU_VSSTATUS = 264
 *   VCPU_VSTVEC   = 272
 *   VCPU_VSSCRATCH= 280
 *   VCPU_VSATP    = 288
 *   VCPU_VSIE     = 296
 *   VCPU_SCAUSE   = 304
 *   VCPU_STVAL    = 312
 *   VCPU_HTVAL    = 320
 *   VCPU_HOSTCTX  = 328  (uint64_t host_ctx[15])
 *     [0]ra  [8]s0 ... [96]s11 [104]sp [112]orig_stvec
 *
 * sscratch 约定：
 *   进入 guest 前  = vcpu*（陷阱时找到 vcpu）
 *   hext_trap_vector 入口序列：
 *     csrrw a0,sscratch,a0 → a0=vcpu*, sscratch=guest_a0
 *     csrr  t0, sscratch   → t0=guest_a0（立即读，在清零前）
 *     csrw  sscratch, x0   → sscratch=0（恢复内核态约定）
 *     sd    t0, r[10](a0)  → 保存 guest a0
 */

/* vcpu_t 字段偏移 */
#define VCPU_R0         0
#define VCPU_VSEPC      256
#define VCPU_VSSTATUS   264
#define VCPU_VSTVEC     272
#define VCPU_VSSCRATCH  280
#define VCPU_VSATP      288
#define VCPU_VSIE       296
#define VCPU_SCAUSE     304
#define VCPU_STVAL      312
#define VCPU_HTVAL      320

/* host_ctx 字段偏移（VCPU_HOSTCTX = 328，host_ctx[0..14]）*/

/* H-extension CSR 数值地址（-march=rv64gc 汇编器不识别符号名） */
#define CSR_HSTATUS     0x600
#define CSR_VSSTATUS    0x200
#define CSR_VSIE        0x204
#define CSR_VSTVEC      0x205
#define CSR_VSSCRATCH   0x240
#define CSR_VSEPC       0x241
#define CSR_VSATP       0x280
#define CSR_HTVAL       0x643
#define HCTX_RA         328
#define HCTX_S0         336
#define HCTX_S1         344
#define HCTX_S2         352
#define HCTX_S3         360
#define HCTX_S4         368
#define HCTX_S5         376
#define HCTX_S6         384
#define HCTX_S7         392
#define HCTX_S8         400
#define HCTX_S9         408
#define HCTX_S10        416
#define HCTX_S11        424
#define HCTX_SP         432
#define HCTX_STVEC      440

.section .text
.align 2

.extern __global_pointer$

/* ──────────────────────────────────────────────────────────────
 * hext_enter_guest(vcpu_t *a0) → int
 * ────────────────────────────────────────────────────────────── */
.global hext_enter_guest
.type hext_enter_guest, @function
hext_enter_guest:
    /* 1. 保存 host callee-saved + sp + ra 到 vcpu->host_ctx */
    sd      ra,  HCTX_RA(a0)
    sd      s0,  HCTX_S0(a0)
    sd      s1,  HCTX_S1(a0)
    sd      s2,  HCTX_S2(a0)
    sd      s3,  HCTX_S3(a0)
    sd      s4,  HCTX_S4(a0)
    sd      s5,  HCTX_S5(a0)
    sd      s6,  HCTX_S6(a0)
    sd      s7,  HCTX_S7(a0)
    sd      s8,  HCTX_S8(a0)
    sd      s9,  HCTX_S9(a0)
    sd      s10, HCTX_S10(a0)
    sd      s11, HCTX_S11(a0)
    sd      sp,  HCTX_SP(a0)

    /* 2. 保存当前 stvec（主 trap 向量），退出时还原 */
    csrr    t0, stvec
    sd      t0, HCTX_STVEC(a0)

    /* 3. 恢复 VS-CSRs */
    ld      t0, VCPU_VSEPC(a0)
    csrw    CSR_VSEPC, t0

    ld      t0, VCPU_VSSTATUS(a0)
    csrw    CSR_VSSTATUS, t0

    ld      t0, VCPU_VSTVEC(a0)
    csrw    CSR_VSTVEC, t0

    ld      t0, VCPU_VSSCRATCH(a0)
    csrw    CSR_VSSCRATCH, t0

    ld      t0, VCPU_VSATP(a0)
    csrw    CSR_VSATP, t0

    ld      t0, VCPU_VSIE(a0)
    csrw    CSR_VSIE, t0

    /* 4. 安装 VMM 专属陷阱向量（Direct 模式） */
    la      t0, hext_trap_vector
    csrw    stvec, t0

    /* 5. 将 vcpu* 存入 sscratch（guest 陷入时取回） */
    csrw    sscratch, a0

    /* 6. 设置 sepc = vcpu->vsepc（sret 的跳转目标） */
    ld      t0, VCPU_VSEPC(a0)
    csrw    sepc, t0

    /* 7. 设置 sstatus.SPP=1（sret 返回 S-mode 即 VS-mode），SPIE=0 */
    csrr    t0, sstatus
    ori     t0, t0, 0x100       /* SPP = bit8 = 1 */
    li      t1, ~0x20
    and     t0, t0, t1          /* SPIE = bit5 = 0 */
    csrw    sstatus, t0

    /* 8. 设置 hstatus.SPV=1 + SPVP=1（sret 进入 VS-mode） */
    csrr    t0, CSR_HSTATUS
    ori     t0, t0, 0x180       /* SPV=bit7=1, SPVP=bit8=1 */
    csrw    CSR_HSTATUS, t0

    /* 9. 加载 guest GPR（a0 = x10 最后加载） */
    ld      x1,  VCPU_R0 +  1*8(a0)
    ld      x2,  VCPU_R0 +  2*8(a0)
    ld      x3,  VCPU_R0 +  3*8(a0)
    ld      x4,  VCPU_R0 +  4*8(a0)
    ld      x5,  VCPU_R0 +  5*8(a0)
    ld      x6,  VCPU_R0 +  6*8(a0)
    ld      x7,  VCPU_R0 +  7*8(a0)
    ld      x8,  VCPU_R0 +  8*8(a0)
    ld      x9,  VCPU_R0 +  9*8(a0)
    ld      x11, VCPU_R0 + 11*8(a0)
    ld      x12, VCPU_R0 + 12*8(a0)
    ld      x13, VCPU_R0 + 13*8(a0)
    ld      x14, VCPU_R0 + 14*8(a0)
    ld      x15, VCPU_R0 + 15*8(a0)
    ld      x16, VCPU_R0 + 16*8(a0)
    ld      x17, VCPU_R0 + 17*8(a0)
    ld      x18, VCPU_R0 + 18*8(a0)
    ld      x19, VCPU_R0 + 19*8(a0)
    ld      x20, VCPU_R0 + 20*8(a0)
    ld      x21, VCPU_R0 + 21*8(a0)
    ld      x22, VCPU_R0 + 22*8(a0)
    ld      x23, VCPU_R0 + 23*8(a0)
    ld      x24, VCPU_R0 + 24*8(a0)
    ld      x25, VCPU_R0 + 25*8(a0)
    ld      x26, VCPU_R0 + 26*8(a0)
    ld      x27, VCPU_R0 + 27*8(a0)
    ld      x28, VCPU_R0 + 28*8(a0)
    ld      x29, VCPU_R0 + 29*8(a0)
    ld      x30, VCPU_R0 + 30*8(a0)
    ld      x31, VCPU_R0 + 31*8(a0)
    ld      x10, VCPU_R0 + 10*8(a0)   /* a0 最后，此后 vcpu* 不可用 */

    /* 10. sret → VS-mode */
    sret

/* ──────────────────────────────────────────────────────────────
 * hext_trap_vector — guest 运行期间专属陷阱向量（HS-mode 入口）
 *
 * 关键：sscratch 管理的正确顺序
 *   csrrw a0, sscratch, a0  → a0=vcpu*, sscratch=guest_a0
 *   csrr  t0, sscratch      → t0=guest_a0（在清零前读！）
 *   csrw  sscratch, x0      → sscratch=0
 *   sd    t0, r[10](a0)     → 保存 guest a0
 * ────────────────────────────────────────────────────────────── */
.global hext_trap_vector
.type hext_trap_vector, @function
.align 2
hext_trap_vector:
    /* 原子取 vcpu*，sscratch 变为 guest_a0 */
    csrrw   a0, sscratch, a0

    /* 立即读出 guest_a0（sscratch 仍是 guest_a0）*/
    csrr    t0, sscratch

    /* 清零 sscratch（内核态 = 0 约定）*/
    csrw    sscratch, x0

    /* 1. 保存 guest GPR → vcpu->r[] */
    sd      x1,  VCPU_R0 +  1*8(a0)
    sd      x2,  VCPU_R0 +  2*8(a0)
    sd      x3,  VCPU_R0 +  3*8(a0)
    sd      x4,  VCPU_R0 +  4*8(a0)
    sd      x5,  VCPU_R0 +  5*8(a0)
    sd      x6,  VCPU_R0 +  6*8(a0)
    sd      x7,  VCPU_R0 +  7*8(a0)
    sd      x8,  VCPU_R0 +  8*8(a0)
    sd      x9,  VCPU_R0 +  9*8(a0)
    sd      t0,  VCPU_R0 + 10*8(a0)  /* guest a0（从 t0）*/
    sd      x11, VCPU_R0 + 11*8(a0)
    sd      x12, VCPU_R0 + 12*8(a0)
    sd      x13, VCPU_R0 + 13*8(a0)
    sd      x14, VCPU_R0 + 14*8(a0)
    sd      x15, VCPU_R0 + 15*8(a0)
    sd      x16, VCPU_R0 + 16*8(a0)
    sd      x17, VCPU_R0 + 17*8(a0)
    sd      x18, VCPU_R0 + 18*8(a0)
    sd      x19, VCPU_R0 + 19*8(a0)
    sd      x20, VCPU_R0 + 20*8(a0)
    sd      x21, VCPU_R0 + 21*8(a0)
    sd      x22, VCPU_R0 + 22*8(a0)
    sd      x23, VCPU_R0 + 23*8(a0)
    sd      x24, VCPU_R0 + 24*8(a0)
    sd      x25, VCPU_R0 + 25*8(a0)
    sd      x26, VCPU_R0 + 26*8(a0)
    sd      x27, VCPU_R0 + 27*8(a0)
    sd      x28, VCPU_R0 + 28*8(a0)
    sd      x29, VCPU_R0 + 29*8(a0)
    sd      x30, VCPU_R0 + 30*8(a0)
    sd      x31, VCPU_R0 + 31*8(a0)

    /* 2. 保存 VS-CSRs → vcpu */
    csrr    t0, CSR_VSEPC
    sd      t0, VCPU_VSEPC(a0)

    csrr    t0, CSR_VSSTATUS
    sd      t0, VCPU_VSSTATUS(a0)

    csrr    t0, CSR_VSTVEC
    sd      t0, VCPU_VSTVEC(a0)

    csrr    t0, CSR_VSSCRATCH
    sd      t0, VCPU_VSSCRATCH(a0)

    csrr    t0, CSR_VSATP
    sd      t0, VCPU_VSATP(a0)

    csrr    t0, CSR_VSIE
    sd      t0, VCPU_VSIE(a0)

    /* 3. 保存 HS 陷阱信息（sepc = guest PC 入口，供 exit handler 使用）*/
    csrr    t0, sepc
    sd      t0, VCPU_VSEPC(a0)   /* 覆盖 vsepc（sepc 是 HS 侧保存的 guest PC）*/

    csrr    t0, scause
    sd      t0, VCPU_SCAUSE(a0)

    csrr    t0, stval
    sd      t0, VCPU_STVAL(a0)

    csrr    t0, CSR_HTVAL
    sd      t0, VCPU_HTVAL(a0)

    /* 3b. 清 hstatus.SPV(bit7)+SPVP(bit8)
     *
     * 硬件在 VS-mode → HS-mode 陷入时设置 hstatus.SPV=1，且不会自动清零。
     * 若不清，host 端任何后续 sret（无论 sched_check_and_yield 切到哪条
     * 路径）都会被误判为"返回虚拟态"，造成 sret 进入 VS/VU-mode：
     *   - 调度回内核任务后 trap_vector 出口（SPP=1）→ 进入 VS-mode
     *   - 调度到用户进程时若漏清 → sret 进入 VU-mode → vsatp(=0) 翻译失败 → Inst PF @ 0x10000
     * 在此处一次性清零，使整个 host 处理路径回到普通 HS-mode 语义。
     */
    csrr    t0, CSR_HSTATUS
    li      t1, ~0x180
    and     t0, t0, t1
    csrw    CSR_HSTATUS, t0

    /* 4. 恢复原主 stvec */
    ld      t0, HCTX_STVEC(a0)
    csrw    stvec, t0

    /* 5. 恢复 host callee-saved + sp */
    ld      ra,  HCTX_RA(a0)
    ld      s0,  HCTX_S0(a0)
    ld      s1,  HCTX_S1(a0)
    ld      s2,  HCTX_S2(a0)
    ld      s3,  HCTX_S3(a0)
    ld      s4,  HCTX_S4(a0)
    ld      s5,  HCTX_S5(a0)
    ld      s6,  HCTX_S6(a0)
    ld      s7,  HCTX_S7(a0)
    ld      s8,  HCTX_S8(a0)
    ld      s9,  HCTX_S9(a0)
    ld      s10, HCTX_S10(a0)
    ld      s11, HCTX_S11(a0)
    ld      sp,  HCTX_SP(a0)

    /* 5b. 恢复内核 gp（x3）
     *
     * hext_enter_guest 在步骤 9 中加载了 guest 的 x3（vcpu->r[3]=0），
     * 导致 gp = 0。陷入 HS-mode 后 gp 仍为 guest 值，返回 C 代码前
     * 必须显式重置为内核的 __global_pointer$，否则 gp 相对寻址全部错误。
     *
     * .option norelax 禁止链接器把 la gp, __global_pointer$ 优化成
     * mv gp, gp（当 gp 已损坏时该优化是错误的）。
     */
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop

    /* 6. 返回 1（正常 guest exit）*/
    li      a0, 1
    ret
