/*
 * include/vmm.h — VMM 类型和公共 API
 *
 * 架构模型：vCPU = task，VM = process（虚拟进程）
 * 当前实现仅支持 AArch64 + VHE（Virtualization Host Extensions）。
 *
 * vcpu_t 前部固定偏移（与 el2_vmcs.S / vcpu_ctx.S 必须完全一致）：
 *
 *   Offset   0 : r[0..30]      = 31 × 8 = 248 B   (x0-x30 guest GPRs)
 *   Offset 248 : sp_el1        = 8 B               (guest SP_EL1)
 *   Offset 256 : elr           = 8 B               (guest PC = ELR_EL2)
 *   Offset 264 : spsr          = 8 B               (guest PSTATE = SPSR_EL2)
 *   Offset 272 : host_ctx[13]  = 13 × 8 = 104 B   (x19-x28, x29, x30, sp)
 *   Offset 376 : sysregs[128]  = 128 B             (guest EL1 系统寄存器)
 *
 * 以上偏移由下方 VCPU_* 宏固化，请勿在 C 侧调整字段顺序。
 */
#ifndef KERNEL_VMM_H
#define KERNEL_VMM_H

#include "types.h"
#include "arch.h"

/* ── asm 可见的固定偏移（与 el2_vmcs.S 对齐）─────────────────── */
#define VCPU_R0         0           /* x0-x30, 31×8 bytes               */
#define VCPU_SP_EL1     248         /* 31*8                              */
#define VCPU_ELR        256         /* 31*8 + 8                          */
#define VCPU_SPSR       264         /* 31*8 + 16                         */
#define VCPU_HOSTCTX    272         /* 31*8 + 24                         */
#define VCPU_SYSREGS    376         /* 31*8 + 24 + 13*8                  */

/* sysregs 缓冲区：保存 guest EL1 系统寄存器，128 B 足够覆盖 Phase 2 用到的子集 */
#define VCPU_SYSREGS_SIZE  128

/* ── VMM 退出原因（对标 x86 VMX_RESUME/VMEXIT 等）──────────── */
#define EL2_RESUME   0   /* 继续运行 guest              */
#define EL2_VMEXIT   1   /* guest 正常结束              */
#define EL2_VMABORT  2   /* guest 中止                  */
#define EL2_VMSKIP   3   /* 跳过当前指令后继续          */
#define EL2_EXIT     4   /* 未处理陷入，终止 VMM 循环   */

/* HVC hypercall number（与 guest_test.S 保持一致）*/
#define HVC_DONE   0
#define HVC_PRINT  1

/* ── 最大 vCPU 数量 ────────────────────────────────────────── */
#define MAX_VCPUS  4

/* ── vCPU 控制块 ─────────────────────────────────────────────
 *
 * 架构特定：AArch64 与 x86_64 使用不同的字段布局。
 * vmm_run_vcpu 只访问公共字段 vcpu_id / launched / vm。
 * ─────────────────────────────────────────────────────────── */
#if ARCH_AARCH64
/* 前 VCPU_SYSREGS + VCPU_SYSREGS_SIZE 字节由汇编直接寻址，勿调整顺序 */
typedef struct vcpu {
    /* ── asm-accessible（勿改动顺序）────────────────────── */
    uint64_t r[31];                    /* x0-x30 guest GPRs    offset=0   */
    uint64_t sp_el1;                   /* guest SP_EL1         offset=248 */
    uint64_t elr;                      /* guest PC             offset=256 */
    uint64_t spsr;                     /* guest PSTATE         offset=264 */
    uint64_t host_ctx[13];             /* host callee-saved+sp offset=272 */
    uint8_t  sysregs[VCPU_SYSREGS_SIZE]; /* guest EL1 sysregs offset=376 */

    /* ── C-only 字段 ──────────────────────────────────── */
    int      vcpu_id;                  /* vCPU 编号                       */
    int      launched;                 /* 已进入 guest 至少一次？         */
    uint64_t page_table_base;          /* VTTBR_EL2 (VMID:PGD)           */
    struct vm *vm;                     /* 所属 VM 反向指针                */
} vcpu_t;

#elif ARCH_X86_64
/*
 * x86_64 guest GPR 保存区（与 vmx_run.S VCPU_X86_* 偏移一致）
 *   rax +0x00  rbx +0x08  rcx +0x10  rdx +0x18
 *   rbp +0x20  rsi +0x28  rdi +0x30
 *   r8  +0x38  r9  +0x40  r10 +0x48  r11 +0x50
 *   r12 +0x58  r13 +0x60  r14 +0x68  r15 +0x70
 *   rflags +0x78
 */
typedef struct {
    uint64_t rax;    /* +0x00 */
    uint64_t rbx;    /* +0x08 */
    uint64_t rcx;    /* +0x10 */
    uint64_t rdx;    /* +0x18 */
    uint64_t rbp;    /* +0x20 */
    uint64_t rsi;    /* +0x28 */
    uint64_t rdi;    /* +0x30 */
    uint64_t r8;     /* +0x38 */
    uint64_t r9;     /* +0x40 */
    uint64_t r10;    /* +0x48 */
    uint64_t r11;    /* +0x50 */
    uint64_t r12;    /* +0x58 */
    uint64_t r13;    /* +0x60 */
    uint64_t r14;    /* +0x68 */
    uint64_t r15;    /* +0x70 */
    uint64_t rflags; /* +0x78 (from VMCS GUEST_RFLAGS on exit) */
} x86_guest_regs_t;

/* vmx_run.S 中使用的 vcpu->regs 字段偏移（vcpu_t 起始处）*/
#define VCPU_X86_RAX    0x00
#define VCPU_X86_RBX    0x08
#define VCPU_X86_RCX    0x10
#define VCPU_X86_RDX    0x18
#define VCPU_X86_RBP    0x20
#define VCPU_X86_RSI    0x28
#define VCPU_X86_RDI    0x30
#define VCPU_X86_R8     0x38
#define VCPU_X86_R9     0x40
#define VCPU_X86_R10    0x48
#define VCPU_X86_R11    0x50
#define VCPU_X86_R12    0x58
#define VCPU_X86_R13    0x60
#define VCPU_X86_R14    0x68
#define VCPU_X86_R15    0x70

typedef struct vcpu {
    x86_guest_regs_t regs;  /* guest GPRs, offset=0, 与 vmx_run.S 对齐  */
    /* ── C-only 字段 ──────────────────────────────────── */
    int      vcpu_id;
    int      launched;
    uint64_t page_table_base;   /* 将来用于 EPT/guest CR3               */
    struct vm *vm;
} vcpu_t;

#elif ARCH_RISCV64
/*
 * RISC-V H-extension 软件 VMCS
 *
 * vcpu_t 前部固定偏移（与 hext_vcpu.S VCPU_RV_* 宏完全一致）：
 *
 *   Offset   0 : r[0..31]      = 32 × 8 = 256 B   (x0-x31 guest GPRs)
 *   Offset 256 : vsepc          = 8 B               (guest PC = vsepc)
 *   Offset 264 : vsstatus       = 8 B               (guest sstatus)
 *   Offset 272 : vstvec         = 8 B               (guest trap vector)
 *   Offset 280 : vsscratch      = 8 B               (guest sscratch)
 *   Offset 288 : vsatp          = 8 B               (guest page table)
 *   Offset 296 : vsie           = 8 B               (guest int enable)
 *   Offset 304 : scause_save    = 8 B               (陷入原因，HS侧保存)
 *   Offset 312 : stval_save     = 8 B               (陷入附加值)
 *   Offset 320 : htval_save     = 8 B               (Stage-2 guest PA)
 *   Offset 328 : host_ctx[15]   = 15 × 8 = 120 B   (ra,s0-s11,sp,orig_stvec)
 *
 * host_ctx 布局（与 hext_vcpu.S 对齐）：
 *   [0]  ra   [1]  s0   [2]  s1   [3]  s2   [4]  s3
 *   [5]  s4   [6]  s5   [7]  s6   [8]  s7   [9]  s8
 *   [10] s9   [11] s10  [12] s11  [13] sp   [14] orig_stvec
 */

/* ── asm 可见的固定偏移 ──────────────────────────────────────── */
#define VCPU_RV_R0         0            /* x0-x31, 32×8 bytes        */
#define VCPU_RV_VSEPC      256          /* 32*8                      */
#define VCPU_RV_VSSTATUS   264
#define VCPU_RV_VSTVEC     272
#define VCPU_RV_VSSCRATCH  280
#define VCPU_RV_VSATP      288
#define VCPU_RV_VSIE       296
#define VCPU_RV_SCAUSE     304
#define VCPU_RV_STVAL      312
#define VCPU_RV_HTVAL      320
#define VCPU_RV_HOSTCTX    328          /* host_ctx[15] = 120 B      */
#define VCPU_RV_HOSTSTVEC  (328 + 14*8) /* = 440: 原主 stvec (host_ctx[14]) */

typedef struct vcpu {
    /* ── asm-accessible（勿改动顺序）────────────────────── */
    uint64_t r[32];         /* x0-x31 guest GPRs    offset=0     */
    uint64_t vsepc;         /* guest PC             offset=256   */
    uint64_t vsstatus;      /* guest sstatus        offset=264   */
    uint64_t vstvec;        /* guest trap vector    offset=272   */
    uint64_t vsscratch;     /* guest sscratch       offset=280   */
    uint64_t vsatp;         /* guest page table     offset=288   */
    uint64_t vsie;          /* guest int enable     offset=296   */
    uint64_t scause_save;   /* 陷入原因              offset=304   */
    uint64_t stval_save;    /* 陷入附加值            offset=312   */
    uint64_t htval_save;    /* Stage-2 guest PA     offset=320   */
    uint64_t host_ctx[15];  /* ra,s0-s11,sp,orig_stvec offset=328
                             * [0]=ra [1-12]=s0-s11 [13]=sp [14]=orig_stvec */

    /* ── C-only 字段 ──────────────────────────────────── */
    int      vcpu_id;
    int      launched;
    struct vm *vm;
} vcpu_t;

#else
/* 其他架构：空壳，只有公共字段 */
typedef struct vcpu {
    int      vcpu_id;
    int      launched;
    uint64_t page_table_base;
    struct vm *vm;
} vcpu_t;
#endif /* ARCH_* */

/* ── VM 配置 ─────────────────────────────────────────────────── */
typedef struct vm_cfg {
    uint64_t mem_base;   /* guest 物理内存基址（Stage-2 IPA base）*/
    uint64_t mem_size;   /* guest 物理内存大小                    */
    int      nr_vcpus;   /* vCPU 数量                             */
} vm_cfg_t;

/* ── VM 控制块 ───────────────────────────────────────────────── */
typedef struct vm {
    vm_cfg_t cfg;
    vcpu_t   vcpus[MAX_VCPUS];  /* 静态嵌入，不动态分配 */
    int      nr_vcpus;
} vm_t;

/* ── AArch64 专用汇编接口（仅 aarch64 编译时可见）──────────── */
#if ARCH_AARCH64

/*
 * el2_enter_guest — 进入 EL1 guest（VHE 版本，对标 vmlaunch/vmresume）
 *
 * 保存 host callee-saved 寄存器到 vcpu->host_ctx，
 * 从 vcpu->r / vcpu->elr / vcpu->spsr / vcpu->sp_el1 加载 guest 状态，
 * 修改 HCR_EL2（TGE→0, VM→1），安装 guest trap 向量，然后 eret 进入 EL1。
 * 当 guest 陷入 EL2（el2_trap_exit）时，保存 guest 状态并恢复 host 上下文，
 * 从此函数 ret 返回。
 */
void el2_enter_guest(vcpu_t *vcpu);

/*
 * save_sysregs_el12   — 保存 guest EL1 系统寄存器到 buf
 * restore_sysregs_el12 — 从 buf 恢复 guest EL1 系统寄存器
 *
 * 使用 _el12 后缀寄存器绕过 VHE E2H 别名，
 * 直接访问真实 EL1 寄存器（而非 EL2 别名）。
 */
void save_sysregs_el12(void *buf);
void restore_sysregs_el12(void *buf);

/*
 * set_stage2_pgd — 写入 VTTBR_EL2（Stage-2 根页表 + VMID）
 */
void set_stage2_pgd(uint64_t pgd_phys, uint32_t vmid);

#endif /* ARCH_AARCH64 */

/* ── 架构钩子（每个架构各自实现）────────────────────────────── */
/*
 * 由 vmm_run_vcpu（vmm.c）调用；每个架构在 arch/vmx.c 或 el2_run.c 中提供实现。
 *
 *   vmm_arch_restore_guest_ctx — 进入 guest 循环前恢复架构相关上下文
 *   vmm_arch_enter_guest       — 执行一次 guest 入口（eret/vmlaunch/vmresume）
 *                                返回 1 成功，0 入口失败
 *   vmm_arch_exit_handler      — 处理一次 VM exit，返回 EL2_* / VMX_* 状态码
 *   vmm_arch_save_guest_ctx    — guest 退出后保存架构相关上下文
 */
void vmm_arch_restore_guest_ctx(vcpu_t *vcpu);
int  vmm_arch_enter_guest(vcpu_t *vcpu);   /* 1=success, 0=entry-failed */
int  vmm_arch_exit_handler(vcpu_t *vcpu);
void vmm_arch_save_guest_ctx(vcpu_t *vcpu);

/*
 * vmm_run_vcpu — 架构无关 vCPU 执行主循环（实现在 vmm.c）
 * 返回 0：guest 正常退出；返回 -1：未处理的 exit。
 */
int vmm_run_vcpu(vcpu_t *vcpu);

/* ── VM 管理 API ─────────────────────────────────────────────── */

/*
 * vm_create — 初始化 VM 实例
 * @vm: 调用方分配的 vm_t（vm->cfg 已填好）
 * 返回 0 成功，-1 失败。
 */
int vm_create(vm_t *vm);

/* 前向声明：避免与 task.h 循环包含 */
struct task;

/*
 * vcpu_task_create — 为指定 vCPU 创建内核任务
 * 任务被调度时执行 vmm_run_vcpu(vcpu) VMM 主循环。
 */
struct task *vcpu_task_create(vcpu_t *vcpu, uint8_t priority);

#endif /* KERNEL_VMM_H */
