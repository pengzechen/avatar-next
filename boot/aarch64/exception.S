/*
 * boot/aarch64/exception.S — AArch64 异常/中断入口
 *
 * trap_frame_t 布局（TRAP_FRAME_SIZE = 816 字节，定义见
 * include/aarch64/exception.h，两处必须一致）：
 *   0 .. 30*8   : x0..x30
 *   31*8 = 248  : usp (SP_EL0)
 *   32*8 = 256  : elr
 *   33*8 = 264  : spsr
 *   34*8 = 272  : tpidr_el0
 *   35*8 = 280  : fp_pad（填充，让 q[] 落在 16 字节边界）
 *   36*8 = 288  : q0..q31（每 16 字节一个，两条一组 stp）
 *   100*8 = 800 : fpcr
 *   101*8 = 808: fpsr
 *
 * 帧大小必须是 16 的倍数：硬件进入 EL1 时不动 SP，16 字节栈对齐是软件
 * 不变量（AAPCS64），stp q/ldp q 也要求 16 字节对齐。
 */

.equ TRAP_FRAME_SIZE, 102 * 8

.macro SAVE_REGS
    sub     sp, sp, TRAP_FRAME_SIZE
    stp     x0, x1,   [sp, 2 * 0]
    stp     x2, x3,   [sp, 2 * 8]
    stp     x4, x5,   [sp, 4 * 8]
    stp     x6, x7,   [sp, 6 * 8]
    stp     x8, x9,   [sp, 8 * 8]
    stp     x10, x11, [sp, 10 * 8]
    stp     x12, x13, [sp, 12 * 8]
    stp     x14, x15, [sp, 14 * 8]
    stp     x16, x17, [sp, 16 * 8]
    stp     x18, x19, [sp, 18 * 8]
    stp     x20, x21, [sp, 20 * 8]
    stp     x22, x23, [sp, 22 * 8]
    stp     x24, x25, [sp, 24 * 8]
    stp     x26, x27, [sp, 26 * 8]
    stp     x28, x29, [sp, 28 * 8]

    mrs     x9, sp_el0
    mrs     x10, elr_el1
    mrs     x11, spsr_el1
    stp     x30, x9,  [sp, 30 * 8]
    stp     x10, x11, [sp, 32 * 8]
    mrs     x9, tpidr_el0            /* 用户态 TLS 线程指针 */
    str     x9, [sp, #34 * 8]

    /*
     * ── FP/SIMD：无条件保存 q0-q31 + FPCR/FPSR ──────────────────────
     *
     * 不能按 spsr 条件跳过：内核态（EL1）陷入可能打断一段**没有函数调用**
     * 的内核代码，此时 v0-v7/v16-v31 仍是活跃值 —— 异步陷阱不是编译器
     * 意义上的 call，AAPCS64 的 caller-saved 规则保护不了它们。删掉
     * -mgeneral-regs-only 之后内核确实会生成 ldp q0,q1（结构体拷贝，
     * 见 kernel_syscall_fs_file_ops.o / proc_lifecycle.o），所以这一层
     * 保存是必需的。对照 boot/riscv64/exception.S:121-155。
     *
     * ⚠️ 必须 bl 到向量表**外面**的 fp_save_common，不能就地展开：
     *    向量表的 16 个表项各自钉死在 128 字节槽位上（硬件按
     *    VBAR + group*0x200 + slot*0x80 跳转）。这 34 条存储就地展开会
     *    让 SAVE_REGS 涨到 200+ 字节，把后面所有表项挤到错误偏移 ——
     *    实测症状是普通 IRQ 跳进 FIQ 表项，打 invalid_exception(2,0)。
     *    x30 此时已存进帧（[30*8]），被 bl 覆盖无妨。
     *    表项预算由向量表里各处的 VEC_ALIGN 在汇编期强制。
     */
    bl      fp_save_common
.endm

/*
 * 把下一个向量表项钉在距 exception_vector_base 恰好 \expect_off 的位置。
 *
 * 硬件按 VBAR + group*0x200 + slot*0x80 跳转，16 个表项各自只有 128 字节。
 * 某个表项的代码一旦超长，后面所有表项都会挪到错误偏移 —— 症状是异常/中断
 * 跳进别的表项代码里（实测过：普通 IRQ 落进 FIQ 表项，打 invalid_exception(2,0)
 * 然后 panic）。超长时这里的 .org 会以
 *   Error: attempt to move .org backwards
 * 在汇编期失败，指向的正是溢出的那个边界。
 *
 * 为什么不用 .if + "."：.p2align 产生的 fragment 边界会让 GAS 无法在汇编期
 * 求出 "." 的常量差值（报 non-constant expression），.org 没有这个问题。
 * 补齐的空隙落在表项末尾的 b 之后，硬件不会执行到。
 */
.macro VEC_ALIGN, expect_off
    .org exception_vector_base + \expect_off
.endm

.macro RESTORE_REGS
    ldp     x10, x11, [sp, 32 * 8]
    ldp     x30, x9,  [sp, 30 * 8]

    /* 检查 SPSR，如果不是返回到 EL0，不恢复 SP_EL0 / tpidr_el0 */
    and     x12, x11, #0xf       /* 提取 M bits (EL) */
    cmp     x12, #0              /* EL0t = 0 */
    b.ne    1f                  /* 如果不是 EL0，跳过 SP_EL0 恢复 */
    msr     sp_el0, x9           /* 只在返回用户态时恢复 SP_EL0 */
    ldr     x9, [sp, #34 * 8]    /* 只在返回用户态时恢复 tpidr_el0 */
    msr     tpidr_el0, x9
1:
    msr     elr_el1, x10
    msr     spsr_el1, x11

    /*
     * ── FP/SIMD：无条件恢复（与 SAVE_REGS 对称）─────────────────────
     *
     * 必须放在标号 1: **之后**：上面那次 spsr 判断只管 SP_EL0/tpidr_el0
     * 要不要恢复，拿它门控 FP 会把所有 EL1 返回路径的 FP 恢复全部跳过。
     * 此处 x9/x10 已死（GPR 恢复段会从帧里重新加载它们）。
     *
     * FPCR/FPSR 先于 q0-q31（对照 riscv64：fscsr 先于 fld）。从本块到
     * eret 之间只剩整数指令，不会再有 FP 状态被改动。
     */
    ldr     x9,  [sp, #100 * 8]
    ldr     x10, [sp, #101 * 8]
    msr     fpcr, x9
    msr     fpsr, x10
    ldp     q0,  q1,  [sp, #36 * 8]
    ldp     q2,  q3,  [sp, #40 * 8]
    ldp     q4,  q5,  [sp, #44 * 8]
    ldp     q6,  q7,  [sp, #48 * 8]
    ldp     q8,  q9,  [sp, #52 * 8]
    ldp     q10, q11, [sp, #56 * 8]
    ldp     q12, q13, [sp, #60 * 8]
    ldp     q14, q15, [sp, #64 * 8]
    ldp     q16, q17, [sp, #68 * 8]
    ldp     q18, q19, [sp, #72 * 8]
    ldp     q20, q21, [sp, #76 * 8]
    ldp     q22, q23, [sp, #80 * 8]
    ldp     q24, q25, [sp, #84 * 8]
    ldp     q26, q27, [sp, #88 * 8]
    ldp     q28, q29, [sp, #92 * 8]
    ldp     q30, q31, [sp, #96 * 8]

    ldp     x28, x29, [sp, 28 * 8]
    ldp     x26, x27, [sp, 26 * 8]
    ldp     x24, x25, [sp, 24 * 8]
    ldp     x22, x23, [sp, 22 * 8]
    ldp     x20, x21, [sp, 20 * 8]
    ldp     x18, x19, [sp, 18 * 8]
    ldp     x16, x17, [sp, 16 * 8]
    ldp     x14, x15, [sp, 14 * 8]
    ldp     x12, x13, [sp, 12 * 8]
    ldp     x10, x11, [sp, 10 * 8]
    ldp     x8, x9,   [sp, 8 * 8]
    ldp     x6, x7,   [sp, 6 * 8]
    ldp     x4, x5,   [sp, 4 * 8]
    ldp     x2, x3,   [sp, 2 * 8]
    ldp     x0, x1,   [sp, 0 * 8]
    add     sp, sp, TRAP_FRAME_SIZE
.endm

.macro INVALID_EXCP, kind, source
.p2align 7
    SAVE_REGS
    mov     x0, sp
    mov     x1, \kind
    mov     x2, \source
    bl      invalid_exception
    b       .Lexception_return
.endm

.macro HANDLE_SYNC
.p2align 7
    SAVE_REGS
    mov     x0, sp
    bl      handle_sync_exception
    b       .Lexception_return
.endm

.macro HANDLE_EL0_SYNC
.p2align 7
    SAVE_REGS
    /* Trap frame is complete; EL0-owned kernel work can now be preempted. */
    msr     daifclr, #2
    mov     x0, sp
    bl      handle_el0_sync_exception
    /* Keep register restore and eret atomic against nested IRQ. */
    msr     daifset, #2
    mov     x0, sp
    bl      sched_check_and_yield_from_trap
    b       .Lexception_return
.endm

.macro HANDLE_IRQ
.p2align 7
    SAVE_REGS
    mov     x0, sp
    bl      handle_irq_exception

    /* EL0 can run forever without syscalls; deliver pending signals on IRQ return. */
    mov     x0, sp
    bl      signal_deliver_from_trap

    /* 检查是否需要重调度（IRQ 触发的抢占） */
    mov     x0, sp
    bl      sched_check_and_yield_from_trap

    b       .Lexception_return
.endm


.extern invalid_exception
.extern handle_sync_exception
.extern handle_el0_sync_exception
.extern handle_irq_exception
.extern sched_check_and_yield_from_trap
.extern signal_deliver_from_trap


.section .text
.p2align 11
.global exception_vector_base
exception_vector_base:
    // current EL, with SP_EL0
    INVALID_EXCP 0 0
    VEC_ALIGN 0x080
    INVALID_EXCP 1 0
    VEC_ALIGN 0x100
    INVALID_EXCP 2 0
    VEC_ALIGN 0x180
    INVALID_EXCP 3 0
    VEC_ALIGN 0x200

    // current EL, with SP_ELx
    HANDLE_SYNC
    VEC_ALIGN 0x280
    HANDLE_IRQ
    VEC_ALIGN 0x300
    INVALID_EXCP 2 1
    VEC_ALIGN 0x380
    INVALID_EXCP 3 1
    VEC_ALIGN 0x400

    // lower EL, aarch64
    HANDLE_EL0_SYNC      /* 从用户态陷入（系统调用、缺页等） */
    VEC_ALIGN 0x480
    HANDLE_IRQ
    VEC_ALIGN 0x500
    INVALID_EXCP 2 2
    VEC_ALIGN 0x580
    INVALID_EXCP 3 2
    VEC_ALIGN 0x600

    // lower EL, aarch32
    INVALID_EXCP 0 3
    VEC_ALIGN 0x680
    INVALID_EXCP 1 3
    VEC_ALIGN 0x700
    INVALID_EXCP 2 3
    VEC_ALIGN 0x780
    INVALID_EXCP 3 3
    VEC_ALIGN 0x800

/* ── 向量表之外（不再受 128 字节槽位限制）────────────────────────── */

.Lexception_return:
    RESTORE_REGS
    eret

/*
 * fp_save_common - 把 q0-q31 + FPCR/FPSR 存进当前陷阱帧
 *
 * 由 SAVE_REGS 以 bl 调用（见那里的说明：34 条存储放不进 128 字节表项）。
 * 入口：sp = trap_frame_t 基址（GPR/CSR 部分已由 SAVE_REGS 存好），
 *       x9/x10 可破坏（其原值此前已写进帧里，见 [8*8]/[10*8]）。
 * FPCR/FPSR 在 800/808，超出 stp 的 imm7×8 范围，只能用 str。
 */
fp_save_common:
    mrs     x9, fpcr
    mrs     x10, fpsr
    str     x9,  [sp, #100 * 8]
    str     x10, [sp, #101 * 8]
    stp     q0,  q1,  [sp, #36 * 8]
    stp     q2,  q3,  [sp, #40 * 8]
    stp     q4,  q5,  [sp, #44 * 8]
    stp     q6,  q7,  [sp, #48 * 8]
    stp     q8,  q9,  [sp, #52 * 8]
    stp     q10, q11, [sp, #56 * 8]
    stp     q12, q13, [sp, #60 * 8]
    stp     q14, q15, [sp, #64 * 8]
    stp     q16, q17, [sp, #68 * 8]
    stp     q18, q19, [sp, #72 * 8]
    stp     q20, q21, [sp, #76 * 8]
    stp     q22, q23, [sp, #80 * 8]
    stp     q24, q25, [sp, #84 * 8]
    stp     q26, q27, [sp, #88 * 8]
    stp     q28, q29, [sp, #92 * 8]
    stp     q30, q31, [sp, #96 * 8]
    ret
