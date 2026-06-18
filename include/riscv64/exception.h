#ifndef RISCV64_EXCEPTION_H
#define RISCV64_EXCEPTION_H

/*
 * RISC-V 64-bit S-mode 专用异常/中断类型定义
 * 由 include/exception.h 在 ARCH_RISCV64 时自动包含
 */

#include "types.h"

/* ── trap_frame ─────────────────────────────────────────────────── *
 *
 * 布局（69 × 8 = 552 字节）：
 *   x[0]  .. x[31]   : 通用寄存器 (x0 保存为 0，sp = x[2] 保存原始值)
 *   sepc   @ [32]    : 异常发生时的 PC
 *   scause @ [33]    : 异常/中断原因
 *   stval  @ [34]    : 陷阱附加信息
 *   sstatus@ [35]    : 监管模式状态
 *   f[0]  .. f[31]   : FP 寄存器 (f0..f31)
 *   fcsr   @ [68]    : FP 控制/状态寄存器
 */
#define TRAP_FRAME_SIZE  552   /* 69 * 8 */

typedef struct {
    uint64_t x[32];      /* x0 .. x31  (x2=sp 保存进入陷阱前的原始值) */
    uint64_t sepc;       /* Supervisor Exception PC                    */
    uint64_t scause;     /* Supervisor Cause Register                  */
    uint64_t stval;      /* Supervisor Trap Value                      */
    uint64_t sstatus;    /* Supervisor Status Register                 */
    uint64_t f[32];      /* f0 .. f31  (FP registers, double-width)    */
    uint64_t fcsr;       /* FP Control/Status Register                 */
} trap_frame_t;

/* ── IRQ handler typedef (RISC-V scause-based) ──────────────────── */

typedef void (*irq_handler_t)(void *);   /* frame 实际是 trap_frame_t * */

/* ── scause 中断原因（bit63=1 时生效）────────────────────────────── */

#define SCAUSE_INTERRUPT_BIT      (1ULL << 63)

#define CAUSE_SUPERVISOR_SOFTWARE  1u
#define CAUSE_SUPERVISOR_TIMER     5u
#define CAUSE_SUPERVISOR_EXTERNAL  9u

/* ── scause 异常原因（bit63=0 时生效）───────────────────────────── */

#define CAUSE_INSN_ADDR_MISALIGN   0u
#define CAUSE_INSN_FAULT           1u
#define CAUSE_ILLEGAL_INSN         2u
#define CAUSE_BREAKPOINT           3u
#define CAUSE_LOAD_ADDR_MISALIGN   4u
#define CAUSE_LOAD_FAULT           5u
#define CAUSE_STORE_ADDR_MISALIGN  6u
#define CAUSE_STORE_FAULT          7u
#define CAUSE_USER_ECALL           8u
#define CAUSE_SUPERVISOR_ECALL     9u
#define CAUSE_INSN_PAGE_FAULT      12u
#define CAUSE_LOAD_PAGE_FAULT      13u
#define CAUSE_STORE_PAGE_FAULT     15u

/* ── sstatus 相关位 ──────────────────────────────────────────────── */

#define SSTATUS_SIE   (1UL << 1)   /* Supervisor Interrupt Enable */
#define SSTATUS_SPIE  (1UL << 5)   /* Saved SIE before trap       */
#define SSTATUS_SPP   (1UL << 8)   /* Previous privilege (S-mode) */

/* ── 声明 ────────────────────────────────────────────────────────── */

/* 初始化异常处理：设置 stvec，配置异常入口 */
void exception_init(void);

/* 调度器准备好后显式打开/关闭 S-mode 全局中断。 */
void riscv_kernel_interrupt_enable(void);
void riscv_kernel_interrupt_disable(void);

/* 诊断计数：按 trap 前特权级统计 RISC-V IRQ 入口。 */
extern volatile uint64_t g_rv_irq_from_kernel;
extern volatile uint64_t g_rv_irq_from_user;

#endif /* RISCV64_EXCEPTION_H */
