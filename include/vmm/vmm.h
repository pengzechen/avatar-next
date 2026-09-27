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
 *   Offset 376 : host_vbar     = 8 B               host VBAR_EL2
 *   Offset 384 : host_tpidr    = 8 B               host per-CPU pointer
 *   Offset 392 : sysregs[128]  = 128 B             (guest EL1 系统寄存器)
 *
 * 以上偏移由下方 VCPU_* 宏固化，请勿在 C 侧调整字段顺序。
 */
#ifndef KERNEL_VMM_H
#define KERNEL_VMM_H

#include "types.h"
#include "arch.h"
#include "vmm_mmio.h"
#include "vmm_virq.h"     /* virq_t：设备中断线的架构无关命名 */

/*
 * 中断控制器的头**只能出现在各自架构的守卫里** —— 共享头不该命名任何具体
 * 控制器（第二轮做中断抽象时，这里会换成唯一的 vmm_vintc.h）。
 * ⚠️ vmm_vgic.h 从前是无条件包含的，于是 GICv2 的定义被拖进包含本头的
 * 全部 TU（三个架构都算）。
 */
#if ARCH_AARCH64
#include "aarch64/stage2.h"     /* s2_ctx_t：vm_t 里每个 VM 一份 stage-2 */
#include "vmm/vmm_vgic.h"       /* vgic_t：GICv2 后端 */
#include "vmm/vmm_vpl011.h"     /* vpl011_init/destroy：状态已搬进 vpl011.c 的池 */
#endif
#if ARCH_AARCH64 && DRIVER_GIC_V3
#include "vmm_vgicv3.h"
#endif

#if ARCH_RISCV64
#include "riscv64/gstage.h"     /* gstage_ctx_t：vm_t 里每个 VM 一份 G-stage */
#include "vmm/vmm_vplic.h"      /* vplic_state_t：每个 VM 一份 vPLIC */
#endif
/*
 * uart16550 是 riscv64 与 x86_64 共用的 guest 控制台型号（两个架构都编
 * kernel/vmm/vdev/vuart16550.c），所以状态字段两个架构都要有。
 */
#if ARCH_RISCV64 || ARCH_X86_64
#include "vmm/vmm_uart16550.h"  /* uart16550_state_t：每个 VM 一份控制台状态 */
#endif

#if ARCH_X86_64
#include "x86_64/ept.h"          /* ept_ctx_t：vm_t 里每个 VM 一份 EPT */
#include "vmm/vmm_vlapic.h"      /* vlapic_state_t：每个 VM 一份 vLAPIC */
#include "vmm/vmm_x86_chipset.h" /* x86_chipset_state_t：PIC/PIT/IOAPIC 桩 */
#endif

/* ── asm 可见的固定偏移（与 el2_vmcs.S 对齐）─────────────────── */
#define VCPU_R0         0           /* x0-x30, 31×8 bytes               */
#define VCPU_SP_EL1     248         /* 31*8                              */
#define VCPU_ELR        256         /* 31*8 + 8                          */
#define VCPU_SPSR       264         /* 31*8 + 16                         */
#define VCPU_HOSTCTX    272         /* 31*8 + 24                         */
#define VCPU_HOST_VBAR  376         /* saved host VBAR_EL2               */
#define VCPU_HOST_TPIDR 384         /* saved host per-CPU pointer        */
#define VCPU_SYSREGS    392         /* guest EL1 sysregs                 */
#define VCPU_EXIT_TYPE  520         /* VCPU_SYSREGS + VCPU_SYSREGS_SIZE  */
#define VCPU_ESR        528
#define VCPU_FAR        536
#define VCPU_HPFAR      544
#define VCPU_CNTV_CTL   552
#define VCPU_CNTV_CVAL  560

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

/* ── 同一个内核里最多几个 VM ──────────────────────────────────
 *
 * 每个 VM 的 vm_t 里嵌着 vGIC（GICv3 时约 70 KB，含 64 KB 的 dist_regs）、
 * vpl011 状态、以及 stage-2 的静态 L1/L2 —— 合计约 80 KB。静态池 4 个 =
 * 约 320 KB BSS，相对内核窗口（0x40080000 起、到 rootfs 保留区还有 500 MB+）
 * 可以忽略；换成动态分配反而要几十页**连续**物理内存，还会丢掉表所需的
 * 对齐保证。所以用静态池。
 *
 * ⚠️ 必须 <= stage2.h 的 STAGE2_MAX_VMS（静态 stage-2 表按 slot 索引）。
 */
#define MAX_VMS    4

/* VM 生命周期状态（vm.c 的 vm_alloc/vm_free 维护）*/
#define VM_FREE      0   /* 槽位可用                        */
#define VM_LOADING   1   /* 正在加载镜像、建 stage-2        */
#define VM_RUNNING   2   /* vCPU 任务在跑                   */
#define VM_SUSPENDED 3   /* vCPU 任务被 park，状态完整保留  */
#define VM_DYING     4   /* 已请求销毁，等 vCPU 任务收拾完  */

/*
 * 哪些架构已经实现「从 rootfs 加载并启动 Linux guest」（kernel/vmm/guest_loader.c）。
 * 用一个宏而不是到处写 #if ARCH_AARCH64：/dev/vmm、kernel_main 的直启分支
 * 都要按它开关，散落的架构判断会在加架构时漏掉一处。
 */
#define VMM_GUEST_LINUX_SUPPORTED  (ARCH_AARCH64 || ARCH_RISCV64 || ARCH_X86_64)

/* 前向声明：避免与 task.h 循环包含 */
struct task;

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
    uint64_t host_vbar;                /* host VBAR_EL2        offset=376 */
    uint64_t host_tpidr;               /* host per-CPU pointer offset=384 */
    uint8_t  sysregs[VCPU_SYSREGS_SIZE]; /* guest EL1 sysregs offset=392 */
    uint64_t exit_type;                /* 0=sync, 1=IRQ, 2=FIQ, 3=SError */
    uint64_t esr;                      /* ESR_EL2 captured on guest exit  */
    uint64_t far;                      /* FAR_EL2 captured on guest exit  */
    uint64_t hpfar;                    /* HPFAR_EL2 captured on guest exit*/
    uint64_t cntv_ctl;                 /* guest virtual timer control     */
    uint64_t cntv_cval;                /* guest virtual timer compare     */

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
    /* ── C-only 字段（launched 的位置被 vmx_run.S 硬编码为 0x84）── */
    int      vcpu_id;
    int      launched;
    uint64_t page_table_base;   /* guest CR3 的 GPA（EPT 下的物理地址）*/
    struct vm *vm;

    /*
     * ── x86 虚拟 MSR 影子 ─────────────────────────────────────
     *
     * MSR bitmap 把所有 MSR 访问都陷入 VMM（vmx.c 的 msr_bitmap_init），
     * 因此 guest 的 wrmsr **不会**碰到真实硬件 —— 否则 Linux 一开机写
     * STAR/LSTAR/EFER/FS_BASE 就把宿主自己的 syscall 环境搞坏了。
     * 这里只放 Linux 真正会用到的那几个；其余 MSR 访问按「未知」处理
     * （读回 0、写忽略），需要时再往这里加。
     */
    uint64_t msr_efer;
    uint64_t msr_star, msr_lstar, msr_cstar, msr_sfmask;
    uint64_t msr_fs_base, msr_gs_base, msr_kernel_gs_base;
    uint64_t msr_pat;
    uint64_t apic_base;          /* IA32_APIC_BASE：vLAPIC 的使能/模式位 */

    /*
     * ── 待注入 guest 的事件 ───────────────────────────────────
     *
     * VS→HS 抢断后，被 VMM 拦下的中断/异常要经 VM-entry
     * interruption-information 送回 guest。guest 当时屏蔽着中断就置
     * intr_window，让硬件在它开中断的那一刻(exit 7)再回来投递。
     */
    uint64_t pending_event;      /* 0 = 无；否则是 0x4016 的完整编码 */
    uint64_t pending_errcode;
    int      intr_window;

    /*
     * ── guest 启动状态（由 guest_loader 填，vmcs_init_guest 使用）──
     *
     * x86 的 guest 状态大部分在 VMCS 字段里（RIP/RSP/CR3/段），不在
     * vcpu_t 里。为了不改 vmx.c 里那套"玩具 guest 用 entry 指针"的老路径，
     * 这里放一组可选的启动值：置了就用它，没置就退回旧行为。
     */
    uint64_t g_rip;              /* 0 = 用 vmcs_init_guest 的 entry 参数 */
    uint64_t g_rsp;
    uint64_t g_cr3;              /* **GPA**（EPT 负责翻译）*/
    uint64_t g_gdt_base;         /* GPA */
    uint32_t g_gdt_limit;
    int      g_boot_linux;       /* 1 = Linux 引导路径 */

    /*
     * VMCS 是否已在**跑 vCPU 的这颗核**上初始化过（每个 VM 生命周期一次）。
     *
     * 不能沿用 vmx_vcpu_setup() 在 /bin/vmm-run 所在核上的初始化：
     *   1) VMCLEAR 只对**本核 current 的** VMCS 有效 —— setup 核 clear 之后
     *      vCPU 核一 VMPTRLD，那块 VMCS 就"搬"走了，下次再在 setup 核上
     *      clear 是空操作，launch state 永远停在 launched ⇒ 第二次启动的
     *      VMLAUNCH 报 inst_error=0x4（VMLAUNCH with non-clear VMCS）。
     *   2) VMCLEAR 会把 VMCS 复位成"上次退出时的快照"，所以它必须发生在
     *      写字段**之前** —— 先 clear 再 setup 的顺序不能反。
     * 两条合起来只有一个写法：clear + 全部初始化一起，在 vCPU 核上做完。
     * 字段放末尾 —— vmx_run.S 硬编码了 launched@0x84 等偏移，不能动前面。
     */
    int      vmcs_ready;
} vcpu_t;

/*
 * 编译期钉死结构体偏移 == vmx_run.S 里硬编码的偏移。
 * vmx_run.S 用 0x78(regs.rflags) / 0x84(launched) 直接寻址，改字段顺序
 * 会让 guest 寄存器保存到错误的槽位 —— 而症状是「guest 随机跑飞」。
 */
_Static_assert(offsetof(vcpu_t, regs)     == VCPU_X86_RAX, "vmx_run.S regs @0x00");
_Static_assert(offsetof(vcpu_t, regs.rflags) == 0x78,      "vmx_run.S rflags @0x78");
_Static_assert(offsetof(vcpu_t, vcpu_id)  == 0x80,         "vmx_run.S vcpu_id @0x80");
_Static_assert(offsetof(vcpu_t, launched) == 0x84,         "vmx_run.S launched @0x84");

#elif ARCH_RISCV64
/*
 * RISC-V H-extension 软件 VMCS
 *
 * vcpu_t 前部固定偏移（与 hext_vcpu.S VCPU_RV_* 宏完全一致）：
 *
 *   Offset   0 : r[0..31]      = 32 × 8 = 256 B   (x0-x31 guest GPRs)
 *   Offset 256 : vsepc          = 8 B               (guest 自己的 sepc)
 *   Offset 264 : vsstatus       = 8 B               (guest sstatus)
 *   Offset 272 : vstvec         = 8 B               (guest trap vector)
 *   Offset 280 : vsscratch      = 8 B               (guest sscratch)
 *   Offset 288 : vsatp          = 8 B               (guest page table)
 *   Offset 296 : vsie           = 8 B               (guest int enable)
 *   Offset 304 : scause_save    = 8 B               (陷入原因，HS侧保存)
 *   Offset 312 : stval_save     = 8 B               (陷入附加值)
 *   Offset 320 : htval_save     = 8 B               (Stage-2 guest PA)
 *   Offset 328 : hstatus_save   = 8 B               (陷阱时的 hstatus)
 *   Offset 336 : pc             = 8 B               (恢复 PC，来自 HS sepc)
 *   Offset 344 : host_ctx[16]   = 16 × 8 = 128 B   (ra,s0-s11,sp,orig_stvec,tp)
 *   Offset 472 : htinst_save    = 8 B               (htinst，MMIO 取指回退)
 *
 * host_ctx 布局（与 hext_vcpu.S 对齐）：
 *   [0]  ra   [1]  s0   [2]  s1   [3]  s2   [4]  s3
 *   [5]  s4   [6]  s5   [7]  s6   [8]  s7   [9]  s8
 *   [10] s9   [11] s10  [12] s11  [13] sp   [14] orig_stvec
 *   [15] tp
 *
 * ⚠️ host_ctx 必须覆盖 ra/s0-s11/sp/gp/tp 全部「跨调用保持」的寄存器。
 * gp 由陷阱向量用 `la gp, __global_pointer$` 重建（不占槽位），**tp 必须
 * 显式存取** —— 宿主内核把 tp 当 per-CPU 指针用，而进入 guest 时它会被
 * 换成 guest 的 x4。漏了这个槽位，宿主回来之后每一次取 CPU id / per-CPU
 * 变量都会踩到 guest 的地址上（实测表现为 klog_cpu_id 取址缺页 + 之后
 * 陷在异常处理里的死循环）。历史上这里只有 15 个槽位、只恢复了 gp，
 * tp 是后补的第 16 个 —— 加寄存器时请连带检查这一条。
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
#define VCPU_RV_HSTATUS    328          /* 陷阱时的 hstatus（含 SPVP）*/
#define VCPU_RV_PC         336          /* 恢复 PC（HS sepc）        */
#define VCPU_RV_HOSTCTX    344          /* host_ctx[16] = 128 B      */
#define VCPU_RV_HOSTSTVEC  (344 + 14*8) /* = 456: 原主 stvec (host_ctx[14]) */
#define VCPU_RV_HTINST     (344 + 16*8) /* = 472: htinst（在 host_ctx 之后）*/

typedef struct vcpu {
    /* ── asm-accessible（勿改动顺序）────────────────────── */
    uint64_t r[32];         /* x0-x31 guest GPRs    offset=0     */
    uint64_t vsepc;         /* guest 自己的 sepc    offset=256   */
    uint64_t vsstatus;      /* guest sstatus        offset=264   */
    uint64_t vstvec;        /* guest trap vector    offset=272   */
    uint64_t vsscratch;     /* guest sscratch       offset=280   */
    uint64_t vsatp;         /* guest page table     offset=288   */
    uint64_t vsie;          /* guest int enable     offset=296   */
    uint64_t scause_save;   /* 陷入原因              offset=304   */
    uint64_t stval_save;    /* 陷入附加值            offset=312   */
    uint64_t htval_save;    /* Stage-2 guest PA     offset=320   */
    uint64_t hstatus_save;  /* 陷阱时的 hstatus     offset=328   */
    uint64_t pc;            /* 恢复 PC（HS sepc）   offset=336   */
    uint64_t host_ctx[16];  /* ra,s0-s11,sp,orig_stvec,tp    offset=344
                             * [0]=ra [1-12]=s0-s11 [13]=sp [14]=orig_stvec
                             * [15]=tp（per-CPU 指针，见上方 host_ctx 说明）*/

    /* ⚠️ htinst ≠ htval：htval 是出错 GPA>>2，htinst 才是触发陷阱的指令。
     * MMIO 模拟在「从 guest PC 取指失败」时用 htinst 回退，用错寄存器会把
     * 一个地址当指令解码 —— 解出的长度/寄存器全是垃圾。偏移放在 host_ctx
     * 之后，纯粹是为了不动 HCTX_* 那一组宏。*/
    uint64_t htinst_save;   /* htinst               offset=472   */

    /* ── C-only 字段 ──────────────────────────────────── */
    int      vcpu_id;
    int      launched;
    struct vm *vm;
    uint64_t timer_deadline;  /* guest SBI 定时器截止（guest time 单位），0=未设置 */
} vcpu_t;

/*
 * 编译期钉死结构体偏移 == hext_vcpu.S 里的 VCPU_RV_* / HCTX_* 宏。
 * 两边只靠注释约定，加一个字段就会整体错位，而错位的表现是"guest 随机
 * 跑飞"这种极难反查的故障（历史上 tp 槽位就是漏加过一次，见上方说明）。
 * 与 include/aarch64/exception.h 钉 trap_frame_t 是同一手法。
 */
_Static_assert(offsetof(vcpu_t, r)           == VCPU_RV_R0,       "hext_vcpu.S VCPU_R0");
_Static_assert(offsetof(vcpu_t, vsepc)       == VCPU_RV_VSEPC,    "hext_vcpu.S VCPU_VSEPC");
_Static_assert(offsetof(vcpu_t, vsstatus)    == VCPU_RV_VSSTATUS, "hext_vcpu.S VCPU_VSSTATUS");
_Static_assert(offsetof(vcpu_t, vstvec)      == VCPU_RV_VSTVEC,   "hext_vcpu.S VCPU_VSTVEC");
_Static_assert(offsetof(vcpu_t, vsscratch)   == VCPU_RV_VSSCRATCH,"hext_vcpu.S VCPU_VSSCRATCH");
_Static_assert(offsetof(vcpu_t, vsatp)       == VCPU_RV_VSATP,    "hext_vcpu.S VCPU_VSATP");
_Static_assert(offsetof(vcpu_t, vsie)        == VCPU_RV_VSIE,     "hext_vcpu.S VCPU_VSIE");
_Static_assert(offsetof(vcpu_t, scause_save) == VCPU_RV_SCAUSE,   "hext_vcpu.S VCPU_SCAUSE");
_Static_assert(offsetof(vcpu_t, stval_save)  == VCPU_RV_STVAL,    "hext_vcpu.S VCPU_STVAL");
_Static_assert(offsetof(vcpu_t, htval_save)  == VCPU_RV_HTVAL,    "hext_vcpu.S VCPU_HTVAL");
_Static_assert(offsetof(vcpu_t, hstatus_save)== VCPU_RV_HSTATUS,  "hext_vcpu.S VCPU_HSTATUS");
_Static_assert(offsetof(vcpu_t, pc)          == VCPU_RV_PC,       "hext_vcpu.S VCPU_PC");
_Static_assert(offsetof(vcpu_t, host_ctx)    == VCPU_RV_HOSTCTX,  "hext_vcpu.S HCTX_RA");
/* host_ctx[15] 必须正好是 tp 槽 —— 漏掉它宿主的 per-CPU 指针就被 guest 覆盖 */
_Static_assert(offsetof(vcpu_t, host_ctx) + 15 * 8 == VCPU_RV_HOSTCTX + 15 * 8,
               "hext_vcpu.S HCTX_TP");
_Static_assert(offsetof(vcpu_t, htinst_save) == VCPU_RV_HTINST, "hext_vcpu.S VCPU_HTINST");
#endif /* ARCH_* */

/* ── VM 配置 ─────────────────────────────────────────────────── */
typedef struct vm_cfg {
    uint64_t mem_base;   /* guest 物理内存基址（Stage-2 IPA base）*/
    uint64_t mem_size;   /* guest 物理内存大小                    */
    int      nr_vcpus;   /* vCPU 数量                             */
} vm_cfg_t;

/* ── MMIO 总线（vmm_mmio.h）──────────────────────────────────── */
struct mmio_bus;

/* ── VM 控制块 ───────────────────────────────────────────────── */
typedef struct vm {
    vm_cfg_t cfg;
    vcpu_t   vcpus[MAX_VCPUS];  /* 静态嵌入，不动态分配 */
    int      nr_vcpus;

    /* ── VM 身份与生命周期（vm.c 的静态池管理）───────────────── */
    uint32_t vmid;              /* 1..255，同时写进 VTTBR_EL2 的 VMID 域 */
    int      slot;              /* 静态池下标（也用于索引 stage-2 静态表）*/
    int      state;             /* VM_FREE / VM_LOADING / ...          */
    volatile int stop_req;      /* 置位后 vCPU 任务在主循环顶部退出     */

#if ARCH_AARCH64
    /*
     * 每个 VM 自己的 Stage-2：页表、VMID、RAM 窗口、按需页账本。
     *
     * 从前这些是全局的（stage2.c 里的 s2_l1/s2_l2/s_vttbr…），只能有一个 VM；
     * 而且 guest RAM 要预先分配 192 MiB 并整片清零。现在改成按需分页：
     * 表初始化后是空的，guest 首次访问某页时才分配宿主物理页并装映射
     * （缺页处理见 kernel/vmm/aarch64/el2_run.c 的 handle_dabt）。
     */
    s2_ctx_t s2;

    /*
     * 控制台设备状态（vpl011_state_t）与它的锁**不在这里** ——
     * 它们已按 vm->slot 搬进 kernel/vmm/vdev/vpl011.c 的静态池。
     * 见该文件顶部 vpl011_slot_t 的说明。
     */
#endif

    /*
     * 控制台归属：1 = 本 VM 的输出进自己的 TX 环（等 helper 来取）；
     * 0 = 直打宿主控制台（直启模式，或后台 VM 的输出走 klog）。
     *
     * ⚠️ 这个标志从前叫设备 state 里的 tx_channel（aarch64 是 vpl011 的，
     * riscv/x86 是 uart16550 的），是**宿主侧的运行模式选择**，不是设备
     * 寄存器状态。放在设备 state 里的时候，*_init() 那句 memset 必须特意
     * 把它存下来再恢复（GUEST_CONSOLE.md §2 记着这个陷阱）。挪到 vm_t 之后
     * memset 可以整片清零，陷阱自然消失。
     *
     * 设备侧通过 dev->priv → state->owner 回指到这里读取。
     */
    int console_owned;

#if ARCH_RISCV64 || ARCH_X86_64
    /*
     * uart16550 控制台状态（两个架构共用同一份设备模型）。
     *
     * 从前这是一对文件级 static（g_uart16550 + g_uart16550_lock）—— 整机
     * 只有一份，于是第二个 VM 的 uart16550_init() 一句 memset 就把第一个 VM
     * 的 RX/TX FIFO 清空，两个 VM 从此抢同一个控制台。现在每 VM 一份。
     * 与 aarch64 侧 vpl011 的处理完全对称。
     */
    uart16550_state_t uart16550;
    spinlock_noirq_t  uart16550_lock;

    /* 控制台设备对象本身（dev->priv 指向上面那份状态）。*/
    mmio_device_t     uart_dev;
#endif

#if ARCH_X86_64
    /*
     * 每 VM 自己的 EPT：页表、RAM 窗口、按需页账本。
     * 与 aarch64 的 s2 / riscv 的 gstage 同义（三个架构各有一份 stage-2）。
     */
    ept_ctx_t ept;

    /*
     * 每 VM 的 vLAPIC（按 vcpu_id 索引）。
     *
     * ⚠️ 从前是 `static vlapic_t g_vlapic[MAX_VCPUS]`，按 vcpu_id 索引 ——
     * 而 vcpu_id 是每个 VM 内部从 0 开始的，于是两个 VM 的 vcpu0 指向同一份
     * LAPIC。见 include/vmm/vmm_vlapic.h 的说明。
     */
    vlapic_state_t vlapic[MAX_VCPUS];

    /*
     * 传统芯片组桩（PIC / IO-APIC / PIT / 端口 0x61）。
     *
     * ⚠️ 这些从前是 vmx.c 的文件级 static，整机一份 —— 第二个 VM 启动时
     * vmm_arch_vm_init() 里那段"复位设备桩"会把第一个 VM 的状态一起清掉。
     */
    x86_chipset_state_t chipset;
#endif

#if ARCH_RISCV64
    /*
     * 每 VM 自己的 G-stage：页表、VMID、RAM 窗口、按需页账本。
     * 与 aarch64 的 s2 字段同义（那边是 stage-2 / VTTBR_EL2，这边是
     * G-stage / hgatp）。
     */
    gstage_ctx_t gstage;

    /* 每 VM 一份 vPLIC（从前是 vplic.c 里的一个 g_vplic 全局）。*/
    vplic_state_t    vplic;
    spinlock_noirq_t vplic_lock;
    mmio_device_t    plic_dev;

    /*
     * hext_vcpu_setup() 给 guest 用的栈。
     *
     * 从前是 `g_guest_stack[MAX_VCPUS][4096]` —— 按 vcpu_id 索引，于是两个
     * VM 的 vcpu0 会共用同一块栈。这个函数目前没有调用者（guest_loader 路径
     * 自己设 vcpu 状态），但按 VM 分开才是它对多 VM 唯一安全的形态。
     */
#define VMM_GUEST_STACK_SIZE  4096
    uint8_t guest_stack[MAX_VCPUS][VMM_GUEST_STACK_SIZE];
#endif

    /*
     * MMIO 总线 —— 三个架构都有（设备型号不同，总线本身是同一样东西）。
     * 放守卫块外面：它们不是架构私有状态。
     */
    mmio_bus_t mmio_bus_storage;
    mmio_bus_t *mmio_bus;

#if ARCH_AARCH64
    /*
     * AArch64 的虚拟中断控制器 + 两个 MMIO 设备对象（dev->priv 指向上面的状态）。
     *
     * ⚠️ 这一块从前**没有守卫**，于是 x86/riscv 的 vm_t 里白扛一份 vgic_t
     *（含 dist_regs[4096]，6 KB 量级）和三个 mmio_device_t。它们只在
     * kernel/vmm/aarch64/ 下被引用，收进守卫是纯收益。
     *
     * vgic（GICv2）与 vgic3（GICv3）只会用到一个，由 GIC=v2|v3 编译期二选一，
     * 但两个字段都保留：kernel/vmm/aarch64/vm_init.c 里用 DRIVER_GIC_V3 分支。
     * GICv3 的结构体只在 GIC=v3 时才嵌入，避免 v2 路径白扛 ~68KB。
     */
    vgic_t vgic;
    mmio_device_t vgicd_dev;
    mmio_device_t vgicc_dev;
#if DRIVER_GIC_V3
    vgic3_t vgic3;
    mmio_device_t vgic3d_dev;
    mmio_device_t vgic3r_dev;
#endif
#endif /* ARCH_AARCH64 */
} vm_t;

/*
 * 池大小必须 <= 各架构静态表的槽位数 —— 那些表按 vm->slot 索引，
 * 越界就是踩到隔壁（或 BSS 之外）。放到这里而不是各架构文件里：
 * 两边都要看得见 MAX_VMS 才能断言。
 */
#if ARCH_AARCH64
_Static_assert(MAX_VMS <= STAGE2_MAX_VMS, "MAX_VMS > STAGE2_MAX_VMS");
#endif
#if ARCH_RISCV64
_Static_assert(MAX_VMS <= GSTAGE_MAX_VMS, "MAX_VMS > GSTAGE_MAX_VMS");
#endif
#if ARCH_X86_64
_Static_assert(MAX_VMS <= EPT_MAX_VMS, "MAX_VMS > EPT_MAX_VMS");
/* vlapic_state_t 数组按 MAX_VCPUS 定长，两边必须一致 */
_Static_assert(MAX_VCPUS <= VLAPIC_MAX_VCPUS, "MAX_VCPUS > VLAPIC_MAX_VCPUS");
#endif

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
 * 前四个由 vmm_run_vcpu（vcpu.c）在**每次进出 guest 时**调用；每个架构在
 * arch/vmx.c、el2_run.c、hext_run.c 中提供实现。
 *
 *   vmm_arch_restore_guest_ctx — 进入 guest 循环前恢复架构相关上下文
 *   vmm_arch_enter_guest       — 执行一次 guest 入口（eret/vmlaunch/vmresume）
 *                                返回 1 成功，0 入口失败
 *   vmm_arch_exit_handler      — 处理一次 VM exit，返回 EL2_* / VMX_* 状态码
 *   vmm_arch_save_guest_ctx    — guest 退出后保存架构相关上下文
 *
 * 第五个不是主循环钩子，而是**建 VM 时**的一次性钩子，由 vm_create()（vm.c）
 * 调用 —— 所以它没有和上面四个排在一起，单独列在下面。
 */
void vmm_arch_restore_guest_ctx(vcpu_t *vcpu);
int  vmm_arch_enter_guest(vcpu_t *vcpu);   /* 1=success, 0=entry-failed */
int  vmm_arch_exit_handler(vcpu_t *vcpu);
void vmm_arch_save_guest_ctx(vcpu_t *vcpu);

/*
 * vmm_arch_vm_init — 建 VM：stage-2、MMIO 总线、虚拟设备、vCPU 数组
 *
 * 注意它**跑在调用者的核上**（helper 模式下是 /bin/vmm-run 所在的核），
 * 而 vCPU 任务可能被摊到别的核 —— 凡是"每个逻辑处理器一份"的状态都不能
 * 在这里设，必须在每次进 guest 前重设（见 docs/bugfix/SMP_HELPER_MODE_BUGFIX.md）。
 */
int vmm_arch_vm_init(vm_t *vm);

/*
 * vmm_arch_vm_destroy — 拆 VM：与 vmm_arch_vm_init 严格逆序
 *
 * 设备状态搬到按 slot 索引的静态池之后，vm_free 里那句
 * `memset(vm, 0, sizeof(*vm))` **再也碰不到它们** —— 必须由这里显式归还。
 * 各设备自己的 destroy 必须幂等、且对"从未 init 过的槽"安全（回滚路径上
 * vm_free 会在 vm_create 失败时被调用）。
 */
void vmm_arch_vm_destroy(vm_t *vm);

/*
 * vmm_arch_irq_raise — 把一个设备的中断线接到本架构的投递机制上
 *
 * 这是「设备 → 线」的唯一出口。线的**含义由架构决定**（见 include/vmm/vmm_virq.h）：
 *   aarch64 → vmm_vgic{,3}_set_pending(&vm->vgic{,3}, vcpu_id, irq.line)
 *   riscv64 → vplic_set_pending(vm, irq.line)        （PLIC 没有 vcpu_id 这个维度）
 *   x86_64  → 查 guest 编的 IO-APIC 重定向表拿 vector → vlapic_raise_irq
 *
 * ⚠️ **三个实现里不允许出现任何使能/门控判据**。LAPIC 在 raise 那一刻就判
 * SVR 门控、GIC/PLIC 在投递时才判 enabled —— 这个差异是**可观测行为**
 * （控制器关闭期间的拉线是否被记住），抽接口时不许拍平。
 */
void vmm_arch_irq_raise(vcpu_t *vcpu, virq_t irq);


int vmm_run_vcpu(vcpu_t *vcpu);

/* 建一个 VM：按架构转发到 vmm_arch_vm_init()（见上面的钩子说明）。*/
int vm_create(vm_t *vm);

/* ── VM 池（kernel/vmm/vm.c）───────────────────────────────────
 *
 * 从前内核里只能有一个 VM；现在是一个静态池，见 vm.c 里那段注释。
 * **并发规则：跨任务只传 vmid，不传 vm_t *，也不做引用计数** —— 谁要操作
 * 某个 VM 就现场 vm_get(vmid) 取一次、用完即放。
 */
vm_t *vm_alloc(void);           /* 取一个空闲槽位（state=LOADING）；满了返回 NULL */
vm_t *vm_get(uint32_t vmid);    /* 按 vmid 查；不存在或已释放返回 NULL     */
void  vm_free(vm_t *vm);        /* **只能由该 VM 的 vCPU 任务自己调**      */
int   vm_count_used(void);      /* 池里非 FREE 的槽位数                    */

/* ── 宿主侧 guest 生命周期（/dev/vmm 用）──────────────────────── */
/*
 * vm_request_stop — 请求停止**指定** VM（非阻塞）
 *
 * vCPU 任务在它的下一个安全点（guest 退出路径的循环顶部）看到标志后返回，
 * 由 vcpu_task_fn 收尾（vm_free）。因为是异步的，调用后 guest 还要跑最多
 * 一个宿主 tick（满载约 10ms；空闲时每条 WFI 都是退出，几乎立刻）才真正
 * 结束 —— 想确认请查 vm_get(vmid)->state == VM_FREE。
 */
void vm_request_stop(vm_t *vm);

/* 是否有**任意** VM 在跑（单 VM 视角的粗判；精细判断用 vm_get(vmid)->state）。*/
int  vmm_guest_running(void);

struct task *vcpu_task_create(vcpu_t *vcpu, uint8_t priority);

#endif /* KERNEL_VMM_H */
