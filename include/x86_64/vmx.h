/*
 * include/x86_64/vmx.h — Intel VMX 常量、VMCS 字段编码与辅助类型
 *
 * 对标 ref/bare-vm/arch/x86_64/include/vmx.h，精简适配 Avatar OS。
 */
#ifndef X86_64_VMX_H
#define X86_64_VMX_H

#include "types.h"
#include "x86_64/exception_impl.h"   /* arch_irq_flags()（引号包含会先命中同目录的 exception.h）*/

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
#define PIN_EXTINT      (1u << 0)   /* 外部中断退出到 VMM（否则走 guest IDT）*/
#define PIN_NMI         (1u << 3)
#define PIN_VIRT_NMI    (1u << 5)

/* ── Primary CPU-based 控制位 ────────────────────────────── */
#define CPU_INTR_WINDOW (1u << 2)   /* guest 开中断的瞬间 exit（投递时机）*/
#define CPU_HLT         (1u << 7)   /* HLT 陷入 VMM */
#define CPU_VMCALL      0           /* VMCALL 总是退出，无需显式使能 */
#define CPU_CR3_LOAD_STORE (1u << 15)
#define CPU_MOV_DR      (1u << 23)
#define CPU_IO_BITMAPS  (1u << 25)  /* 用 I/O bitmap 决定哪些端口陷入 */
#define CPU_USE_MSR_BITMAPS (1u << 28)
#define CPU_SECONDARY   (1u << 31)  /* 使能 Secondary controls */

/* ── Secondary CPU-based 控制位 ─────────────────────────── */
#define CPU_EPT         (1u << 1)
#define CPU_VPID        (1u << 5)
#define CPU2_UNRESTRICTED_GUEST (1u << 7)   /* 放宽段/特权检查，Linux 少踩 #GP */

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
    IO_BITMAP_A           = 0x2000,   /* 端口 0x0000-0x7FFF，1 bit/端口 */
    IO_BITMAP_B           = 0x2002,   /* 端口 0x8000-0xFFFF            */
    MSR_BITMAP            = 0x2004,   /* 4 KiB：读 1K + 写 1K + 读写 1K */
    TSC_OFFSET            = 0x2010,
    VMCS_LINK_PTR         = 0x2800,
    VMCS_LINK_PTR_HI      = 0x2801,
    EPT_POINTER           = 0x201a,   /* EPTP：EPT 根页表 + 页走行长度 + 内存类型 */
    /* MSR 自动换入换出区（进出 guest 时硬件负责装/卸，见 vmx.c）*/
    VM_EXIT_MSR_LOAD_ADDR  = 0x2008,   /* 权威值见 Linux vmx.h */
    VM_EXIT_MSR_STORE_ADDR = 0x2006,   /* 退出时把 guest 的 MSR 存回内存（swapgs 往返用）*/
    VM_ENTRY_MSR_LOAD_ADDR = 0x200a,
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
    /* VM-entry 事件注入三件套（中断/异常注入的唯一通道）*/
    VM_EXIT_MSR_LOAD_COUNT  = 0x4010,
    VM_EXIT_MSR_STORE_COUNT = 0x400e,
    VM_ENTRY_MSR_LOAD_COUNT = 0x4014,
    VM_ENTRY_INTR_INFO    = 0x4016,
    VM_ENTRY_EXC_ERRCODE  = 0x4018,
    VM_ENTRY_INST_LEN     = 0x401a,
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
    GUEST_PHYS_ADDR       = 0x2400,   /* EPT violation 的 guest 物理地址 */
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
#define VMX_REASON_TRIPLE_FAULT 2
#define VMX_REASON_INTR_WINDOW  7    /* guest 中断解除屏蔽的瞬间      */
#define VMX_REASON_CPUID        10
#define VMX_REASON_HLT          12
#define VMX_REASON_VMCALL       18
#define VMX_REASON_CR           28
#define VMX_REASON_IO           30   /* IN/OUT（I/O bitmap 命中）    */
#define VMX_REASON_MSR_READ     31
#define VMX_REASON_MSR_WRITE    32
#define VMX_REASON_EPT_VIOL     48
#define VMX_ENTRY_FAILURE       (1u << 31)

/*
 * ── VM-entry interruption-information（0x4016）─────────────────
 *
 * 这是把中断/异常送进 guest 的**唯一**通道：VMM 在 VM-entry 前写好，
 * 硬件在进入 guest 时投递。被 VMM 拦截下来的 guest 中断/异常都靠它回去。
 */
#define VMX_INTR_VALID          (1u << 31)
#define VMX_INTR_TYPE_EXTINT    (0u << 8)   /* 外部中断（含 LAPIC timer）*/
#define VMX_INTR_TYPE_NMI       (2u << 8)
#define VMX_INTR_TYPE_HWEXC     (3u << 8)   /* 硬件异常（带 vector）      */
#define VMX_INTR_TYPE_SWINT     (4u << 8)   /* INT n                      */
#define VMX_INTR_TYPE_SWEXC     (6u << 8)   /* 软件异常（UD2/INT3 等）    */
#define VMX_INTR_ERRCODE_VALID  (1u << 11)  /* 附带 error code            */
#define VMX_INTR_VECTOR(v)      ((uint64_t)((v) & 0xff))

/* ── I/O exit 的 qualification（EXI_QUALIFICATION）────────── */
#define VMX_IO_QUAL_SIZE(q)     (uint32_t)((q) & 0x7)        /* 0=1B 1=2B 3=4B */
#define VMX_IO_QUAL_IN          (1u << 3)                    /* 1=IN 0=OUT     */
#define VMX_IO_QUAL_STR         (1u << 4)                    /* 串操作         */
#define VMX_IO_QUAL_REP         (1u << 5)
#define VMX_IO_QUAL_PORT(q)     (uint32_t)(((q) >> 16) & 0xffff)

/* ── 需要 VMM 接管的 MSR ─────────────────────────────────── */
#define MSR_IA32_APIC_BASE      0x0000001b
#define MSR_IA32_PAT            0x00000277
#define MSR_STAR                0xc0000081
#define MSR_LSTAR               0xc0000082
#define MSR_CSTAR               0xc0000083
#define MSR_SYSCALL_MASK        0xc0000084
#define MSR_FS_BASE             0xc0000100
#define MSR_GS_BASE             0xc0000101
#define MSR_KERNEL_GS_BASE      0xc0000102

/* IA32_APIC_BASE 的位 */
#define APIC_BASE_ENABLE        (1ULL << 11)  /* APIC 全局使能 */
#define APIC_BASE_X2APIC        (1ULL << 10)  /* x2APIC 模式   */
#define APIC_BASE_ADDR_MASK     0x000FFFFFFFFFF000ULL

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
/* 读 RFLAGS：与 arch_irq_flags() 是同一条指令，直接转调，不重复写汇编 */
static inline uint64_t vmx_read_rflags(void)
{
    return arch_irq_flags();
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
