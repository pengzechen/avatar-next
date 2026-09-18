#ifndef AARCH64_EXCEPTION_H
#define AARCH64_EXCEPTION_H

/*
 * AArch64 专用异常/中断类型定义
 * 由 include/exception.h 在 ARCH_AARCH64 时自动包含
 */

#include "types.h"

/* ── trap_frame ─────────────────────────────────────────────────── */

#define NUM_REGS 31

/* ── trap_frame_t 布局（TRAP_FRAME_SIZE = 816 字节）───────────────────
 *
 *   偏移      字段
 *   0..247    r[NUM_REGS]  x0..x30
 *   248       usp          EL0/EL1 user/guest stack
 *   256       elr          Exception Link Register
 *   264       spsr         Saved Process Status Register
 *   272       tpidr_el0    用户态线程指针寄存器 (TLS)
 *   280       fp_pad       填充：让 q[] 落在 16 字节边界（stp q 要求）
 *   288+16*i  q[0..31]     FP/SIMD 寄存器（q31 @ 784）
 *   800       fpcr         FP 控制寄存器
 *   808       fpsr         FP 状态寄存器
 *
 * 为什么帧大小必须是 16 的倍数：硬件进入 EL1 时不会调整 SP，SP 的 16 字节
 * 对齐是纯软件不变量（AAPCS64）。SAVE_REGS 的 sub sp 若不是 16 的倍数，C
 * 函数就会在错位栈上运行（改这段之前的 280 字节正是如此），且 stp q/ldp q
 * 会做非对齐访问。
 *
 * ⚠️ 改这里的任何字段/偏移，必须同步改这两个汇编文件（它们用字面量偏移）：
 *      boot/aarch64/exception.S       SAVE_REGS / RESTORE_REGS
 *      kernel/task/aarch64/switch.S   arch_fork_resume_user
 *    下面的断言只能挡住 C 侧漂移，挡不住 .S 漂移。
 */
typedef struct {
    uint64_t    r[NUM_REGS]; /* x0..x30                        */
    uint64_t    usp;         /* EL0/EL1 user/guest stack       */
    uint64_t    elr;         /* Exception Link Register        */
    uint64_t    spsr;        /* Saved Process Status Register  */
    uint64_t    tpidr_el0;   /* 用户态线程指针寄存器 (TLS)    */
    uint64_t    fp_pad;      /* 对齐填充，见上方布局说明        */
    __uint128_t q[32];       /* q0..q31 FP/SIMD 寄存器          */
    uint64_t    fpcr;        /* FP 控制寄存器                  */
    uint64_t    fpsr;        /* FP 状态寄存器                  */
} trap_frame_t;

#define TRAP_FRAME_SIZE 816

/* 直接用 _Static_assert 而不是 assert.h 的 static_assert：后者会连带
 * #include "klog.h"，把日志头拽进这个被广泛包含的架构头。 */
_Static_assert(sizeof(trap_frame_t) == TRAP_FRAME_SIZE,
               "TRAP_FRAME_SIZE 与 boot/aarch64/exception.S 不一致");
_Static_assert(sizeof(trap_frame_t) % 16 == 0,
               "trap frame 必须是 16 字节倍数（AAPCS64 栈对齐 + stp q 对齐）");
_Static_assert(offsetof(trap_frame_t, usp) == 248, "SAVE_REGS");
_Static_assert(offsetof(trap_frame_t, elr) == 256, "SAVE_REGS / fork 恢复");
_Static_assert(offsetof(trap_frame_t, spsr) == 264, "SAVE_REGS");
_Static_assert(offsetof(trap_frame_t, tpidr_el0) == 272, "RESTORE_REGS");
_Static_assert(offsetof(trap_frame_t, q) == 288, "SAVE_REGS stp q / fork 恢复");
_Static_assert(offsetof(trap_frame_t, fpcr) == 800, "SAVE_REGS");
_Static_assert(offsetof(trap_frame_t, fpsr) == 808, "SAVE_REGS");

typedef trap_frame_t cpu_ctx_t;

/* ── Hypervisor Syndrome Register ───────────────────────────────── */

union hsr {
    uint32_t bits;

    struct {
        unsigned long iss : 25;
        unsigned long len : 1;
        unsigned long ec  : 6;
    };

    struct hsr_cond {
        unsigned long iss     : 20;
        unsigned long cc      : 4;
        unsigned long ccvalid : 1;
        unsigned long len     : 1;
        unsigned long ec      : 6;
    } cond;

    struct hsr_wfi_wfe {
        unsigned long ti      : 1;
        unsigned long sbzp    : 19;
        unsigned long cc      : 4;
        unsigned long ccvalid : 1;
        unsigned long len     : 1;
        unsigned long ec      : 6;
    } wfi_wfe;

    struct hsr_cp32 {
        unsigned long read    : 1;
        unsigned long crm     : 4;
        unsigned long reg     : 5;
        unsigned long crn     : 4;
        unsigned long op1     : 3;
        unsigned long op2     : 3;
        unsigned long cc      : 4;
        unsigned long ccvalid : 1;
        unsigned long len     : 1;
        unsigned long ec      : 6;
    } cp32;

    struct hsr_cp64 {
        unsigned long read    : 1;
        unsigned long crm     : 4;
        unsigned long reg1    : 5;
        unsigned long reg2    : 5;
        unsigned long sbzp    : 1;
        unsigned long op1     : 4;
        unsigned long cc      : 4;
        unsigned long ccvalid : 1;
        unsigned long len     : 1;
        unsigned long ec      : 6;
    } cp64;

    struct hsr_cp {
        unsigned long coproc  : 4;
        unsigned long sbz0p   : 1;
        unsigned long tas     : 1;
        unsigned long res0    : 14;
        unsigned long cc      : 4;
        unsigned long ccvalid : 1;
        unsigned long len     : 1;
        unsigned long ec      : 6;
    } cp;

    struct hsr_dabt {
        unsigned long dfsc   : 6;
        unsigned long write  : 1;
        unsigned long s1ptw  : 1;
        unsigned long cache  : 1;
        unsigned long eat    : 1;
        unsigned long sbzp0  : 6;
        unsigned long reg    : 5;
        unsigned long sign   : 1;
        unsigned long size   : 2;
        unsigned long valid  : 1;
        unsigned long len    : 1;
        unsigned long ec     : 6;
    } dabt;
};

/* ── EPT violation ──────────────────────────────────────────────── */

enum EPT_VIOLATION_REASON {
    PREFETCH = 0,
    DABT
};

typedef struct _ept_violation_info_t {
    union hsr              hsr;
    enum EPT_VIOLATION_REASON reason;
    vaddr_t                gva;
    paddr_t                gpa;
} ept_violation_info_t;

/* ── IRQ handler typedef (AArch64 GIC) ──────────────────────────── */

typedef void (*irq_handler_t)(uint64_t *);

/* AArch64 专用：访问全局 GIC handler 向量表 */
irq_handler_t *get_g_handler_vec(void);

/*
 * 「guest 持有」的中断：宿主的中断入口对它们只做优先级下降（EOIR），
 * 不写 GICC_DIR —— deactivate 由 guest 的虚拟 EOI（vGIC 的 HW=1 list
 * register）完成。原因见 boot/aarch64/exception.c: handle_irq_exception()。
 */
void irq_mark_guest_owned(int vector);
int irq_is_guest_owned(int vector);

/* exception_init 由 boot/aarch64/exception.c 提供 */
void exception_init(void);

#endif /* AARCH64_EXCEPTION_H */
