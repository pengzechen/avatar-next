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
 * 前 VCPU_SYSREGS + VCPU_SYSREGS_SIZE 字节由汇编直接寻址，
 * 其余字段仅供 C 代码访问。
 * ─────────────────────────────────────────────────────────── */
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
