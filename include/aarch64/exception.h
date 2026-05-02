#ifndef AARCH64_EXCEPTION_H
#define AARCH64_EXCEPTION_H

/*
 * AArch64 专用异常/中断类型定义
 * 由 include/exception.h 在 ARCH_AARCH64 时自动包含
 */

#include "types.h"

/* ── trap_frame ─────────────────────────────────────────────────── */

#define NUM_REGS 31

typedef struct {
    uint64_t r[NUM_REGS]; /* x0..x30                        */
    uint64_t usp;         /* EL0/EL1 user/guest stack       */
    uint64_t elr;         /* Exception Link Register        */
    uint64_t spsr;        /* Saved Process Status Register  */
} trap_frame_t;

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

/* exception_init 由 boot/aarch64/exception.c 提供 */
void exception_init(void);

#endif /* AARCH64_EXCEPTION_H */
