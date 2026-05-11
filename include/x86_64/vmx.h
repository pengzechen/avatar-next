/*
 * include/x86_64/vmx.h — Intel VMX 常量、VMCS 字段编码与辅助类型
 *
 * 对标 ref/bare-vm/arch/x86_64/include/vmx.h，精简适配 Avatar OS。
 */
#ifndef X86_64_VMX_H
#define X86_64_VMX_H

#include "types.h"

/* ── CR 位定义 ───────────────────────────────────────────── */
#define X86_CR0_PE      0x00000001UL
#define X86_CR0_WP      0x00010000UL
#define X86_CR0_PG      0x80000000UL
#define X86_CR4_PAE     0x00000020UL
#define X86_CR4_VMXE    0x00002000UL
#define X86_CR4_PCIDE   0x00020000UL
#define X86_EFLAGS_CF   0x00000001UL
#define X86_EFLAGS_ZF   0x00000040UL

/* ── VMX MSR 地址 ─────────────────────────────────────────── */
#define MSR_IA32_FEATURE_CONTROL    0x0000003a
#define MSR_IA32_VMX_BASIC          0x00000480
#define MSR_IA32_VMX_PINBASED_CTLS  0x00000481
#define MSR_IA32_VMX_PROCBASED_CTLS 0x00000482
#define MSR_IA32_VMX_EXIT_CTLS      0x00000483
#define MSR_IA32_VMX_ENTRY_CTLS     0x00000484
#define MSR_IA32_VMX_CR0_FIXED0     0x00000486
#define MSR_IA32_VMX_CR0_FIXED1     0x00000487
#define MSR_IA32_VMX_CR4_FIXED0     0x00000488
#define MSR_IA32_VMX_CR4_FIXED1     0x00000489
#define MSR_IA32_VMX_PROCBASED_CTLS2 0x0000048b
#define MSR_IA32_VMX_TRUE_PIN       0x0000048d
#define MSR_IA32_VMX_TRUE_PROC      0x0000048e
#define MSR_IA32_VMX_TRUE_EXIT      0x0000048f
#define MSR_IA32_VMX_TRUE_ENTRY     0x00000490
#define MSR_EFER                    0xc0000080

/* ── VMX 能力 MSR 联合体 ──────────────────────────────────── */
typedef union {
    uint64_t val;
    struct {
        uint32_t revision;
        uint32_t size      : 13;
        uint32_t reserved1 : 3;
        uint32_t width     : 1;
        uint32_t dual      : 1;
        uint32_t type      : 4;
        uint32_t insouts   : 1;
        uint32_t ctrl      : 1;
        uint32_t reserved2 : 8;
    };
} vmx_basic_t;

typedef union {
    uint64_t val;
    struct { uint32_t set, clr; };
} vmx_ctrl_msr_t;

/* ── Pin-based 控制位 ─────────────────────────────────────── */
#define PIN_NMI         (1u << 3)
#define PIN_VIRT_NMI    (1u << 5)

/* ── Primary CPU-based 控制位 ────────────────────────────── */
#define CPU_HLT         (1u << 7)   /* HLT 陷入 VMM */
#define CPU_VMCALL      0           /* VMCALL 总是退出，无需显式使能 */
#define CPU_SECONDARY   (1u << 31)  /* 使能 Secondary controls */

/* ── Secondary CPU-based 控制位 ─────────────────────────── */
#define CPU_EPT         (1u << 1)
#define CPU_VPID        (1u << 5)

/* ── VM-Exit 控制位 ──────────────────────────────────────── */
#define EXI_HOST_64     (1u << 9)   /* host 为 64-bit */
#define EXI_SAVE_EFER   (1u << 20)
#define EXI_LOAD_EFER   (1u << 21)

/* ── VM-Entry 控制位 ─────────────────────────────────────── */
#define ENT_GUEST_64    (1u << 9)   /* guest 为 64-bit (IA-32e) */
#define ENT_LOAD_EFER   (1u << 15)

/* ── VMCS 字段编码 ───────────────────────────────────────── */
typedef enum {
    /* 16-bit control */
    VPID                  = 0x0000,
    /* 16-bit guest */
    GUEST_SEL_ES          = 0x0800,
    GUEST_SEL_CS          = 0x0802,
    GUEST_SEL_SS          = 0x0804,
    GUEST_SEL_DS          = 0x0806,
    GUEST_SEL_FS          = 0x0808,
    GUEST_SEL_GS          = 0x080a,
    GUEST_SEL_LDTR        = 0x080c,
    GUEST_SEL_TR          = 0x080e,
    /* 16-bit host */
    HOST_SEL_ES           = 0x0c00,
    HOST_SEL_CS           = 0x0c02,
    HOST_SEL_SS           = 0x0c04,
    HOST_SEL_DS           = 0x0c06,
    HOST_SEL_FS           = 0x0c08,
    HOST_SEL_GS           = 0x0c0a,
    HOST_SEL_TR           = 0x0c0c,
    /* 64-bit control */
    VMCS_LINK_PTR         = 0x2800,
    VMCS_LINK_PTR_HI      = 0x2801,
    /* 64-bit guest */
    GUEST_DEBUGCTL        = 0x2802,
    GUEST_EFER            = 0x2806,
    /* 64-bit host */
    HOST_EFER             = 0x2c02,
    /* 32-bit control */
    PIN_CONTROLS          = 0x4000,
    CPU_EXEC_CTRL0        = 0x4002,
    EXC_BITMAP            = 0x4004,
    PF_ERROR_MASK         = 0x4006,
    PF_ERROR_MATCH        = 0x4008,
    CR3_TARGET_COUNT      = 0x400a,
    EXI_CONTROLS          = 0x400c,
    ENT_CONTROLS          = 0x4012,
    CPU_EXEC_CTRL1        = 0x401e,
    /* 32-bit read-only */
    VMX_INST_ERROR        = 0x4400,
    EXI_REASON            = 0x4402,
    EXI_INTR_INFO         = 0x4404,
    EXI_INTR_ERROR        = 0x4406,
    EXI_INST_LEN          = 0x440c,
    /* 32-bit guest */
    GUEST_LIMIT_ES        = 0x4800,
    GUEST_LIMIT_CS        = 0x4802,
    GUEST_LIMIT_SS        = 0x4804,
    GUEST_LIMIT_DS        = 0x4806,
    GUEST_LIMIT_FS        = 0x4808,
    GUEST_LIMIT_GS        = 0x480a,
    GUEST_LIMIT_LDTR      = 0x480c,
    GUEST_LIMIT_TR        = 0x480e,
    GUEST_LIMIT_GDTR      = 0x4810,
    GUEST_LIMIT_IDTR      = 0x4812,
    GUEST_AR_ES           = 0x4814,
    GUEST_AR_CS           = 0x4816,
    GUEST_AR_SS           = 0x4818,
    GUEST_AR_DS           = 0x481a,
    GUEST_AR_FS           = 0x481c,
    GUEST_AR_GS           = 0x481e,
    GUEST_AR_LDTR         = 0x4820,
    GUEST_AR_TR           = 0x4822,
    GUEST_INTR_STATE      = 0x4824,
    GUEST_ACTV_STATE      = 0x4826,
    GUEST_SYSENTER_CS     = 0x482a,
    /* 32-bit host */
    HOST_SYSENTER_CS      = 0x4c00,
    /* natural-width control */
    CR0_MASK              = 0x6000,
    CR4_MASK              = 0x6002,
    CR0_READ_SHADOW       = 0x6004,
    CR4_READ_SHADOW       = 0x6006,
    /* natural-width read-only */
    EXI_QUALIFICATION     = 0x6400,
    /* natural-width guest */
    GUEST_CR0             = 0x6800,
    GUEST_CR3             = 0x6802,
    GUEST_CR4             = 0x6804,
    GUEST_BASE_ES         = 0x6806,
    GUEST_BASE_CS         = 0x6808,
    GUEST_BASE_SS         = 0x680a,
    GUEST_BASE_DS         = 0x680c,
    GUEST_BASE_FS         = 0x680e,
    GUEST_BASE_GS         = 0x6810,
    GUEST_BASE_LDTR       = 0x6812,
    GUEST_BASE_TR         = 0x6814,
    GUEST_BASE_GDTR       = 0x6816,
    GUEST_BASE_IDTR       = 0x6818,
    GUEST_DR7             = 0x681a,
    GUEST_RSP             = 0x681c,
    GUEST_RIP             = 0x681e,
    GUEST_RFLAGS          = 0x6820,
    GUEST_SYSENTER_ESP    = 0x6824,
    GUEST_SYSENTER_EIP    = 0x6826,
    /* natural-width host */
    HOST_CR0              = 0x6c00,
    HOST_CR3              = 0x6c02,
    HOST_CR4              = 0x6c04,
    HOST_BASE_FS          = 0x6c06,
    HOST_BASE_GS          = 0x6c08,
    HOST_BASE_TR          = 0x6c0a,
    HOST_BASE_GDTR        = 0x6c0c,
    HOST_BASE_IDTR        = 0x6c0e,
    HOST_SYSENTER_ESP     = 0x6c10,
    HOST_SYSENTER_EIP     = 0x6c12,
    HOST_RSP              = 0x6c14,
    HOST_RIP              = 0x6c16,
} vmcs_field_t;

/* ── VMCS 头部结构 ───────────────────────────────────────── */
typedef struct {
    uint32_t revision_id : 31;
    uint32_t shadow_vmcs : 1;
} vmcs_hdr_t;

typedef struct {
    vmcs_hdr_t hdr;
    uint32_t   abort;
    uint8_t    data[0];
} vmcs_t;

/* ── VMX 退出原因 ────────────────────────────────────────── */
#define VMX_REASON_EXC_NMI      0
#define VMX_REASON_EXTINT       1
#define VMX_REASON_CPUID        10
#define VMX_REASON_HLT          12
#define VMX_REASON_VMCALL       18
#define VMX_REASON_CR           28
#define VMX_REASON_EPT_VIOL     48
#define VMX_ENTRY_FAILURE       (1u << 31)

/* ── VMCS guest 活动状态 ─────────────────────────────────── */
#define ACTV_ACTIVE     0

/* ── Hypercall 编号（与 guest_test.S 保持一致）────────────── */
#define VMX_HYPERCALL_DONE      0   /* guest 正常结束（对标 HVC_DONE）  */
#define VMX_HYPERCALL_PRINT     1   /* 打印迭代计数（对标 HVC_PRINT）   */

/* ── GDT 选择子（与 boot/x86_64/boot.S 一致）────────────── */
#define X86_SEL_CODE64  0x10    /* GDT[2]: 64-bit code DPL=0 */
#define X86_SEL_DATA    0x18    /* GDT[3]: data       DPL=0 */
#define X86_SEL_TSS     0x30    /* GDT[6+7]: TSS (16B) */

/* ── CPU 辅助内联函数 ────────────────────────────────────── */
static inline uint64_t vmx_rdmsr(uint32_t idx)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(idx));
    return lo | ((uint64_t)hi << 32);
}

static inline void vmx_wrmsr(uint32_t idx, uint64_t val)
{
    __asm__ volatile("wrmsr" :: "c"(idx),
                     "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

static inline uint64_t vmx_read_cr0(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr0,%0" : "=r"(v));
    return v;
}
static inline uint64_t vmx_read_cr3(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr3,%0" : "=r"(v));
    return v;
}
static inline uint64_t vmx_read_cr4(void)
{
    uint64_t v;
    __asm__ volatile("mov %%cr4,%0" : "=r"(v));
    return v;
}
static inline void vmx_write_cr0(uint64_t v)
{
    __asm__ volatile("mov %0,%%cr0" :: "r"(v) : "memory");
}
static inline void vmx_write_cr4(uint64_t v)
{
    __asm__ volatile("mov %0,%%cr4" :: "r"(v) : "memory");
}
static inline uint64_t vmx_read_rflags(void)
{
    uint64_t f;
    __asm__ volatile("pushfq; pop %0" : "=rm"(f));
    return f;
}
static inline void vmx_cpuid(uint32_t op,
                              uint32_t *eax, uint32_t *ebx,
                              uint32_t *ecx, uint32_t *edx)
{
    *eax = op; *ecx = 0;
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "0"(*eax), "2"(*ecx) : "memory");
}

/* ── VMCS 读写内联函数 ───────────────────────────────────── */
static inline uint64_t vmcs_read(vmcs_field_t enc)
{
    uint64_t val;
    __asm__ volatile("vmread %1,%0" : "=rm"(val) : "r"((uint64_t)enc) : "cc");
    return val;
}

static inline int vmcs_write(vmcs_field_t enc, uint64_t val)
{
    uint8_t ret;
    __asm__ volatile("vmwrite %1,%2; setbe %0"
        : "=qm"(ret) : "rm"(val), "r"((uint64_t)enc) : "cc");
    return ret;
}

#endif /* X86_64_VMX_H */
