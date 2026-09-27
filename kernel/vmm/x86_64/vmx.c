/*
 * kernel/vmm/x86_64/vmx.c — Intel VMX 初始化、VMCS 管理与架构钩子
 *
 * 对标 AArch64 的 el2_run.c + stage2.c；实现 vmm.h 中声明的四个架构钩子。
 *
 * 简化设计（单核 QEMU，不使用 EPT）：
 *   - Guest 与 Host 共享 CR3（同一页表，identity map）
 *   - HLT → VMM 步进 RIP，yield，resume
 *   - VMCALL(rdi=0, HVC_DONE)   → VMM 正常退出
 *   - VMCALL(rdi=1, HVC_PRINT)  → VMM 打印迭代计数，yield，resume
 *   - 其余 exit → 打印诊断信息，返回 EL2_EXIT
 *
 * QEMU 启动需要 -enable-kvm -cpu host（KVM 硬件 VMX）。
 */

#include "vmm/vmm.h"
#include "pmm.h"        /* pmm_get_free_pages（缺页失败时的诊断）*/
#include "klog.h"
#include "string.h"
#include "task/task.h"
#include "task/cpu.h"   /* get_current_cpu_id()：VMXON 是按 CPU 记的 */
#include "x86_64/vmx.h"
#include "x86_64/ept.h"  /* EPT 二级翻译 */
#include "guest_loader.h"  /* GUEST_LINUX_*_GPA（guest 描述符表要用）*/
#include "vmm/vmm_mmio.h"    /* MMIO 总线 */
#include "vmm/vmm_uart16550.h"
#include "vmm/vmm_console.h" /* vmm_console_irq_asserted：控制台中断线（电平触发）*/
#include "vmm/vmm_vlapic.h"
#include "irq/lapic.h"   /* g_tsc_freq_hz：宿主标定出的 TSC 频率（CPUID 0x15/0x16 要报给 guest）*/

/* guest RAM 的宿主物理窗口（定义在 guest_boot.c）*/
#include "mm_vm.h"   /* virt_to_phys */

/* ── 静态存储（4KB 对齐）──────────────────────────────────── */

/* VMXON 区域（4KB，**每个逻辑处理器一块**）
 *
 * VMXON 区域是「该逻辑处理器在 VMX operation 期间使用」的，每个进 VMX
 * operation 的核都要有自己的一块（KVM / Xen 都按 per-CPU 分配）。
 *
 * 来历（2026-09-27 定位 SMP=2 helper 模式那个 #GP 时）：这块原来是全局单例，
 * 一度被当成根因 —— 真正的根因是 `IA32_FEATURE_CONTROL`（每 vCPU 一份，
 * 只在 helper 核上使能过，见 vmx_global_init 里的说明）。改成 per-CPU 之后
 * 症状照旧，才顺着 KVM 的 handle_vmon 找到真凶。**per-CPU 化本身仍然是对的**，
 * 只是它不是那次故障的原因 —— 别把这段历史当成「共用一块必挂」的证据。
 *
 * 对照：VMCS 那边本来就是 per-vCPU 的（g_vmcs_storage[MAX_VCPUS]）。*/
static uint8_t g_vmxon_region[CONFIG_SMP_CPUS][4096] __attribute__((aligned(4096)));

/* 每个 vCPU 的 VMCS（4KB）*/
static uint8_t g_vmcs_storage[MAX_VMS * MAX_VCPUS][4096]
    __attribute__((aligned(4096)));

/* 每个 vCPU 的 guest 栈（4KB）*/
static uint8_t g_guest_stack[MAX_VMS * MAX_VCPUS][4096]
    __attribute__((aligned(16)));

/*
 * vcpu → 引擎状态数组的下标
 *
 * 下面这些数组（VMCS / guest 栈 / MSR 加载表 / 诊断环形缓冲）从前按
 * **vcpu_id** 索引。而 vcpu_id 是**每个 VM 内部**从 0 开始的编号，于是两个
 * VM 的 vcpu0 指向同一块 VMCS：第二个 VM 一启动就把第一个 VM 的 VMCS 覆盖
 * 掉，症状是"guest 随机跑飞"这种最难反查的一类。
 *
 * 改成 (vm->slot, vcpu_id) 展平。vm 可能是 NULL（直接建 vcpu 的玩具路径），
 * 那时退回只用 vcpu_id —— 与改动前的行为一致。
 */
static inline uint32_t vcpu_slot(const vcpu_t *vcpu)
{
    return vcpu->vm ? (uint32_t)(vcpu->vm->slot * MAX_VCPUS + vcpu->vcpu_id)
                    : (uint32_t)vcpu->vcpu_id;
}

/* VMX capability（init 时从 MSR 读取）*/
static vmx_basic_t   g_vmx_basic;
static vmx_ctrl_msr_t g_pin_rev, g_cpu_rev[2], g_exi_rev, g_ent_rev;

static uint32_t g_ctrl_pin;
static uint32_t g_ctrl_cpu[2];
static uint32_t g_ctrl_exit;
static uint32_t g_ctrl_enter;


/* ================================================================
 * MSR / I/O bitmap
 *
 * 这两张表解决的是两类「不拦住就会直接打到真实硬件」的 guest 访问：
 *
 *   1. MSR bitmap（Intel SDM Vol 3C §25.6.9，4 KiB）
 *      [0x000,0x400) MSR 0x00000000-0x00001FFF 的**读**拦截位
 *      [0x400,0x800) 同区间的**写**拦截位
 *      [0x800,0xC00) MSR 0xC0000000-0xC0001FFF 的读拦截位
 *      [0xC00,0x1000) 同区间的写拦截位
 *      位=1 → 该 MSR 的该操作陷入 VMM。
 *
 *      不拦的后果很严重：Linux 一开机就写 STAR/LSTAR/SFMASK/EFER 建立
 *      syscall 环境，这些写会**落到真实 MSR** 上，把宿主自己的 syscall
 *      入口改掉。而且 VM-exit 只恢复 CR0/CR3/CR4/EFER，其余 MSR 不会
 *      自动还原 —— 宿主回不到干净状态。
 *
 *   2. I/O bitmap A/B（各 4 KiB，1 bit / 端口）
 *      A 覆盖端口 0x0000-0x7FFF，B 覆盖 0x8000-0xFFFF。
 *
 *      不拦的后果：guest 的 in/out **直接执行在真实端口上** ——
 *      0x3f8 是真的串口、0xcf8 是真的 PCI 配置口、0x70 是真的 CMOS。
 * ================================================================ */
static uint8_t g_msr_bitmap[4096] __attribute__((aligned(4096)));
static uint8_t g_io_bitmap_a[4096] __attribute__((aligned(4096)));
static uint8_t g_io_bitmap_b[4096] __attribute__((aligned(4096)));

static void vmx_bitmaps_init(void)
{
    /*
     * 两张表都**全拦截**。
     *
     * 全拦而不是按需拦，是为了让 guest 绝无可能写到真实硬件：未知的 MSR
     * 读回 0、写忽略（见 msr_emulate_*），未知端口按「无设备」处理。
     * 代价是每次 MSR/端口访问一次 VM exit —— 对 Linux 启动路径来说
     * 只有几千次，可以接受；等启动完再按需放行（白名单）留作后续优化。
     */
    memset(g_msr_bitmap, 0xFF, sizeof(g_msr_bitmap));
    memset(g_io_bitmap_a, 0xFF, sizeof(g_io_bitmap_a));
    memset(g_io_bitmap_b, 0xFF, sizeof(g_io_bitmap_b));
}

/* ================================================================
 * 事件注入：把被 VMM 拦下的中断/异常送回 guest
 *
 * VM-entry interruption-information（0x4016）是**唯一**通道。guest 当时
 * 屏蔽着中断（RFLAGS.IF=0）就不能硬投 —— 开了 CPU_INTR_WINDOW 让硬件在
 * 它开中断的瞬间(exit 7)再回来，那时才投。
 * ================================================================ */

static void vmx_inject_pending(vcpu_t *vcpu)
{
    uint64_t info = vcpu->pending_event;

    if (!info) {
        if (vcpu->intr_window) {   /* 事件已消散，撤掉窗口 */
            vcpu->intr_window = 0;
            vmcs_write(CPU_EXEC_CTRL0, g_ctrl_cpu[0] & ~CPU_INTR_WINDOW);
        }
        return;
    }

    /* 外部中断（含 LAPIC timer）要等 guest 自己允许中断 */
    if ((info & 0x700) == VMX_INTR_TYPE_EXTINT) {
        uint64_t rflags = vmcs_read(GUEST_RFLAGS);
        uint64_t istate = vmcs_read(GUEST_INTR_STATE);

        /*
         * ⚠️ 两个条件**缺一不可**，只看 IF 会被硬件打回（reason 33）：
         *     RFLAGS.IF = 1
         *     interruptibility state 的阻塞位全清
         *       bit0 STI 影子 / bit1 MOV SS 影子 / bit3 SMI
         * （bit2 是「NMI 阻塞」，只挡 NMI，这里不能一起判。）
         *
         * 实测踩过：guest 的 default_idle 执行 `sti; hlt`，而 hlt 恰好
         * 落在 STI 影子里 —— VM-exit 时硬件把 bit0=1 一并存进 VMCS
         * （exit 时 actv=0 intr_state=0x1）。我们只判 IF=1 就注入，
         * 于是**每一次** VM-entry 都被拒，guest 永远停在 idle。
         * 正确做法和 IF=0 时一样：挂起 + 开 interrupt-window，
         * 等影子被下一条指令清掉，硬件再用 exit 7 把我们叫回来。
         */
        if (!(rflags & 0x200) || (istate & (0x1 | 0x2 | 0x8))) {
            if (!vcpu->intr_window) {
                vcpu->intr_window = 1;
                vmcs_write(CPU_EXEC_CTRL0, g_ctrl_cpu[0] | CPU_INTR_WINDOW);
            }
            return;
        }
    }

    /* 记进 vLAPIC 的 ISR：guest 处理完写 EOI 时要能退掉它 */
    if ((info & 0x700) == VMX_INTR_TYPE_EXTINT)
        vlapic_accept_interrupt(vcpu->vm, (uint32_t)(info & 0xff), 0);

    vmcs_write(VM_ENTRY_INTR_INFO, info);
    vmcs_write(VM_ENTRY_EXC_ERRCODE,
               (info & VMX_INTR_ERRCODE_VALID) ? vcpu->pending_errcode : 0);
    vmcs_write(VM_ENTRY_INST_LEN, 0);

    vcpu->pending_event   = 0;
    vcpu->pending_errcode = 0;
    if (vcpu->intr_window) {
        vcpu->intr_window = 0;
        vmcs_write(CPU_EXEC_CTRL0, g_ctrl_cpu[0] & ~CPU_INTR_WINDOW);
    }
}


/* ================================================================
 * MSR 模拟
 *
 * 因为 MSR bitmap 全拦截，guest 的每次 rdmsr/wrmsr 都到这里。
 * 「认得的」走 vcpu 里的影子，「不认得的」读回 0、写忽略 —— 对
 * Linux 来说等价于「这台机器没有那个特性」，比让它看见宿主的值安全得多。
 * ================================================================ */
/* ================================================================
 * MSR load/save list：让 guest 的 syscall MSR 在进出时自动装卸
 *
 * 为什么必须做：MSR bitmap 只拦**显式**的 rdmsr/wrmsr。guest 执行
 * `syscall` 指令时，CPU 用的是**真实的** IA32_LSTAR —— 不做处理的话，
 * guest 用户态一发系统调用就直接跳进**宿主内核的 syscall 入口**，
 * 在宿主的地址空间里跑 guest 的寄存器。
 *
 * 做法是 VMCS 的 MSR 列表（VM-entry load / VM-exit load）：
 * 硬件在进入 guest 前把这些 MSR 装成 guest 的值，退出时再装回宿主的。
 * 全程在硬件里原子完成，没有「退出到 C 代码之前宿主还在用 guest MSR」
 * 的窗口（自己用 wrmsr 换就会留这个窗口，中断打进来就崩）。
 *
 * guest 侧的值由 msr_emulate_write 维护（写是被拦下来的）—— 对 syscall 那
 * 几个 MSR（STAR/LSTAR/CSTAR/SFMASK/PAT）这足够了，**但 GS 那一对不够**：
 * `swapgs` 指令直接在硬件上换 GS_BASE/KERNEL_GS_BASE，VMM 看不到，必须靠
 * VM-exit 的 store 表把值捞回来（见 vmx_msr_lists_init 的说明）。
 * ================================================================ */
#define VMM_MSR_COUNT 6

typedef struct {
    uint32_t idx;
    uint32_t rsvd;
    uint64_t val;
} vmx_msr_entry_t;

static vmx_msr_entry_t g_msr_guest[MAX_VMS * MAX_VCPUS][VMM_MSR_COUNT]
    __attribute__((aligned(16)));
static vmx_msr_entry_t g_msr_host[MAX_VMS * MAX_VCPUS][VMM_MSR_COUNT]
    __attribute__((aligned(16)));

/* VM-exit MSR store 区**必须与 entry-load 区分开**（KVM 同此）：
 * 两块区域重叠时 VM-entry 会以 reason 34（MSR loading）失败 —— 实测就是
 * "Unhandled exit reason=34 rip=0x1000000"。这里只放需要往返的两条。*/
static vmx_msr_entry_t g_msr_store[MAX_VMS * MAX_VCPUS][2]
    __attribute__((aligned(16)));

/*
 * ⚠️ GS 这一对（GS_BASE / KERNEL_GS_BASE）**必须成对出现**，而且必须同时有
 * **exit store 表**（见 vmx_msr_lists_init）。
 *
 * 原因：guest 的 `swapgs` 指令直接在硬件上交换这两个 MSR，VMM 完全看不见；
 * 而 VM-entry 每次都会按 load 表把两半**装回旧值**。只 load 不 store 的话，
 * `swapgs` 等于没执行 —— 而 Linux 的 `paranoid_entry` 恰好靠
 * `rdmsr MSR_GS_BASE`（`SAVE_AND_SET_GSBASE`）判断"现在在内核 GS 还是用户
 * GS"，读到的是过期影子就判断错，随后 `%gs:` 取到错误的 per-CPU 基址、异常，
 * 异常又走同一条 paranoid 入口 —— **死循环，且循环里唯一陷入 VMM 的只有那条
 * rdmsr**（实测：rip 恒为 paranoid_entry+0x93、exit reason 恒为 31/RDMSR、
 * 12 万次采样一动不动，guest 停在 `Run /init as init process`）。
 *
 * 做法与 KVM 相同：另开一块 **exit-store 区**（g_msr_store）在退出时把
 * guest 真实的值捞回内存，下一次入口再分别装回去 —— GS_BASE 走
 * GUEST_BASE_GS 字段、KERNEL_GS_BASE 走 load 表，见
 * vmx_refresh_host_state()。
 */
static const uint32_t g_msr_list[VMM_MSR_COUNT] = {
    MSR_STAR, MSR_LSTAR, MSR_CSTAR, MSR_SYSCALL_MASK,
    MSR_KERNEL_GS_BASE, MSR_IA32_PAT,
};

#define VMM_HOST_MSR_COUNT VMM_MSR_COUNT

/* exit-store 表里的两条（顺序即 g_msr_store 的下标）*/
static const uint32_t g_msr_store_list[2] = { MSR_GS_BASE, MSR_KERNEL_GS_BASE };

/* MSR 在 g_msr_list 里的下标（不在表里返回 -1）*/
static int vmx_msr_index(uint32_t msr)
{
    for (int i = 0; i < VMM_MSR_COUNT; i++) {
        if (g_msr_list[i] == msr)
            return i;
    }
    return -1;
}

/* guest 写了某个被拦截的 MSR 之后，同步进 VM-entry 的装填区 */
static void vmx_msr_sync_guest(vcpu_t *vcpu, uint32_t idx, uint64_t val)
{
    int i = vmx_msr_index(idx);

    if (i >= 0)
        g_msr_guest[vcpu_slot(vcpu)][i].val = val;
}

static void vmx_msr_lists_init(vcpu_t *vcpu)
{
    int id = (int)vcpu_slot(vcpu);
    const uint64_t guest_init[VMM_MSR_COUNT] = {
        vcpu->msr_star, vcpu->msr_lstar, vcpu->msr_cstar,
        vcpu->msr_sfmask, vcpu->msr_kernel_gs_base, vcpu->msr_pat,
    };

    for (int i = 0; i < VMM_MSR_COUNT; i++) {
        g_msr_guest[id][i].idx  = g_msr_list[i];
        g_msr_guest[id][i].rsvd = 0;
        g_msr_guest[id][i].val  = guest_init[i];

        g_msr_host[id][i].idx   = g_msr_list[i];
        g_msr_host[id][i].rsvd  = 0;
        g_msr_host[id][i].val   = vmx_rdmsr(g_msr_list[i]);
    }

    vmcs_write(VM_ENTRY_MSR_LOAD_COUNT, VMM_MSR_COUNT);
    vmcs_write(VM_ENTRY_MSR_LOAD_ADDR,  virt_to_phys(g_msr_guest[id]));

    /*
     * exit-store 区：**独立缓冲**，只放需要往返回来的 GS 两兄弟。
     * ⚠️ 不能与 entry-load 区（g_msr_guest）复用同一块内存 —— 重叠时
     * VM-entry 直接以 reason 34（MSR loading）失败（实测
     * "Unhandled exit reason=34 rip=0x1000000"）。
     */
    for (int i = 0; i < 2; i++) {
        g_msr_store[id][i].idx  = g_msr_store_list[i];
        g_msr_store[id][i].rsvd = 0;
        g_msr_store[id][i].val  = (i == 0) ? vcpu->msr_gs_base
                                           : vcpu->msr_kernel_gs_base;
    }
    vmcs_write(VM_EXIT_MSR_STORE_COUNT, 2);
    vmcs_write(VM_EXIT_MSR_STORE_ADDR,  virt_to_phys(g_msr_store[id]));
    vmcs_write(VM_EXIT_MSR_LOAD_COUNT,  VMM_HOST_MSR_COUNT);
    vmcs_write(VM_EXIT_MSR_LOAD_ADDR,   virt_to_phys(g_msr_host[id]));

}

static int msr_emulate_read(vcpu_t *vcpu, uint32_t msr, uint64_t *val)
{
    switch (msr) {
    case MSR_EFER:            *val = vcpu->msr_efer;            return 1;
    case MSR_STAR:            *val = vcpu->msr_star;            return 1;
    case MSR_LSTAR:           *val = vcpu->msr_lstar;           return 1;
    case MSR_CSTAR:           *val = vcpu->msr_cstar;           return 1;
    case MSR_SYSCALL_MASK:    *val = vcpu->msr_sfmask;          return 1;
    case MSR_FS_BASE:         *val = vcpu->msr_fs_base;         return 1;
    /*
     * ⚠️ GS 这一对**要读硬件维护的那份**（exit-store 写回的 g_msr_guest），
     * 不能读 vcpu 里的影子：影子只在显式 wrmsr 时更新，而 guest 的 `swapgs`
     * 直接在硬件上交换这两个 MSR，我们看不见。Linux 的 paranoid_entry 靠
     * `rdmsr MSR_GS_BASE` 判断自己在哪个 GS，给它过期影子就会判错 → 死循环。
     */
    case MSR_GS_BASE:
        *val = g_msr_store[vcpu_slot(vcpu)][0].val;
        return 1;
    case MSR_KERNEL_GS_BASE:
        *val = g_msr_store[vcpu_slot(vcpu)][1].val;
        return 1;
    case MSR_IA32_PAT:        *val = vcpu->msr_pat;             return 1;
    case MSR_IA32_APIC_BASE:  *val = vcpu->apic_base;           return 1;
    default:
        *val = 0;   /* 未知 MSR：当作「没有这个特性」 */
        return 0;
    }
}

static int msr_emulate_write_1(vcpu_t *vcpu, uint32_t msr, uint64_t val)
{
    switch (msr) {
    case MSR_EFER:
        /* Linux 会设 SCE(0)/NXE(11)/LME(8)，也用它切 LMA —— LMA 只读，
         * 由硬件在 VM entry 时按 EFER.LME 决定，这里只存 LME/NXE/SCE。*/
        vcpu->msr_efer = val & ~(1ULL << 10);
        vmcs_write(GUEST_EFER, vcpu->msr_efer);
        return 1;
    case MSR_STAR:            vcpu->msr_star           = val; return 1;
    case MSR_LSTAR:           vcpu->msr_lstar          = val; return 1;
    case MSR_CSTAR:           vcpu->msr_cstar          = val; return 1;
    case MSR_SYSCALL_MASK:    vcpu->msr_sfmask         = val; return 1;
    case MSR_FS_BASE:
        /*
         * ⚠️ FS/GS base 不只是「影子」——它们同时是 **VMCS 字段**
         * （GUEST_BASE_FS/GS），硬件在 VM-entry 时从这里装进寄存器。
         * 只存影子不写 VMCS 的话，guest 写的 base 永远不会生效。
         *
         * FS 这条曾经漏掉（GS 修了、FS 没修，见下面那段注释），
         * 症状极具迷惑性：内核**整个启动过程都正常**，一直到
         *     Run /init as init process
         *     init[1]: segfault at 0 ip ... error 4
         *     Kernel panic - Attempted to kill init!
         * 才崩。因为 glibc/busybox 取 TLS 就一条
         *     mov %fs:0x0,%rax
         * FS base 是 0 ⇒ 访问地址 0 ⇒ 用户态缺页。
         * 内核态不用 FS，所以前面 5 秒谁都没露馅。
         */
        vcpu->msr_fs_base = val;
        vmcs_write(GUEST_BASE_FS, val);
        return 1;
    case MSR_GS_BASE:
        /* 同上：Linux 把 per-CPU 基址写进 GS_BASE，之后所有 %gs 相对访问
         * 都要求 VMCS 里的 GUEST_BASE_GS 同步 —— 漏写的实测症状是早期
         * 启动里 cr2=0 的缺页 + 异常风暴（4000 次 exit 里 3990 次异常）。*/
        vcpu->msr_gs_base = val;
        vmcs_write(GUEST_BASE_GS, val);
        g_msr_store[vcpu_slot(vcpu)][0].val = val; /* 与 exit-store 同一份 */
        return 1;
    case MSR_KERNEL_GS_BASE:
        vcpu->msr_kernel_gs_base = val;
        g_msr_store[vcpu_slot(vcpu)][1].val = val;
        return 1;
    case MSR_IA32_PAT:        vcpu->msr_pat            = val; return 1;
    case MSR_IA32_APIC_BASE:
        /* vLAPIC 的开关：EN(11) 决定 APIC 是否工作，EXTD(10) 是 x2APIC。
         * 第一版只做 xAPIC，所以 EXTD 一律清掉 —— Linux 探测到
         *「x2APIC 不可用」就会安心走 MMIO 路径。*/
        vcpu->apic_base = val & ~APIC_BASE_X2APIC;
        vlapic_set_apic_base(vcpu->vm, vcpu->apic_base);
        return 1;
    default:
        return 0;   /* 未知 MSR：写忽略 */
    }
}


/* ================================================================
 * CPUID 模拟
 *
 * 不模拟的后果：guest 看到的是**宿主的 CPU** —— 包括 AVX/AVX512（我们没有
 * 保存 XSAVE 状态，宿主也没开 FPU 上下文切换）、VMX 位（guest 会试图嵌套）、
 * 以及宿主的核数拓扑。这里报一个保守但自洽的 CPU：SSE2 级别、单核、
 * **没有** XSAVE/AVX/VMX，但有 APIC/TSC/PAE/NX/SYSCALL。
 * ================================================================ */
static void vmx_cpuid_emulate(uint32_t leaf, uint32_t sub,
                              uint32_t *eax, uint32_t *ebx,
                              uint32_t *ecx, uint32_t *edx)
{
    (void)sub;
    *eax = *ebx = *ecx = *edx = 0;

    switch (leaf) {
    case 0x00000000:
        *ebx = 0x756e6547;   /* "Genu" */
        *edx = 0x49656e69;   /* "ineI" */
        *ecx = 0x6c65746e;   /* "ntel" */
        /*
         * 最高基础叶 = 0x16。
         * ⚠️ 这个值必须 ≥ 0x15，否则 native_calibrate_tsc() 直接
         * `if (boot_cpu_data.cpuid_level < 0x15) return 0;` 出局，
         * 内核掉进 quick_pit_calibrate() —— 那个循环 latch + 轮询
         * PIT 通道 2 的高字节，而我们的 8254 桩永远返回写入值，
         * 于是跑满 50000 次 × 5 个端口 exit 才失败（看着像死机）。
         * 仍然不暴露 7/0xB/0xD（那三个会给内核错误的 cache/topology 视图）。
         */
        *eax = 0x00000016;
        return;

    case 0x00000001:
        *eax = 0x000006f2;   /* family 6 / model 6 / stepping 2 */
        /* EDX：FPU PSE TSC MSR PAE CX8 APIC SEP PGE CMOV PAT CLFSH
         *      MMX FXSR SSE SSE2  —— 只放行这些 */
        *edx = (1u << 0)  | (1u << 3)  | (1u << 4)  | (1u << 5)
             | (1u << 6)  | (1u << 8)  | (1u << 9)  | (1u << 11)
             | (1u << 13) | (1u << 15) | (1u << 16) | (1u << 19)
             | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 26);
        /* ECX 全 0：**特别**是 VMX(5)、XSAVE(26)、OSXSAVE(27)、AVX(28) */
        *ecx = 0;
        /*
         * ⚠️ EBX 也必须显式写！它的 [31:24] 是**初始 APIC ID**。
         * 漏写会让内核读到未初始化的垃圾（实测读成 129/0x81），
         * 于是 smpboot 报 "Boot CPU (id 129) not listed by BIOS"，
         * CPU/中断拓扑错乱 ⇒ 后期 "Attempted to kill the idle task!" panic。
         * [15:8] = CLFLUSH 行大小（8），[7:0] = brand index（0）。
         */
        *ebx = (8u << 8);
        return;

    /*
     * ── 0x15 / 0x16：TSC 频率 ──
     *
     * guest 跑在**宿主的 TSC** 上（嵌套 VMX，我们没写 TSC_OFFSET），
     * 所以「宿主的 TSC 频率」就是 guest 的正确频率。
     *
     * 先试透传宿主的真值：裸机跑 avatar 时那是**精确值**（本机物理
     * CPUID 0x15 给 eax=2 ebx=126 ecx=38400000 ⇒ 38400*126/2 =
     * 2419200 kHz）。但实测 **QEMU/KVM 会把 0x15/0x16 抹成全 0**，
     * 而 avatar 平时就是跑在 QEMU 里的 —— 所以这条路基本走不通，
     * 必须能自己合成。
     *
     * 合成方案：把晶体**正好**报成 1 GHz，比例取 tsc/1e6。
     *     crystal_khz = ecx/1000            = 1000000
     *     tsc_khz     = crystal_khz * ebx/eax
     *                 = 1000000 * (tsc_khz/1e6) / 1000 = tsc_khz ✓
     * （分子 1000000*2417 = 2.417e9 仍在 32 位无符号内，不会溢出。）
     *
     * ⚠️ 为什么晶体一定要 **1 GHz** —— 这不是随便挑的：
     * Linux 由它反推 LAPIC timer 频率：
     *     lapic_timer_period = crystal_khz * 1000 / HZ
     * （它假定 LAPIC timer 就跑在晶体上），而我们的 vLAPIC 模型是
     * 「1 count = 1 ns」= **1 GHz**（见 vlapic.c 的 timer_start）。
     * 之前报 2.417 GHz，两边差 2.4 倍，guest 每个 tick 都被拉长，
     * 于是 clocksource watchdog 报 skew 过大、把 tsc-early 判成 unstable。
     */
    case 0x00000015: {
        uint32_t a = 0, b = 0, c = 0, d = 0;
        uint64_t mhz;

        vmx_cpuid(0x15, &a, &b, &c, &d);
        if (a == 0 || b == 0) {          /* 宿主不报（QEMU/KVM 全是 0）*/
            mhz = g_tsc_freq_hz / 1000000ULL;
            if (g_tsc_freq_hz == 0 || mhz == 0)
                return;                  /* 自己也没标定出来：保持全 0 */
            a = 1000;                    /* 分母：比例 = mhz/1000 */
            b = (uint32_t)mhz;           /* 分子 */
            c = 1000000000u;             /* 晶体 = 1 GHz（对齐 vLAPIC 模型）*/
            d = 0;
        }
        *eax = a; *ebx = b; *ecx = c; *edx = d;
        return;
    }

    case 0x00000016: {
        uint32_t a = 0, b = 0, c = 0, d = 0;

        vmx_cpuid(0x16, &a, &b, &c, &d);
        if (a == 0) {
            if (g_tsc_freq_hz == 0)
                return;
            a = (uint32_t)(g_tsc_freq_hz / 1000000ULL);   /* 基准频率 MHz */
            b = c = d = 0;
        }
        *eax = a; *ebx = b; *ecx = c; *edx = d;
        return;
    }

    case 0x80000000:
        *eax = 0x80000001;   /* 最高扩展叶 */
        return;

    case 0x80000001:
        /*
         * ⚠️ CPUID.80000001H 的位分布很容易记错：
         *   EAX = 扩展 family/model（这里给 0）
         *   EDX = SYSCALL/SYSRET(11) | NX(20) | **LM(29)**
         * **LM（长模式）在 EDX bit29，不是 EAX** —— 曾经写成
         * `*eax = (1u<<29)`，结果 guest 看到的 CPU「不支持长模式」，
         * 内核 `startup_32` 里的 `verify_cpu` 检查不过，直接跳进
         * `hlt` 死循环（实测 235 万次 exit reason=12、RIP 原地不动、
         * 串口一个字都没有），非常难从现象反推。
         */
        *eax = 0;
        *edx = (1u << 11)    /* SYSCALL/SYSRET */
             | (1u << 20)    /* NX            */
             | (1u << 26)    /* PDPE1GB：1 GiB 大页。**必须给** —— 缺了它
                              * 内核会走另一条直接映射路径（实测：裸跑打印
                              * "Using GB pages for direct mapping"，
                              *  我们这里没有），随后在自己声明为 usable 的
                              * 物理地址上缺页（CR2=ffff888003200000）。*/
             | (1u << 29);   /* **LM 长模式** */
        return;

    default:
        return;   /* 其余叶一律全 0 */
    }
}

/*
 * 写路径的统一入口：先走模拟，成功的话把新值同步进 VM-entry 的 MSR
 * 装填区 —— guest 会把 LSTAR/STAR/SFMASK 改成自己的 syscall 入口，
 * 下次进 guest 时硬件要装的是**新值**；不同步的话 guest 的 syscall
 * 会一直跳回宿主入口（MSR load list 存在的意义就在这里）。
 */
static int msr_emulate_write(vcpu_t *vcpu, uint32_t msr, uint64_t val)
{
    int known = msr_emulate_write_1(vcpu, msr, val);

    if (known)
        vmx_msr_sync_guest(vcpu, msr, val);
    return known;
}

/* ================================================================
 * guest MMIO 指令解码
 *
 * x86 的 MMIO 数据在**通用寄存器**里，必须解码出错指令才能知道
 * 「哪个寄存器 + 多宽」。这里实现 MMIO 场景实际会用到的子集：
 *   - 前缀扫描：0x66(opsize) / 0x67(addrsize) / REX / 段前缀 / lock
 *   - MOV r, r/m  (0x8A/0x8B 读；0x88/0x89 写)
 *   - MOV r/m, imm (0xC6/0xC7 写)
 * 其余 opcode 返回失败，由调用方按「无法解码」处理并打印诊断。
 *
 * 与 riscv 的实现不同，x86 的 ModRM/SIB/disp 需要完整走一遍才能定位
 * 操作数宽度，因此这里解析到「寄存器号 + 访问宽度」即停。
 * ================================================================ */

/*
 * guest 物理地址 → 本核可以直接读的宿主指针（**查表**，不是算术换算）
 *
 * ⚠️ 按需分页之后 GPA 与 HPA 之间不再有固定偏移 —— 每一页都是各自从 PMM
 * 分配的。老版本那种 `phys_to_virt(x86_guest_hpa_base() + gpa)` 会读到完全
 * 无关的物理页（可能属于别的 VM 或宿主内核），而且**没有任何报错**。
 *
 * 返回 NULL 表示这一页还没被 guest 碰过（按需分页下未映射 = 从没访问）。
 * 调用方必须判 NULL —— 这几处都是诊断代码，跳过即可，不要当成功继续用。
 */
static void *gpa_to_host(const vcpu_t *vcpu, uint64_t gpa)
{
    uint64_t hpa;

    if (!vcpu->vm || !x86_ept_lookup(&vcpu->vm->ept, gpa, &hpa))
        return NULL;
    return phys_to_virt(hpa);
}

/* 本 VM 的 guest RAM 窗口大小（诊断里当越界判据用）。*/
static uint64_t guest_mem_size(const vcpu_t *vcpu)
{
    return vcpu->vm ? vcpu->vm->cfg.mem_size : 0;
}

/* 从 guest 内存取字节（经 EPT 翻译 GPA→HPA→内核直接映射）*/
static int guest_fetch_bytes(const vcpu_t *vcpu, uint64_t gpa, uint8_t *buf,
                             uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        void *p = gpa_to_host(vcpu, gpa + i);
        if (!p)
            return 0;
        buf[i] = *(volatile uint8_t *)p;
    }
    return 1;
}

/* 读 guest 的一个 64 位页表项（GPA → HPA → 直接映射）*/
static int guest_read_u64(const vcpu_t *vcpu, uint64_t gpa, uint64_t *out)
{
    void *p = gpa_to_host(vcpu, gpa);

    if (!p)
        return 0;
    *out = *(volatile uint64_t *)p;
    return 1;
}

/*
 * guest 虚拟地址 → guest 物理地址。
 *
 * ⚠️ 这一步**必须有**。guest 进内核态后 RIP 是内核**虚拟**地址
 * （实测 0xffffffff810608c6），拿它直接当 GPA 去查 EPT 必然失败，
 * 于是「取指令」失败 → 解码失败 → MMIO 读**不回写目标寄存器**，
 * guest 拿到的只是上一条指令的遗留值。症状极具迷惑性，实测：
 *     native_apic_mem_read(APIC_VERSION) 读出 0xc0
 *     native_apic_mem_read(APIC_ID)       读出 129
 * 于是内核打印 "BIOS bug: APIC version mismatch, boot CPU: c0"、
 * 把 CPU 拓扑判错成 flat/1 CPU —— 看着像 vLAPIC 坏了，其实是解码器
 * 根本没读到指令。解码器原先只在解压器阶段用过，那时 guest 恒等映射
 * （VA==PA），所以这个坑一直没暴露。
 *
 * 覆盖两档：
 *   - CR0.PG=0（解压器早期 / 实模式尾巴）：恒等
 *   - 4 级页表（EFER.LMA + CR4.PAE）：PML4→PDPT→PD→PT 真走一遍
 * 其余（32 位 PAE / 2 级）按恒等兜底 —— guest 跑到 MMIO 密集期时
 * 早就是 4 级页表了。
 */
static int guest_va_to_gpa(const vcpu_t *vcpu, uint64_t va, uint64_t *gpa)
{
    uint64_t cr0  = vmcs_read(GUEST_CR0);
    uint64_t cr4  = vmcs_read(GUEST_CR4);
    uint64_t efer = vmcs_read(GUEST_EFER);
    uint64_t tbl, e;
    int      lvl;

    if (!(cr0 & X86_CR0_PG) ||
        !(cr4 & X86_CR4_PAE) || !(efer & 0x100ULL)) {
        *gpa = va;                       /* 未开分页 / 非 4 级：恒等兜底 */
        return 1;
    }

    tbl = vmcs_read(GUEST_CR3) & ~0xFFFULL;
    for (lvl = 0; lvl < 4; lvl++) {
        uint64_t idx = (va >> (12 + 9 * (3 - lvl))) & 0x1FF;

        if (!guest_read_u64(vcpu, tbl + idx * 8, &e))
            return 0;
        if (!(e & 1))
            return 0;                    /* P=0：这一级没映射 */
        if (lvl == 1 && (e & 0x80)) {    /* PDPT 项指向 1 GiB 大页 */
            *gpa = (e & ~((1ULL << 30) - 1)) | (va & ((1ULL << 30) - 1));
            return 1;
        }
        if (lvl == 2 && (e & 0x80)) {    /* PD 项指向 2 MiB 大页 */
            *gpa = (e & ~((1ULL << 21) - 1)) | (va & ((1ULL << 21) - 1));
            return 1;
        }
        tbl = e & ~0xFFFULL;
    }
    *gpa = tbl | (va & 0xFFF);
    return 1;
}

/*
 * 取**指令**字节：rip 是虚拟地址，先翻译成 GPA 再读。
 * x86 保证指令不跨页，所以读满本页剩余部分就够；页尾之后补 0，
 * 免得解码器读到未初始化的栈。
 */
static int guest_fetch_code(const vcpu_t *vcpu, uint64_t va, uint8_t *buf,
                            uint32_t n)
{
    uint64_t gpa;
    uint32_t room, got;

    if (!guest_va_to_gpa(vcpu, va, &gpa))
        return 0;

    room = 0x1000u - (uint32_t)(va & 0xFFFu);
    got  = (n < room) ? n : room;
    if (!guest_fetch_bytes(vcpu, gpa, buf, got))
        return 0;
    for (uint32_t i = got; i < n; i++)
        buf[i] = 0;
    return 1;
}

/* 解码结果
 *
 * ⚠️ reg 用的就是 x86_gpr_ptr() 的编号：**0 就是 RAX**，不是「没有寄存器」
 * 的哨兵 —— 有没有源/目标寄存器一律看 has_reg。
 *
 * 曾经用 `reg != 0` 当判据（想表达「立即数写没有源寄存器」），于是凡是
 * 数据来自 %eax/%rax 的 MMIO 写都被当成立即数 0 写进设备：实测 guest 往
 * IO-APIC 重定向表写的那些表项（编译器恰好用 RAX 传值）全被吞成 0，
 * 内核看到的表项状态和它自己写的对不上，串口的 IRQ 探测因此永远失败。
 */
typedef struct {
    int      is_write;
    uint8_t  size;       /* 访问字节数 */
    uint8_t  has_reg;    /* reg 有效？（0xC6/0xC7 立即数写为 0）*/
    uint32_t reg;        /* GPR 号（0-15，0 = RAX）*/
    uint64_t inst_len;   /* 指令总长（供步进 RIP）*/
    uint64_t imm;        /* 立即数（0xC6/0xC7 写用）*/
} x86_mmio_access_t;

/*
 * 解析 ModRM 及其后续（SIB / disp），返回操作数是否「使用寄存器直接寻址」。
 * MMIO 场景下操作数是内存操作数（mod != 3），我们只关心它用了哪个寄存器
 * 作为数据源/目标（reg 字段），以及指令总长度。
 */
static int parse_modrm(const uint8_t *p, uint32_t n,
                       uint32_t *reg_field, uint32_t *bytes_consumed)
{
    uint8_t modrm;
    uint32_t used = 1;

    if (n < 1)
        return 0;
    modrm = p[0];

    uint32_t mod = (modrm >> 6) & 3;
    uint32_t rm  = modrm & 7;

    *reg_field = (modrm >> 3) & 7;

    if (mod != 3) {
        /* 内存操作数：SIB + 位移 */
        if (rm == 4) {                 /* SIB */
            if (n < 2) return 0;
            uint32_t base = p[1] & 7;
            used++;
            if (mod == 0 && base == 5)
                used += 4;             /* disp32 */
            else if (mod == 1)
                used += 1;             /* disp8  */
            else if (mod == 2)
                used += 4;             /* disp32 */
        } else if (mod == 0 && rm == 5) {
            used += 4;                 /* RIP 相对 disp32 */
        } else if (mod == 1) {
            used += 1;
        } else if (mod == 2) {
            used += 4;
        }
    }

    if (used > n)
        return 0;
    *bytes_consumed = used;
    return 1;
}

static int decode_mmio_access(const vcpu_t *vcpu, uint64_t rip_va,
                              x86_mmio_access_t *acc)
{
    uint8_t  buf[24];
    uint32_t i = 0;
    int      rex_w = 0, rex_r = 0, opsize16 = 0;

    if (!guest_fetch_code(vcpu, rip_va, buf, sizeof(buf)))
        return 0;

    /* ── 前缀扫描 ── */
    for (; i < sizeof(buf); i++) {
        uint8_t b = buf[i];
        if (b == 0x66) { opsize16 = 1; continue; }        /* 操作数宽度 */
        if (b == 0x67) { continue; }                      /* 地址宽度（忽略）*/
        if (b == 0xF0 || b == 0xF2 || b == 0xF3) continue;/* lock/rep */
        if (b == 0x2E || b == 0x36 || b == 0x3E ||
            b == 0x26 || b == 0x64 || b == 0x65) continue;/* 段前缀 */
        if (b >= 0x40 && b <= 0x4F) {                     /* REX */
            rex_w = (b >> 3) & 1;
            rex_r = (b >> 2) & 1;                         /* 扩展 reg 字段：r8-r15 */
            continue;
        }
        break;                                            /* opcode */
    }
    if (i >= sizeof(buf))
        return 0;

    uint8_t op = buf[i];
    i++;

    uint32_t reg_field = 0, modrm_used = 0;

    switch (op) {
    case 0x88: case 0x89: {          /* MOV r/m, r  → 写 */
        if (!parse_modrm(buf + i, (uint32_t)(sizeof(buf) - i),
                         &reg_field, &modrm_used))
            return 0;
        acc->is_write = 1;
        acc->size = (op == 0x88) ? 1 : (opsize16 ? 2 : (rex_w ? 8 : 4));
        /* 源寄存器在 reg 字段，带 REX.R（r8-r15）；0 是 RAX，不是空值 */
        acc->has_reg = 1;
        acc->reg  = reg_field | (rex_r ? 8u : 0u);
        acc->inst_len = i + modrm_used;
        return 1;
    }
    case 0x8A: case 0x8B: {          /* MOV r, r/m  → 读 */
        if (!parse_modrm(buf + i, (uint32_t)(sizeof(buf) - i),
                         &reg_field, &modrm_used))
            return 0;
        acc->is_write = 0;
        acc->size = (op == 0x8A) ? 1 : (opsize16 ? 2 : (rex_w ? 8 : 4));
        /* 读方向：目标寄存器在 reg 字段，同样要带 REX.R
         * （原来错写成 rex_w —— 读进 %r8..%r15 的数据会落到 %rax..%rdi）*/
        acc->has_reg = 1;
        acc->reg  = reg_field | (rex_r ? 8u : 0u);
        acc->inst_len = i + modrm_used;
        return 1;
    }
    case 0xC6: case 0xC7: {          /* MOV r/m, imm → 写 */
        if (!parse_modrm(buf + i, (uint32_t)(sizeof(buf) - i),
                         &reg_field, &modrm_used))
            return 0;
        uint32_t off = i + modrm_used;
        uint32_t imm_size = (op == 0xC6) ? 1 : (opsize16 ? 2 : 4);
        uint64_t imm = 0;
        if (off + imm_size > sizeof(buf))
            return 0;
        for (uint32_t k = 0; k < imm_size; k++)
            imm |= (uint64_t)buf[off + k] << (8 * k);

        acc->is_write = 1;
        acc->size = imm_size;
        acc->has_reg = 0;            /* 立即数写：无源寄存器 */
        acc->imm  = imm;
        acc->inst_len = off + imm_size;
        return 1;
    }
    default:
        return 0;                   /* 不支持的 opcode */
    }
}

/*
 * 校验解码出的方向/宽度与 EPT qualification 是否自洽。
 * qualification 是硬件给出的权威信息；解码结果不一致说明我们认错了指令，
 * 此时宁可不解码也不要按错误宽度去访问设备。
 */
static int decode_matches_qual(uint64_t qual, const x86_mmio_access_t *acc)
{
    int qual_write = (int)((qual >> 1) & 1);

    if (qual_write != acc->is_write)
        return 0;
    return 1;
}

/* 把解码结果里的寄存器号映射到 vcpu->regs 的对应字段 */
static uint64_t *x86_gpr_ptr(vcpu_t *vcpu, uint32_t reg)
{
    switch (reg & 0xF) {
    case 0:  return &vcpu->regs.rax;
    case 1:  return &vcpu->regs.rcx;
    case 2:  return &vcpu->regs.rdx;
    case 3:  return &vcpu->regs.rbx;
    case 4:  return NULL;   /* RSP 未保存在 x86_guest_regs_t 中（VMCS 自动
                             * 保存/恢复），MMIO 亦不从 RSP 取数据；返回 NULL
                             * 使调用方跳过读写回。*/
    case 5:  return &vcpu->regs.rbp;
    case 6:  return &vcpu->regs.rsi;
    case 7:  return &vcpu->regs.rdi;
    case 8:  return &vcpu->regs.r8;
    case 9:  return &vcpu->regs.r9;
    case 10: return &vcpu->regs.r10;
    case 11: return &vcpu->regs.r11;
    case 12: return &vcpu->regs.r12;
    case 13: return &vcpu->regs.r13;
    case 14: return &vcpu->regs.r14;
    case 15: return &vcpu->regs.r15;
    default: return NULL;
    }
}

/* ── vmx_return 符号（vmx_run.S 中定义）─────────────────── */
extern void vmx_return(void);

/* ── VMLAUNCH/VMRESUME 汇编入口 ─────────────────────────── */
extern int vmx_enter_guest(vcpu_t *vcpu);  /* vmx_run.S */

/* ── VMXON / VMCS 内联操作 ──────────────────────────────────────────────
 * vmxon / vmclear / vmptrld 的 m64 操作数是存放「物理地址」的内存位置。
 * 内核运行在 0xffff800000000000 偏移的高半区，必须用 virt_to_phys 转换。
 * ─────────────────────────────────────────────────────────────────────── */

/* vmxon: pa 是 VMXON 区域的物理地址（64 位值） */
static inline int vmx_on(uint64_t pa)
{
    uint8_t ret;
    uint64_t fl = vmx_read_rflags() | X86_EFLAGS_CF | X86_EFLAGS_ZF;
    __asm__ volatile(
        "pushq %1; popfq; vmxon %2; setbe %0\n\t"
        : "=qm"(ret) : "q"(fl), "m"(pa) : "cc");
    return ret;
}

/* vmclear: pa 是 VMCS 的物理地址 */
static inline int vmcs_clear_pa(uint64_t pa)
{
    uint8_t ret;
    uint64_t fl = vmx_read_rflags() | X86_EFLAGS_CF | X86_EFLAGS_ZF;
    __asm__ volatile(
        "pushq %1; popfq; vmclear %2; setbe %0"
        : "=qm"(ret) : "q"(fl), "m"(pa) : "cc");
    return ret;
}

/* vmptrld: pa 是 VMCS 的物理地址 */
static inline int vmcs_load_pa(uint64_t pa)
{
    uint8_t ret;
    uint64_t fl = vmx_read_rflags() | X86_EFLAGS_CF | X86_EFLAGS_ZF;
    __asm__ volatile(
        "pushq %1; popfq; vmptrld %2; setbe %0"
        : "=qm"(ret) : "q"(fl), "m"(pa) : "cc");
    return ret;
}

/* ── VMX 支持检测 ────────────────────────────────────────── */
static int vmx_check_support(void)
{
    uint32_t eax, ebx, ecx, edx;
    vmx_cpuid(1, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1u << 5))) {
        KLOG_ERROR("[VMX] CPU does not support VMX (CPUID.1.ECX[5]=0)\n");
        return -1;
    }

    uint64_t fc = vmx_rdmsr(MSR_IA32_FEATURE_CONTROL);
    if ((fc & 0x5ULL) == 0x5ULL) {
        KLOG_INFO("[VMX] VMX enabled and locked by firmware\n");
        return 0;
    }
    if (fc & 0x1ULL) {
        KLOG_ERROR("[VMX] VMX locked out by firmware\n");
        return -1;
    }
    vmx_wrmsr(MSR_IA32_FEATURE_CONTROL, 0x5ULL);
    return 0;
}

/* ── VMXON + CR 设置 ─────────────────────────────────────── */
/*
 * VMXON 是**每 CPU 一次**的：对已经处于 VMX operation 的逻辑处理器再执行
 * 一次，指令直接置 CF（实测 `[VMX] VMXON failed (pa=0x4c6000)`）。
 *
 * 直启模式只会走到这里一次，所以原来没暴露；接上 /bin/vmm-run 之后
 * "启动 → Ctrl+] 停掉 → 再启动" 是常规用法，第二次就卡在 VMXON 上 ——
 * 报错是 `vm_create failed` → `guest_loader_run_linux failed: -1`，
 * 看起来像内存/EPT 的问题，其实是这里。
 *
 * 保持 VMX 常开（不 VMXOFF）是常规做法（KVM 也是加载时 VMXON、卸载才关）。
 */
/*
 * ⚠️ 状态必须**按 CPU** 记，不能用一个全局标志：SMP>1 时 `vmm-run`（helper）
 * 跑在 CPU1 上做 VMXON，而 vCPU 任务被钉在 CPU0 上跑 —— CPU0 从没进过 VMX
 * operation，第一条 `vmwrite`（VM_ENTRY_INTR_INFO 之类）就 #UD。
 * 实测现场：`vmm_arch_enter_guest+0x39`、EC=0x0、`RCX=0xc0000101`
 * （前一条 rdmsr MSR_GS_BASE 的残留），SMP=1 时完全正常。
 *
 * 所以 vmm_arch_enter_guest() 每次入口前也调一遍本函数：跑 vCPU 的那个 CPU
 * 自己把自己 VMXON 掉（幂等，只是一次数组查表）。
 */
static int s_vmx_on[CONFIG_SMP_CPUS];

static int vmx_global_init(void)
{
    uint32_t cpu = get_current_cpu_id();
    uint64_t fix_cr0_set, fix_cr0_clr;
    uint64_t fix_cr4_set, fix_cr4_clr;

    fix_cr0_set = vmx_rdmsr(MSR_IA32_VMX_CR0_FIXED0);
    fix_cr0_clr = vmx_rdmsr(MSR_IA32_VMX_CR0_FIXED1);
    fix_cr4_set = vmx_rdmsr(MSR_IA32_VMX_CR4_FIXED0);
    fix_cr4_clr = vmx_rdmsr(MSR_IA32_VMX_CR4_FIXED1);

    g_vmx_basic.val = vmx_rdmsr(MSR_IA32_VMX_BASIC);

    /* 满足 CR0/CR4 VMX 固定位要求 */
    vmx_write_cr0((vmx_read_cr0() & fix_cr0_clr) | fix_cr0_set);
    vmx_write_cr4((vmx_read_cr4() & fix_cr4_clr) | fix_cr4_set | X86_CR4_VMXE);

    /* 本 CPU 已经开过就不用再来一次（见 s_vmx_on 的说明）；CR0/CR4 固定位
     * 上面每次都会重新写一遍，保持 VMX operation 的前置条件成立。*/
    if (s_vmx_on[cpu])
        return 0;

    /*
     * IA32_FEATURE_CONTROL 也是**每个逻辑处理器一份**的（KVM 里就是每个 vCPU
     * 一份）。原来只有 vmx_check_support() 在 vmx_vm_init() 里使能过一次，
     * 那是在 helper 的核上 —— 于是只有那颗核被使能：
     *   SMP=1：helper 与 vCPU 同核，看不出来；
     *   SMP=2：vCPU 所在核的 vmxon 被 KVM 判为「未使能」，直接 #GP
     *          （handle_vmon 里 `msr_ia32_feature_control &
     *            VMXON_NEEDED_FEATURES` 那一条）。
     * 这也解释了「有概率」：helper 落在哪颗核，决定哪颗核被使能。
     * vmx_check_support() 幂等（已使能就直接返回），这里补在真正 VMXON 的核上。
     */
    if (vmx_check_support() != 0)
        return -1;

    /* 写**本核自己那块** VMXON 区域的版本号（区域是 per-CPU 的，见其声明）*/
    uint8_t *vmxon_region = g_vmxon_region[cpu];
    memset(vmxon_region, 0, sizeof(g_vmxon_region[0]));
    *(uint32_t *)vmxon_region = g_vmx_basic.revision;

    /* vmxon 需要物理地址 */
    uint64_t vmxon_pa = virt_to_phys(vmxon_region);
    if (vmx_on(vmxon_pa)) {
        KLOG_ERROR("[VMX] VMXON failed (pa=0x%llx)\n", vmxon_pa);
        return -1;
    }
    s_vmx_on[cpu] = 1;
    KLOG_INFO("[VMX] VMXON success on cpu%u (revision=0x%x)\n",
              cpu, g_vmx_basic.revision);
    return 0;
}

/* ── VMCS 控制字段初始化 ─────────────────────────────────── */
static void vmcs_init_ctrl(int want_ept)
{
    uint32_t msr_pin  = g_vmx_basic.ctrl ? MSR_IA32_VMX_TRUE_PIN   : MSR_IA32_VMX_PINBASED_CTLS;
    uint32_t msr_cpu  = g_vmx_basic.ctrl ? MSR_IA32_VMX_TRUE_PROC  : MSR_IA32_VMX_PROCBASED_CTLS;
    uint32_t msr_exit = g_vmx_basic.ctrl ? MSR_IA32_VMX_TRUE_EXIT  : MSR_IA32_VMX_EXIT_CTLS;
    uint32_t msr_ent  = g_vmx_basic.ctrl ? MSR_IA32_VMX_TRUE_ENTRY : MSR_IA32_VMX_ENTRY_CTLS;

    g_pin_rev.val    = vmx_rdmsr(msr_pin);
    g_cpu_rev[0].val = vmx_rdmsr(msr_cpu);
    g_exi_rev.val    = vmx_rdmsr(msr_exit);
    g_ent_rev.val    = vmx_rdmsr(msr_ent);

    if (g_cpu_rev[0].clr & CPU_SECONDARY)
        g_cpu_rev[1].val = vmx_rdmsr(MSR_IA32_VMX_PROCBASED_CTLS2);

    vmx_bitmaps_init();

    /* 期望控制位
     *
     * PIN_EXTINT：不置的话，宿主时钟中断会在 non-root 里**按 guest 的 IDT
     *   投递** —— 宿主的 ISR 永远跑不到，调度器饿死。置上之后外部中断一律
     *   先退出到 VMM，由 VMM 让出 CPU（见 exit reason 1），guest 自己的
     *   中断则靠 vLAPIC + VM-entry 注入送回。
     *
     * CPU_IO_BITMAPS / CPU_USE_MSR_BITMAPS：见文件上方 bitmap 说明。*/
    g_ctrl_pin    = PIN_EXTINT | PIN_NMI | PIN_VIRT_NMI;
    g_ctrl_exit   = EXI_HOST_64 | EXI_LOAD_EFER | EXI_SAVE_EFER;
    g_ctrl_enter  = ENT_GUEST_64 | ENT_LOAD_EFER;
    g_ctrl_cpu[0] = CPU_HLT | CPU_IO_BITMAPS | CPU_USE_MSR_BITMAPS;

    /* Secondary controls：启用 EPT（移植自 kvmm arch/x86_64/mod.rs）。
     * 前提是 primary controls 允许位 31（CPU_SECONDARY）且 EPT 被允许；
     * 否则回落到「共享 CR3、无 EPT」的原路径（guest 仍可运行）。
     *
     * CPU2_UNRESTRICTED_GUEST：放宽 non-root 下的段/特权检查。Linux 启动
     * 时会碰 CR0/CR4/段寄存器，不放开就会收到一堆 #GP（而 #GP 又要我们
     * 转发），放开之后绝大多数交给硬件直接处理。它要求 EPT 已启用。
     *
     * ⚠️ CPU_EPT 必须跟着 want_ept，不能只看能力位：
     * vmx_vm_init 只在 vm->cfg.mem_size != 0 时才写 EPT_POINTER。
     * 若这里按能力位无条件开 EPT，而 EPTP 还是 0（玩具测试就是这条路），
     * 硬件会让 guest 的**每次取指**都报 EPT violation（exit 48），
     * 而 exit 48 的处理是「解码不成就跳过指令」—— 结果 guest 在乱跑，
     * 日志上看却只是在刷 EPT violation，很容易误判成设备没实现。*/
    g_ctrl_cpu[1] = 0;
    if (want_ept && (g_cpu_rev[0].clr & CPU_SECONDARY) &&
        (g_cpu_rev[1].clr & CPU_EPT)) {
        g_ctrl_cpu[0] |= CPU_SECONDARY;
        g_ctrl_cpu[1]  = CPU_EPT | CPU2_UNRESTRICTED_GUEST;
    }

    /* 与 capability MSR 协商（allowed-1 & allowed-0）*/
    g_ctrl_pin    = (g_ctrl_pin    | g_pin_rev.set)    & g_pin_rev.clr;
    g_ctrl_exit   = (g_ctrl_exit   | g_exi_rev.set)    & g_exi_rev.clr;
    g_ctrl_enter  = (g_ctrl_enter  | g_ent_rev.set)    & g_ent_rev.clr;
    g_ctrl_cpu[0] = (g_ctrl_cpu[0] | g_cpu_rev[0].set) & g_cpu_rev[0].clr;
    /* allowed-1/allowed-0 协商；若 CPU_SECONDARY 最终未置位则清除 ctrl1 */
    if (g_ctrl_cpu[0] & CPU_SECONDARY) {
        g_ctrl_cpu[1] = (g_ctrl_cpu[1] | g_cpu_rev[1].set) & g_cpu_rev[1].clr;
    } else {
        g_ctrl_cpu[1] = 0;
    }

    vmcs_write(PIN_CONTROLS,  g_ctrl_pin);
    vmcs_write(CPU_EXEC_CTRL0, g_ctrl_cpu[0]);
    if (g_ctrl_cpu[0] & CPU_SECONDARY)
        vmcs_write(CPU_EXEC_CTRL1, g_ctrl_cpu[1]);
    vmcs_write(CR3_TARGET_COUNT, 0);
    /*
     * 异常位图。默认 0（guest 自己的 IDT 处理一切）；Linux 引导路径额外
     * 打开 bit14（#PF）—— **只为诊断**：VM-exit 不保存/恢复 CR2，所以
     * 退出到 VMM 后 `mov %cr2` 读到的就是 guest 的缺页线性地址。
     * 拿到地址后要把 #PF 原样注入回 guest（见 exit handler）。
     */
    /*
     * 异常位图保持 0：guest 的异常由它自己的 IDT 处理（真机语义）。
     *
     * 调试期曾临时置成全 1 或 bit14(#PF)，用来把 guest 的缺页拦到 VMM 里
     * 看 CR2 —— 那套诊断（EXC-DBG/EXC-STACK/PF-DBG）仍留在下面 exit
     * handler 里、按需打开即可，但**默认必须是 0**：每个异常多一次世界
     * 切换既改变时序，也会让 guest 的异常处理走样。
     */
    /*
     * ⚠️ 千万不要为了让 VMM 看到 guest 的 #PF 而在这里加 bit14！
     *
     * VM-exit **不保存/恢复 CR2**，而 VM-exit 之后宿主侧会跑一大堆代码
     * （klog、调度…），宿主的缺页会把 CR2 冲掉。再注入回 guest 时，
     * guest 自己的 do_boot_page_fault() 读到的 cr2 就是**错的**，于是它
     * 去身份映射了错误的 2MiB 区域 —— 真正出错的页永远修不好，guest
     * 陷进无限 #PF（实测 60 秒 172 万次、RIP 原地不动、无 HLT、无输出）。
     *
     * 这条规则对**所有**异常都成立：异常位图必须为 0，让 guest 用自己的
     * IDT 处理，CR2 才是对的。要诊断就用宿主侧被动采样
     * （见下面的 [RIP-SAMPLE] 退出采样），不要抢异常。
     */
    vmcs_write(EXC_BITMAP, 0);
    vmcs_write(PF_ERROR_MASK, 0);
    vmcs_write(PF_ERROR_MATCH, 0);

    /* bitmap 的物理地址（协商后确实启用才写）*/
    if (g_ctrl_cpu[0] & CPU_IO_BITMAPS) {
        vmcs_write(IO_BITMAP_A, virt_to_phys(g_io_bitmap_a));
        vmcs_write(IO_BITMAP_B, virt_to_phys(g_io_bitmap_b));
    }
    if (g_ctrl_cpu[0] & CPU_USE_MSR_BITMAPS)
        vmcs_write(MSR_BITMAP, virt_to_phys(g_msr_bitmap));

    KLOG_INFO("[VMX] ctrl: pin=0x%x cpu0=0x%x cpu1=0x%x (EPT=%d io_bm=%d msr_bm=%d)\n",
              g_ctrl_pin, g_ctrl_cpu[0], g_ctrl_cpu[1],
              !!(g_ctrl_cpu[1] & CPU_EPT),
              !!(g_ctrl_cpu[0] & CPU_IO_BITMAPS),
              !!(g_ctrl_cpu[0] & CPU_USE_MSR_BITMAPS));
}

/* ── VMCS 主机状态初始化 ─────────────────────────────────── */
static void vmcs_init_host(void)
{
    /* 读取当前 GDTR / IDTR */
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gdt_desc, idt_desc;
    __asm__ volatile("sgdt %0" : "=m"(gdt_desc));
    __asm__ volatile("sidt %0" : "=m"(idt_desc));

    /* 从 GDT[6] 读取 TSS base（16 字节 TSS 描述符，selector=0x30）*/
    uint64_t *gdt = (uint64_t *)gdt_desc.base;
    /* GDT[6] = lower 8B, GDT[7] = upper 8B */
    uint64_t tss_lo = gdt[6];
    uint64_t tss_hi = gdt[7];
    uint64_t tss_base = ((tss_lo >> 16) & 0xFFFFFFULL)
                      | (((tss_lo >> 56) & 0xFFULL) << 24)
                      | ((tss_hi & 0xFFFFFFFFULL) << 32);

    vmcs_write(HOST_EFER,       vmx_rdmsr(MSR_EFER));
    vmcs_write(EXI_CONTROLS,    g_ctrl_exit);
    vmcs_write(ENT_CONTROLS,    g_ctrl_enter);

    vmcs_write(HOST_CR0,        vmx_read_cr0());
    vmcs_write(HOST_CR3,        vmx_read_cr3());
    vmcs_write(HOST_CR4,        vmx_read_cr4());

    vmcs_write(HOST_SEL_CS,     X86_SEL_CODE64);
    vmcs_write(HOST_SEL_SS,     X86_SEL_DATA);
    vmcs_write(HOST_SEL_DS,     X86_SEL_DATA);
    vmcs_write(HOST_SEL_ES,     X86_SEL_DATA);
    vmcs_write(HOST_SEL_FS,     X86_SEL_DATA);
    vmcs_write(HOST_SEL_GS,     X86_SEL_DATA);
    vmcs_write(HOST_SEL_TR,     X86_SEL_TSS);

    vmcs_write(HOST_BASE_TR,    tss_base);
    vmcs_write(HOST_BASE_GDTR,  gdt_desc.base);
    vmcs_write(HOST_BASE_IDTR,  idt_desc.base);
    /*
     * 宿主的 FS/GS base 是 **VMCS 宿主状态字段**：VM-exit 时 CPU 会拿这里
     * 的值装回 IA32_FS_BASE / IA32_GS_BASE。原来这两行写的是 0，等于
     * 「每从 guest 退出来一次，就把宿主的 per-CPU 指针清零」——
     * 宿主自己的 %gs 相对寻址随即全指向垃圾。实测症状：用户进程一发
     * syscall，宿主 SYSCALL 入口的 `push %gs:0x60` 就缺页到地址 0x60，
     * 日志上看起来像「用户态程序把内核搞崩了」。
     *
     * 这里先取真实值；之后每次入口前还要再刷（见 vmx_refresh_host_state）。
     */
    vmcs_write(HOST_BASE_FS,    vmx_rdmsr(MSR_FS_BASE));
    vmcs_write(HOST_BASE_GS,    vmx_rdmsr(MSR_GS_BASE));

    vmcs_write(HOST_SYSENTER_CS,  0);
    vmcs_write(HOST_SYSENTER_ESP, 0);
    vmcs_write(HOST_SYSENTER_EIP, 0);

    vmcs_write(VMCS_LINK_PTR,    ~0ULL);

    /* HOST_RSP 每次 vmlaunch/vmresume 前在 vmx_run.S 中动态写入 */
    vmcs_write(HOST_RIP, (uint64_t)(uintptr_t)vmx_return);
}

/* ── VMCS guest 状态初始化 ──────────────────────────────── */
static void vmcs_init_guest(vcpu_t *vcpu, void (*entry)(void))
{
    /* ── CR 寄存器（guest 与 host 共享 CR3，不使用 EPT）── */
    vmcs_write(GUEST_CR0,   vmx_read_cr0());
    vmcs_write(GUEST_CR3,   vmx_read_cr3()); /* 共享 host 页表 */
    vmcs_write(GUEST_CR4,   vmx_read_cr4());
    vmcs_write(GUEST_EFER,  vmx_rdmsr(MSR_EFER));
    vmcs_write(GUEST_DR7,   0);

    /* ── 段选择子 ── */
    vmcs_write(GUEST_SEL_CS,   X86_SEL_CODE64);
    vmcs_write(GUEST_SEL_SS,   X86_SEL_DATA);
    vmcs_write(GUEST_SEL_DS,   X86_SEL_DATA);
    vmcs_write(GUEST_SEL_ES,   X86_SEL_DATA);
    vmcs_write(GUEST_SEL_FS,   X86_SEL_DATA);
    vmcs_write(GUEST_SEL_GS,   X86_SEL_DATA);
    vmcs_write(GUEST_SEL_LDTR, 0);
    vmcs_write(GUEST_SEL_TR,   X86_SEL_TSS);

    /* ── 段基址 ── */
    vmcs_write(GUEST_BASE_CS,   0);
    vmcs_write(GUEST_BASE_SS,   0);
    vmcs_write(GUEST_BASE_DS,   0);
    vmcs_write(GUEST_BASE_ES,   0);
    vmcs_write(GUEST_BASE_FS,   0);
    vmcs_write(GUEST_BASE_GS,   0);
    vmcs_write(GUEST_BASE_LDTR, 0);

    /* TSS base（与 host 相同）*/
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gdt_desc, idt_desc;
    __asm__ volatile("sgdt %0" : "=m"(gdt_desc));
    __asm__ volatile("sidt %0" : "=m"(idt_desc));
    uint64_t *gdt = (uint64_t *)gdt_desc.base;
    uint64_t tss_lo = gdt[6];
    uint64_t tss_hi = gdt[7];
    uint64_t tss_base = ((tss_lo >> 16) & 0xFFFFFFULL)
                      | (((tss_lo >> 56) & 0xFFULL) << 24)
                      | ((tss_hi & 0xFFFFFFFFULL) << 32);
    vmcs_write(GUEST_BASE_TR,   tss_base);
    vmcs_write(GUEST_BASE_GDTR, gdt_desc.base);
    vmcs_write(GUEST_BASE_IDTR, idt_desc.base);

    /* ── 段限制 ── */
    vmcs_write(GUEST_LIMIT_CS,   0xFFFFFFFF);
    vmcs_write(GUEST_LIMIT_SS,   0xFFFFFFFF);
    vmcs_write(GUEST_LIMIT_DS,   0xFFFFFFFF);
    vmcs_write(GUEST_LIMIT_ES,   0xFFFFFFFF);
    vmcs_write(GUEST_LIMIT_FS,   0xFFFFFFFF);
    vmcs_write(GUEST_LIMIT_GS,   0xFFFFFFFF);
    vmcs_write(GUEST_LIMIT_LDTR, 0xFFFF);
    vmcs_write(GUEST_LIMIT_TR,   0x0067);  /* 103 bytes TSS */
    vmcs_write(GUEST_LIMIT_GDTR, (uint64_t)(gdt_desc.limit));
    vmcs_write(GUEST_LIMIT_IDTR, (uint64_t)(idt_desc.limit));

    /* ── 段访问权限 ── */
    vmcs_write(GUEST_AR_CS,   0xa09b); /* 64-bit code: L=1, G=1, P=1, DPL=0 */
    vmcs_write(GUEST_AR_SS,   0xc093);
    vmcs_write(GUEST_AR_DS,   0xc093);
    vmcs_write(GUEST_AR_ES,   0xc093);
    vmcs_write(GUEST_AR_FS,   0xc093);
    vmcs_write(GUEST_AR_GS,   0xc093);
    vmcs_write(GUEST_AR_LDTR, 0x0082); /* LDT, present */
    vmcs_write(GUEST_AR_TR,   0x008b); /* Busy TSS 64-bit */

    /* ── SYSENTER ── */
    vmcs_write(GUEST_SYSENTER_CS,  0);
    vmcs_write(GUEST_SYSENTER_ESP, 0);
    vmcs_write(GUEST_SYSENTER_EIP, 0);

    /* ── RIP / RSP / RFLAGS ──
     * Linux 引导路径（guest_boot.c）会填好 g_rip/g_rsp/g_cr3 并要求直接用；
     * 玩具 guest（VMM_TEST）没填，退回"内核符号地址 + 静态栈"的老行为。*/
    if (vcpu->g_boot_linux) {
        vmcs_write(GUEST_RIP, vcpu->g_rip);
        vmcs_write(GUEST_RSP, vcpu->g_rsp);

        /* guest 自己的 CR3（GPA）：下面是它自建的临时页表，不是宿主的 */
        vmcs_write(GUEST_CR3, vcpu->g_cr3);

        /*
         * ── CR4 与 VMXE：用 guest/host mask + read shadow，不要改 GUEST_CR4 ──
         *
         * 两个约束看起来是矛盾的：
         *   (a) VMX 操作期间 CR4.VMXE 是**固定位**，必须为 1 ——
         *       GUEST_CR4 里清掉 VMXE，VM-entry 直接以 reason 33
         *       「invalid guest state」被拒（guest 一个指令都跑不到）；
         *   (b) 但 guest 看到的 CR4 **不能**有 VMXE —— Linux 早期启动会
         *       `mov %eax,%cr4`（只留 PAE|MCE），在 non-root 下清 VMXE
         *       触发 #GP，那时它还没有 IDT → triple fault。
         *
         * 解法就是硬件为此提供的那对字段：
         *   CR4_MASK        = 1<<13  → guest 写 VMXE 位会**陷入** VMM；
         *   CR4_READ_SHADOW = 真实值去掉 VMXE → guest 读 CR4 看到的是这个。
         * 于是 GUEST_CR4 始终带 VMXE（满足 a），guest 读写看到的都没有
         * （满足 b），而 guest 对 CR4 的其它位改动照常生效。
         */
        vmcs_write(GUEST_CR4, vmx_read_cr4());
        vmcs_write(CR4_MASK, (1ULL << 13));
        vmcs_write(CR4_READ_SHADOW, vmx_read_cr4() & ~(1ULL << 13));

        /* 长模式：LME=1 且 LMA=1（后者硬件进入时置位，但 VMCS 里要一起给，
         * 否则 64 位 guest 的 vmresume 会因 EFER 非法而失败）*/
        vmcs_write(GUEST_EFER, vcpu->msr_efer | (1ULL << 8) | (1ULL << 10));

        /*
         * ⚠️ 三个描述符表（GDTR/IDTR/TR）的 base **必须落在 guest 物理空间内**。
         *
         * 玩具 guest 直接用宿主的 GDTR/IDTR/TR 也能进，是因为那条路径
         * **没开 EPT** —— 那时 base 被当作宿主物理地址解释。一旦开了 EPT，
         * VM-entry 会按 **guest 物理地址**去检查它们，而宿主的
         * 0xffff8000_002001d0 这种内核虚拟地址远超 guest 物理宽度，
         * 结果就是第一次 entry 直接被拒：exit reason 33「invalid guest
         * state」，guest 一个指令都没执行。
         *
         * 所以这里给 guest 自建 GDT（含 64 位代码段）、空 IDT、空 TSS，
         * 三者都在 guest 低端内存里（0x98000/0x99000/0x9A000）。
         */
        vmcs_write(GUEST_BASE_GDTR,  vcpu->g_gdt_base);
        vmcs_write(GUEST_LIMIT_GDTR, vcpu->g_gdt_limit);
        vmcs_write(GUEST_BASE_IDTR,  GUEST_LINUX_IDT_GPA);
        vmcs_write(GUEST_LIMIT_IDTR, 0xFFF);
        vmcs_write(GUEST_BASE_TR,    GUEST_LINUX_TSS_GPA);
        vmcs_write(GUEST_LIMIT_TR,   0x67);      /* 104 字节，64 位 TSS */
        vmcs_write(GUEST_AR_TR,      0x8b);      /* busy 64-bit TSS */
        vmcs_write(GUEST_SEL_TR,     0x30);
        /* 选择子必须是 Linux 硬编码的那两个：CS=0x10、DS/ES/SS=0x18
         * （见 guest_boot.c 里 GDT 布局的说明）*/
        vmcs_write(GUEST_SEL_CS,  0x10);
        vmcs_write(GUEST_SEL_SS,  0x18);
        vmcs_write(GUEST_SEL_DS,  0x18);
        vmcs_write(GUEST_SEL_ES,  0x18);
        vmcs_write(GUEST_AR_CS,   0xa09b);
        vmcs_write(GUEST_AR_SS,   0xc093);
        vmcs_write(GUEST_AR_DS,   0xc093);
        vmcs_write(GUEST_AR_ES,   0xc093);
        vmcs_write(GUEST_AR_FS,   0xc093);
        vmcs_write(GUEST_AR_GS,   0xc093);
        vmcs_write(GUEST_BASE_FS, 0);
        vmcs_write(GUEST_BASE_GS, 0);
        vmcs_write(GUEST_BASE_LDTR, 0);
        vmcs_write(GUEST_AR_LDTR, 0x82);
        vmcs_write(GUEST_LIMIT_LDTR, 0xFFFF);
        vmcs_write(GUEST_SEL_LDTR, 0);

        if (vcpu->g_cr3 == 0) {
            /*
             * ── 32 位保护模式 guest（Linux 传统引导协议）──
             *
             * 上面的 64 位状态全部**覆盖掉**，对标 tgoskits 的引导 stub 与
             * QEMU 的 load_linux()：
             *   CR0  = PE|NE|ET（**PG=0**，分页关闭 —— 靠
             *          CPU2_UNRESTRICTED_GUEST 才被允许）
             *   CR4  = VMXE（固定位，guest 看不到，靠 READ_SHADOW 置 0）
             *   EFER = 0（LME/LMA=0）
             *   CS=0x10 / DS..GS=0x18，32 位 flat 段（AR=0xc09a / 0xc092）
             * 这样 guest 等价于「刚做完 16→32 位切换」的机器，剩下的
             * 32→64 位切换、页表、identity map、5 级页表探测全部由内核
             * 自己的 startup_32/startup_64 完成 —— **与 QEMU 裸跑同一条路**。
             *
             * 判定依据 g_cr3 == 0 是 guest_boot.c 里的约定（非零=64 位直启）。
             */
            vmcs_write(GUEST_CR0, 0x31ULL);
            vmcs_write(GUEST_CR3, 0);
            vmcs_write(GUEST_CR4, 0x2000ULL);
            vmcs_write(CR4_MASK, (1ULL << 13));
            vmcs_write(CR4_READ_SHADOW, 0);
            vmcs_write(GUEST_EFER, 0);
            vmcs_write(GUEST_SEL_CS, 0x10);
            vmcs_write(GUEST_SEL_SS, 0x18);
            vmcs_write(GUEST_SEL_DS, 0x18);
            vmcs_write(GUEST_SEL_ES, 0x18);
            /*
             * ⚠️ AR 必须带 **accessed 位**（0x9b/0x93）。GDT 描述符里写的
             * 是 0x9a/0x92，但 VMCS 里存的是**已加载段**的缓存属性 ——
             * 硬件在加载段时会把 A 位置 1。写成 0x9a/0x92 的后果是
             * VM-entry 直接被拒：exit reason 33「invalid guest state」，
             * guest 一条指令都跑不到（实测）。
             */
            vmcs_write(GUEST_AR_CS, 0xc09b);
            vmcs_write(GUEST_AR_SS, 0xc093);
            vmcs_write(GUEST_AR_DS, 0xc093);
            vmcs_write(GUEST_AR_ES, 0xc093);
            vmcs_write(GUEST_AR_FS, 0xc093);
            vmcs_write(GUEST_AR_GS, 0xc093);
            /*
             * 32 位保护模式：清掉 ENT_GUEST_64（IA-32e guest）。
             * **ENT_LOAD_EFER 保留** —— GUEST_EFER 里的 0x801（SCE|NXE，LME=0）
             * 对 32 位 guest 是合法值（KVM 同款），而关掉 load 反而会让
             * guest 带着宿主的 EFER（LMA=1）跑 32 位模式，自相矛盾。
             */
            vmcs_write(ENT_CONTROLS, g_ctrl_enter & ~ENT_GUEST_64);
        }
    } else {
        vmcs_write(GUEST_RIP,    (uint64_t)(uintptr_t)entry);
        vmcs_write(GUEST_RSP,    (uint64_t)(uintptr_t)
                   (g_guest_stack[vcpu_slot(vcpu)] + sizeof(g_guest_stack[0])));
    }
    vmcs_write(GUEST_RFLAGS, vcpu->g_boot_linux ? vcpu->regs.rflags : 0x2);

    vmcs_write(GUEST_ACTV_STATE,  ACTV_ACTIVE);
    vmcs_write(GUEST_INTR_STATE,  0);
    vmcs_write(GUEST_DEBUGCTL,    0);
}

/* ── VMCS 每 vCPU 初始化 ─────────────────────────────────── */
static int vcpu_vmcs_init(vcpu_t *vcpu, void (*entry)(void))
{
    vmcs_t *v = (vmcs_t *)g_vmcs_storage[vcpu_slot(vcpu)];
    memset(v, 0, 4096);
    v->hdr.revision_id = g_vmx_basic.revision;
    v->hdr.shadow_vmcs = 0;

    /*
     * 虚拟 MSR 影子初始化（MSR load list 的初始化在 vmcs_load 之后，
     * 因为那几个字段是写进当前被装载的 VMCS 的）。
     *
     * EFER 抄宿主的（SCE/NXE/LME 都是 64-bit guest 需要的），之后由
     * msr_emulate_write 维护 —— guest 看到的 EFER 恒等于 vcpu->msr_efer，
     * 而真实 MSR 只有宿主在用。
     *
     * APIC base 预置成上电默认值 0xFEE00900：EN(bit11)=1、BSP 位(bit8)=1、
     * 基址 0xFEE00000。vLAPIC 靠读它判断 guest 是否已经打开 APIC。*/
    vcpu->msr_efer          = vmx_rdmsr(MSR_EFER);
    vcpu->msr_star          = 0;
    vcpu->msr_lstar         = 0;
    vcpu->msr_cstar         = 0;
    vcpu->msr_sfmask        = 0;
    vcpu->msr_fs_base       = 0;
    vcpu->msr_gs_base       = 0;
    vcpu->msr_kernel_gs_base = 0;
    vcpu->msr_pat           = 0x0007040600070406ULL;  /* 上电默认 PAT */
    vcpu->apic_base         = 0xFEE00900ULL;
    vcpu->pending_event     = 0;
    vcpu->pending_errcode   = 0;
    vcpu->intr_window       = 0;

    vlapic_init(vcpu->vm, (uint32_t)vcpu->vcpu_id);

    uint64_t vmcs_pa = virt_to_phys(v);
    if (vmcs_clear_pa(vmcs_pa)) {
        KLOG_ERROR("[VMX] vmclear failed for vcpu%d (pa=0x%llx)\n",
                   vcpu->vcpu_id, vmcs_pa);
        return -1;
    }
    if (vmcs_load_pa(vmcs_pa)) {
        KLOG_ERROR("[VMX] vmptrld failed for vcpu%d (pa=0x%llx)\n",
                   vcpu->vcpu_id, vmcs_pa);
        return -1;
    }

    vmcs_init_ctrl(vcpu->vm && vcpu->vm->cfg.mem_size != 0);
    vmcs_init_host();
    vmcs_init_guest(vcpu, entry);

    /* 若已协商启用 EPT，写入 EPTP（EPT 根页表指针）*/
    if (g_ctrl_cpu[0] & CPU_SECONDARY && g_ctrl_cpu[1] & CPU_EPT) {
        vmcs_write(EPT_POINTER, x86_ept_eptp(&vcpu->vm->ept));
    }

    /*
     * Flush: 把 VMWRITE 写的字段从 CPU 内部缓存写回内存，
     * 同时将 launch state 重置为 "clear"，
     * 这样后续 vmptrld + vmlaunch 才能正确工作。
     * (参考 Intel SDM 和 ref/bare-vm/arch/x86_64/vm.c:arch_vm_init)
     */
    /*
     * MSR 自动换入换出区：guest 的 syscall MSR 必须由硬件在进出时装卸，
     * 否则 guest 的 `syscall` 会跳到宿主内核的入口（见 vmx_msr_lists_init）。
     * 必须在 vmcs_clear 之前写 —— 那几个字段属于当前装载的 VMCS。
     */
    /*
     * 踩坑记录（2026-09-25）：这两个地址字段原本写成 0x2012/0x2016，
     * **都是错的**。症状极具误导性：guest 进去就再也没出来，但
     * **没有** VM-entry failure、exit handler 一次都没进、宿主最终 IF=0
     * 停住 —— 看起来像"这套机制不能用"。实际是地址指到了别的控制字段上，
     * 硬件从错误的地方读装填表。
     * 权威编码查 `arch/x86/include/asm/vmx.h`：
     *     VM_EXIT_MSR_LOAD_ADDR  = 0x2008
     *     VM_ENTRY_MSR_LOAD_ADDR = 0x200a
     * （计数字段 0x400e/0x4010/0x4014 原本就是对的。）
     */
    vmx_msr_lists_init(vcpu);

    if (vmcs_clear_pa(vmcs_pa)) {
        KLOG_ERROR("[VMX] vmcs_clear(flush) failed for vcpu%d\n", vcpu->vcpu_id);
        return -1;
    }

    KLOG_INFO("[VMX] VMCS initialized for vcpu%d, entry=%p\n",
              vcpu->vcpu_id, entry);
    return 0;
}

/* ── exit handler ────────────────────────────────────────── */
/* ================================================================
 * 端口 I/O（PIO）模拟
 *
 * I/O bitmap 全拦，所以 guest 的每条 in/out 都到 x86_pio_handle。
 * 返回 1 = 已处理（is_in 时把值写进 *val），0 = 本机没有这个端口。
 *
 * 为什么桩也要认真写：Linux 启动时会**探测**这些端口，读到垃圾会让它
 * 误判硬件（例如把 PIC 认成有中断挂起、把 PCI 认成有总线），然后卡在
 * 探测循环或者认错设备模型。桩的原则是「给一个自洽的、什么都没有的视图」。
 * ================================================================ */
/* 状态在 vm->chipset（见 include/vmm/vmm_x86_chipset.h 的说明）*/
/* ── IO-APIC（MMIO 0xFEC00000）─────────────────────────────
 *
 * 两个寄存器：0x00 IOREGSEL 选**间接寄存器号**，0x10 IOWIN 读写数据。
 * 间接寄存器：0x00 ID、0x01 VER([7:0]版本 [23:16]最大表项号)、
 *             0x10+2n / 0x10+2n+1 = 第 n 项重定向表项的低/高 32 位。
 *
 * 表项初值全**屏蔽**（低 32 位的 bit16）—— 我们没有真外部中断，
 * 让所有线都是屏蔽态，内核探测完就会安心走 LAPIC timer。
 */
#define IOAPIC_MMIO_BASE   0xFEC00000ULL
#define IOAPIC_MMIO_SIZE   0x1000ULL
#define IOAPIC_NENT        24           /* 对应 version 0x11 */
#define IOAPIC_RT_MASKED   0x00010000u  /* 重定向表项低位的 mask 位 */

/* 状态在 vm->chipset */

/*
 * 诊断：最近 N 次 VM-exit 的环形记录（per-vCPU），triple fault 时整段回放。
 *
 * 起因：第二次启动 guest 会 triple fault，但 exit 路径上的 TEMP-DBG 是
 * `static unsigned n` + `n <= 25` 限流的 —— 第一次启动就用掉了全部配额，
 * 第二次启动的 exit 详情**一次都没打**，于是「崩之前 guest 在干什么」
 * 完全看不到。这里用环形缓冲，绕开"次数配额"这种一次性的诊断方式。
 * 每次 vmx_vm_init() 清零（和 vCPU 状态同生命周期）。
 */
#define VMM_EXIT_TRACE_MAX 32
static struct {
    uint32_t reason;
    uint32_t intr;
    uint64_t rip;
} g_exit_trace[MAX_VMS * MAX_VCPUS][VMM_EXIT_TRACE_MAX];
static uint32_t g_exit_trace_n[MAX_VMS * MAX_VCPUS];  /* 下一个写入位置 */

/* 入口状态诊断的 per-vCPU 计数（每次 vmx_vm_init 清零）。
 * 早先这里是 `static unsigned n` —— 第一次启动就把配额用光，第二次启动
 * 一条都不打，正好把最需要看的那次盖住了。 */
static uint32_t g_entry_dbg_n[MAX_VMS * MAX_VCPUS];

/*
 * 传统芯片组桩（PIC/IO-APIC/PIT/0x61）的状态现在住在 vm->chipset 里
 * （每 VM 一份，见 include/vmm/vmm_x86_chipset.h）。
 *
 * vm 可能是 NULL —— 只建 vcpu、不建 VM 的玩具路径就是（vmcs_init_ctrl 那边
 * 也按 `vcpu->vm &&` 判过）。那条路径下退回一份全局兜底状态，行为与多 VM
 * 改造前完全一致。
 */
static x86_chipset_state_t g_fallback_chipset;

static inline x86_chipset_state_t *chipset_of(const vcpu_t *vcpu)
{
    return vcpu->vm ? &vcpu->vm->chipset : &g_fallback_chipset;
}

static uint32_t ioapic_reg_read(x86_chipset_state_t *chip, uint32_t reg)
{
    switch (reg) {
    case 0x00: return 0;                                    /* ID = 0 */
    case 0x01: return ((uint32_t)(IOAPIC_NENT - 1) << 16) | 0x11u;
    case 0x02: return 0;                                    /* ARB（只读）*/
    default:
        if (reg >= 0x10 && reg < 0x10 + IOAPIC_NENT * 2)
            return chip->ioapic_rt[reg - 0x10];
        return 0;
    }
}

static void ioapic_reg_write(x86_chipset_state_t *chip, uint32_t reg,
                             uint32_t val)
{
    /* ID/VER/ARB 只读，写忽略；只接受重定向表 */
    if (reg >= 0x10 && reg < 0x10 + IOAPIC_NENT * 2)
        chip->ioapic_rt[reg - 0x10] = val;
}

/* ── 8254 PIT（三个通道，各一个 16 位递减计数）───────────────
 *
 * 时钟 1.193182 MHz。**必须真递减、真实现 LSB/MSB 读指针**：
 * Linux 的 quick_pit_calibrate()（arch/x86/kernel/tsc.c）做的事是
 *     往通道 2 写 0xffff，然后反复 `inb(0x42); inb(0x42)`，
 *     丢掉 LSB、拿 MSB 和期望值比 —— 等它从 0xff 递减下去。
 * 我们原来的桩「读回最后写入的字节」让 MSB 永远等于 0xff，
 * 于是 pit_expect_msb() 的 50000 轮循环**全部命中**、每轮 2 个
 * VM-exit，看着像死机，实际是在白烧退出。
 *
 * mode 0（一次性）：装载后递减，数到 0 就停住、输出置 1 —— 和真硬件
 * 一致，也正因为是「一次性」，内核只买得到 ~55ms 的窗口，它自己也
 * 按 50ms 上限（MAX_QUICK_PIT_MS）设计。
 */
#define PIT_HZ        1193182ULL
#define PIT_NS_PER_S  1000000000ULL

typedef struct {   /* 定义已移到 include/vmm/vmm_x86_chipset.h */
    uint16_t reload;      /* 装载值 */
    uint64_t base_ns;     /* 装载时刻（宿主单调 ns）*/
    int      running;     /* 已装载、正在计数 */
    int      read_hi;     /* 读指针：0=下次读 LSB，1=下次读 MSB */
    int      rw;          /* 控制字 RW 位：1=只 LSB 2=只 MSB 3=先 LSB 后 MSB */
    int      wstate;      /* RW=3 时已写的字节数 */
    uint8_t  wlo;         /* RW=3 时暂存的低字节 */
    uint16_t latched;     /* latch 命令冻结的值 */
    int      lat_valid;
} pit_ch_t;

/* 状态在 vm->chipset */

static uint16_t pit_now(x86_chipset_state_t *chip, int ch)
{
    x86_pit_ch_t *p = &chip->pit[ch];
    uint64_t ns, ticks;

    if (!p->running)
        return p->reload;
    ns = vlapic_now_ns();
    if (ns <= p->base_ns)
        return p->reload;               /* 标定未就绪/回绕：当作刚装载 */
    ticks = (ns - p->base_ns) * PIT_HZ / PIT_NS_PER_S;
    if (ticks >= p->reload)
        return 0;                       /* mode 0：到 0 停住 */
    return (uint16_t)(p->reload - ticks);
}

static uint8_t pit_read(x86_chipset_state_t *chip, int ch)
{
    x86_pit_ch_t *p = &chip->pit[ch];
    uint16_t c;
    uint8_t  v;

    if (p->lat_valid) {                 /* latch 快照：先低后高 */
        v = p->read_hi ? (uint8_t)(p->latched >> 8) : (uint8_t)p->latched;
        if (p->read_hi)
            p->lat_valid = 0;
        p->read_hi ^= 1;
        return v;
    }
    c = pit_now(chip, ch);
    v = p->read_hi ? (uint8_t)(c >> 8) : (uint8_t)c;
    p->read_hi ^= 1;                    /* 内部 LSB/MSB 翻转触发器 */
    return v;
}

static void pit_write_ctrl(x86_chipset_state_t *chip, uint8_t v)
{
    int ch = (v >> 6) & 3;

    if (ch == 3)
        return;                         /* read-back 命令：忽略 */

    if (((v >> 4) & 3) == 0) {          /* Latch：冻结当前计数 */
        chip->pit[ch].latched   = pit_now(chip, ch);
        chip->pit[ch].lat_valid = 1;
        chip->pit[ch].read_hi   = 0;
        return;
    }

    /* 装/卸模式（RW 位）并复位字节指针；mode/BCD 位忽略 —— 我们只做
     * 「递减到 0」这一种，够内核标定用。*/
    chip->pit[ch].rw       = (v >> 4) & 3;
    chip->pit[ch].wstate   = 0;
    chip->pit[ch].read_hi  = 0;
    chip->pit[ch].lat_valid = 0;
}

static void pit_write_data(x86_chipset_state_t *chip, int ch, uint8_t v)
{
    x86_pit_ch_t *p = &chip->pit[ch];

    if (p->rw == 1) {                   /* 只写 LSB */
        p->reload = v;
    } else if (p->rw == 2) {            /* 只写 MSB */
        p->reload = (uint16_t)(v << 8);
    } else {                            /* RW=3：先 LSB 后 MSB，写 MSB 才装载 */
        if (p->wstate == 0) {
            p->wlo    = v;
            p->wstate = 1;
            return;
        }
        p->reload = (uint16_t)(((uint16_t)v << 8) | p->wlo);
        p->wstate = 0;
    }

    p->running    = 1;                  /* 真 8254 是在写 MSB 那一刻起算 */
    p->base_ns    = vlapic_now_ns();
    p->read_hi    = 0;
    p->lat_valid  = 0;
}

int x86_pio_handle(vcpu_t *vcpu, uint32_t port, int is_in, uint32_t bytes,
                   uint64_t *val)
{
    x86_chipset_state_t *chip = chipset_of(vcpu);

    (void)bytes;   /* 端口访问一律按字节语义处理 */

    switch (port) {
    /* ── COM1（16550A）：复用 vUART 的寄存器状态机 ──
     * x86 Linux 的 8250 驱动读写的就是这 8 个端口。*/
    case 0x3f8: case 0x3f9: case 0x3fa: case 0x3fb:
    case 0x3fc: case 0x3fd: case 0x3fe: case 0x3ff:
        if (is_in)
            *val = uart16550_port_read(&vcpu->vm->uart_dev, port - 0x3f8);
        else
            uart16550_port_write(&vcpu->vm->uart_dev, port - 0x3f8, (uint8_t)*val);
        return 1;

    /* ── 8259 PIC 桩 ──
     * 中断走 vLAPIC，这里没有真控制器；但 Linux 要读写它做探测/屏蔽。
     * 给「全屏蔽、无中断挂起」的一致视图：读 IMR 回写过的掩码，
     * 读 IRR/ISR 回 0，写 ICW/OCW2(EOI) 忽略。*/
    case 0x20: case 0xa0:
        if (is_in) { *val = 0; return 1; }
        return 1;
    case 0x21:
        if (is_in) { *val = chip->pic_master_imr; return 1; }
        chip->pic_master_imr = (uint8_t)*val;
        return 1;
    case 0xa1:
        if (is_in) { *val = chip->pic_slave_imr; return 1; }
        chip->pic_slave_imr = (uint8_t)*val;
        return 1;

    /* ── 8254 PIT ── 实现见上面 pit_* 一组函数。
     * 只做计数、**不产生任何中断**（guest 的 tick 由 vLAPIC timer 提供）。*/
    case 0x40: case 0x41: case 0x42:
        if (is_in) { *val = pit_read(chip, port - 0x40); return 1; }
        pit_write_data(chip, port - 0x40, (uint8_t)*val);
        return 1;
    case 0x43:
        if (!is_in) pit_write_ctrl(chip, (uint8_t)*val);
        return 1;

    /* ── 0x61（端口 B / NMI 状态）──
     * 内核写它是为了「抬高通道 2 的 gate、关掉喇叭」，读它是为了取样
     * 通道 2 的输出位。bit5 = 输出（mode 0 下数到 0 才置 1）。
     * bit4 是输出的反相副本，老代码用它做「计数器已归零」检测。*/
    case 0x61:
        if (!is_in) { chip->port61 = (uint8_t)*val; return 1; }
        {
            uint8_t v = (uint8_t)(chip->port61 & 0x03);  /* 保留 gate/speaker */
            if (chip->pit[2].running && pit_now(chip, 2) == 0)
                v |= 0x20;                            /* bit5: 计数到 0 */
            *val = v;
        }
        return 1;

    /* ── 0x70/0x71（CMOS / RTC）──
     * 回 0：Linux 读到的 RTC 是 1970，但不会挂。*/
    case 0x70: case 0x71:
        if (is_in) { *val = 0; return 1; }
        return 1;

    /* ── PCI 配置空间（0xCF8/0xCFC）──
     * 全 1 =「没有这个设备」，Linux 会判定无 PCI 总线。*/
    case 0xcf8: case 0xcf9: case 0xcfc: case 0xcfd: case 0xcfe: case 0xcff:
        if (is_in) { *val = 0xffffffffULL; return 1; }
        return 1;

    case 0x80:   /* 诊断口：历史上什么都不接 */
        return 1;

    default:
        return 0;
    }
}

unsigned g_reason_hist[64];

static int vmx_exit_handler(vcpu_t *vcpu)
{
    uint32_t reason    = (uint32_t)(vmcs_read(EXI_REASON) & 0xff);


    /* TEMP-DBG：前 25 次退出打详细，之后每 2000 次采一次 RIP + 原因直方图 */
    {
        static unsigned n;
        /*
         * 诊断：每 20000 次退出采一次样（guest 在不在动、卡在哪个 reason）。
         * 需要时把这行的注释去掉即可 —— 排查「guest 看起来没动静」时很有用：
         * 有采样 = 还在跑，采样停住 = 真的卡了。
         */
        if (0 && (n % 20000) == 0) {
            extern unsigned g_reason_hist[64];
            KLOG_WARN("[RIP-SAMPLE] exit#%u rip=0x%llx | 1:%u 2:%u 7:%u "
                      "10:%u 12:%u 28:%u 30:%u 31:%u 32:%u 48:%u\n",
                      n, (unsigned long long)vmcs_read(GUEST_RIP),
                      g_reason_hist[1], g_reason_hist[2], g_reason_hist[7],
                      g_reason_hist[10], g_reason_hist[12], g_reason_hist[28],
                      g_reason_hist[30], g_reason_hist[31], g_reason_hist[32],
                      g_reason_hist[48]);
        }
        if (reason < 64) g_reason_hist[reason]++;
        n++;
        if (n <= 25) {
            KLOG_INFO("[VMX-DBG] exit#%u reason=%u rip=0x%llx intr_info=0x%llx\n",
                      n, reason, (unsigned long long)vmcs_read(GUEST_RIP),
                      (unsigned long long)vmcs_read(EXI_INTR_INFO));
        }
    }

    uint64_t guest_rip = vmcs_read(GUEST_RIP);
    uint64_t inst_len  = vmcs_read(EXI_INST_LEN);

    /* 环形记录（见 g_exit_trace 的注释）—— 只在 triple fault 时才回放 */
    {
        int id = (int)vcpu_slot(vcpu);
        uint32_t k  = g_exit_trace_n[id];
        g_exit_trace[id][k].reason = reason;
        g_exit_trace[id][k].intr   = (uint32_t)vmcs_read(EXI_INTR_INFO);
        g_exit_trace[id][k].rip    = guest_rip;
        g_exit_trace_n[id] = (k + 1) % VMM_EXIT_TRACE_MAX;
    }

    /* 保存 guest rflags（从 VMCS 读取，不从寄存器）*/
    vcpu->regs.rflags = vmcs_read(GUEST_RFLAGS);

    switch (reason) {
    case VMX_REASON_HLT: {
        uint64_t istate = vmcs_read(GUEST_INTR_STATE);

        /*
         * ⚠️ 清掉 STI / MOV-SS 影子位。
         *
         * guest 的 default_idle 就是 `sti; hlt`：hlt 落在 sti 的**影子里**，
         * 于是 VM-exit 时硬件把 interruptibility state 的 bit0=1 一并存进
         * VMCS。我们步进 RIP 跳过 hlt 再进去时，那个 bit 还是 1 —— 硬件
         * 认为「影子还没走完」，于是：
         *   - 注入外部中断 → VM-entry 直接被拒（reason 33）
         *   - 开 interrupt-window → 窗口条件要求「无阻塞」，永远不成立
         * 实测退化成 870 万次 HLT 退出的纯自旋（guest 再也收不到 tick）。
         *
         * 语义上清掉是**对的**：影子要求「sti 的下一条指令执行完才认中断」，
         * 而 hlt 已经执行完了（我们正是因此才拿到 HLT 退出）。
         */
        vmcs_write(GUEST_INTR_STATE, istate & ~(0x1ULL | 0x2ULL | 0x8ULL));

        /* HLT 类似 WFI：步进 RIP，yield，resume。
         * 指令长度取自 VMCS EXI_INST_LEN（移植自 kvmm handle_hlt），
         * 而非硬编码 +1 —— HLT 实际为 1 字节，但统一用 inst_len 更稳妥。*/
        vmcs_write(GUEST_RIP, guest_rip + inst_len);
        /* kvmm 在让出前执行 vmclear：VMCS 的「当前加载」状态不能跨线程
         * 存续，清除后由 vmm_arch_enter_guest 重新 vmptrld。*/
        vmcs_clear_pa(virt_to_phys((vmcs_t *)g_vmcs_storage[vcpu_slot(vcpu)]));
        vcpu->launched = 0;
        task_yield();
        return EL2_RESUME;
    }

    case VMX_REASON_VMCALL: {
        /* VMCALL: rdi = hypercall no, rsi = arg1 */
        uint64_t no   = vcpu->regs.rdi;
        uint64_t arg1 = vcpu->regs.rsi;
        /* 步进 RIP 跳过 vmcall（长度取自 VMCS，移植自 kvmm handle_vmcall）*/
        vmcs_write(GUEST_RIP, guest_rip + inst_len);

        switch (no) {
        case VMX_HYPERCALL_PRINT:
            /* 留在 INFO（test-vmm 的通过证据），抽稀成有界输出 */
            KLOG_INFO_SAMPLE("[VMX] VMCALL_PRINT: iter=%llu (vcpu%d)\n",
                             arg1, vcpu->vcpu_id);
            task_yield();
            return EL2_RESUME;

        case VMX_HYPERCALL_DONE:
            KLOG_INFO("[VMX] VMCALL_DONE: vcpu%d reports %llu iters complete\n",
                      vcpu->vcpu_id, arg1);
            return EL2_VMEXIT;

        default:
            KLOG_WARN("[VMX] Unknown hypercall no=%llu (vcpu%d)\n",
                      no, vcpu->vcpu_id);
            return EL2_RESUME;
        }
    }

    case VMX_REASON_CPUID: {
        /* 报告一个保守自洽的 CPU，而不是把宿主的 CPUID 原样透给 guest
         * （见 vmx_cpuid_emulate 的说明）。*/
        uint32_t a, b, c, d;
        vmx_cpuid_emulate((uint32_t)vcpu->regs.rax, (uint32_t)vcpu->regs.rcx,
                          &a, &b, &c, &d);
        {   /* TEMP-DBG：前 12 次 CPUID + 每次 0x15/0x16（TSC 频率）*/
            static unsigned n;
            uint32_t leaf_now = (uint32_t)vcpu->regs.rax;
            if (n < 12 || leaf_now == 0x15 || leaf_now == 0x16) {
                KLOG_INFO("[CPUID-DBG] leaf=0x%x sub=0x%x -> "
                          "eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x\n",
                          leaf_now, (uint32_t)vcpu->regs.rcx, a, b, c, d);
                n++;
            }
        }
        vcpu->regs.rax = a;
        vcpu->regs.rbx = b;
        vcpu->regs.rcx = c;
        vcpu->regs.rdx = d;
        vmcs_write(GUEST_RIP, guest_rip + inst_len);
        return EL2_RESUME;
    }

    case VMX_REASON_EPT_VIOL: {
        /* EPT 违规 → 设备 MMIO 模拟（移植自 kvmm handle_ept_violation）。
         *
         * EPT violation 是**指令级**陷入：RIP 指向出错指令，需手动步进。
         * 访问方向/宽度从 EXI_QUALIFICATION 解码，读写数据经 guest 寄存器
         * （GPR 由 vmx_run.S 在退出时保存到 vcpu->regs）传递。
         *
         * qualification 位：0=读,1=写,2=取指,3=读权限,4=写权限,5=取指权限,
         *                   6=因 EPT 页表项无效（vs 权限不足）。
         */
        uint64_t gpa  = vmcs_read(GUEST_PHYS_ADDR);
        uint64_t qual = vmcs_read(EXI_QUALIFICATION);
        int is_write  = (int)((qual >> 1) & 1);
        x86_chipset_state_t *chip = chipset_of(vcpu);

        /*
         * ── 先分 RAM / MMIO ────────────────────────────────────────
         *
         * 按需分页之后 EPT 初始是空的，所以 guest **每一次落到新页**都会走到
         * 这里。RAM 窗口内的走缺页分配，窗口外的才是设备 MMIO。
         *
         * ⚠️ 这个分支必须在最前面。老版本没有它 —— 因为那时 guest RAM 是
         * 预先整段映射好的，任何 EPT violation 按定义都必是非 RAM。改成按需
         * 分页后若不加这一条，guest 取指/访存第一次碰到新页就会掉进下面的
         * MMIO 解码路径：解码失败 → 落到最底下的兜底 → **只推进 RIP、不写
         * 目标寄存器**，于是 guest 拿着上一条指令的残留值继续跑。那种故障
         * 表现为"guest 随机跑飞"，比直接崩掉难查得多。
         *
         * ⚠️ **不推进 RIP** —— 这正是"缺页"与"MMIO 模拟"的分野：MMIO 那条路
         * 是 VMM 替 guest 完成了这次访问，所以要把 RIP 挪过去；这里只是把内存
         * 补上，访问本身还得 guest 自己重做一次。两条路走反了的表现分别是
         * "死循环"和"跳过一条随机指令"。
         */
        if (vcpu->vm && x86_ept_ipa_is_ram(&vcpu->vm->ept, gpa)) {
            uint64_t n = x86_ept_map_block(&vcpu->vm->ept, gpa, 1 /*zero*/);

            if (n == 0) {
                KLOG_ERROR("[VMX] vcpu%d: RAM fault at gpa=0x%llx but PMM is "
                           "out of pages (free=%llu), stopping vm%u\n",
                           vcpu->vcpu_id, (unsigned long long)gpa,
                           (unsigned long long)pmm_get_free_pages(g_pmm),
                           vcpu->vm->vmid);
                return EL2_EXIT;
            }
            vcpu->vm->ept.nr_fault += n;
            x86_ept_invept_all(&vcpu->vm->ept);

            KLOG_INFO_SAMPLE("[VMX] vm%u: ept %s fault gpa=0x%llx -> block "
                             "+%llu pages (total fault=%llu)\n",
                             vcpu->vm->vmid, is_write ? "write" : "read",
                             (unsigned long long)gpa, (unsigned long long)n,
                             (unsigned long long)vcpu->vm->ept.nr_fault);
            return EL2_RESUME;
        }

        {   /* TEMP-DBG：前 8 次 EPT violation 的 gpa，看 guest 在碰什么 */
            static unsigned ne;
            if (ne < 8) {
                // KLOG_INFO("[EPT-DBG] gpa=0x%llx %s rip=0x%llx\n",
                //           (unsigned long long)gpa, is_write ? "W" : "R",
                //           (unsigned long long)guest_rip);
                // ne++;
            }
        }

        if (vcpu->vm && vcpu->vm->mmio_bus) {
            uint64_t out = 0;
            /* 清零：imm 仅立即数写路径才赋值，其余路径保持 0 */
            x86_mmio_access_t acc = {0};

            /* 解码出错指令（GUEST_RIP 处，CISC 变长）以取得
             * 「数据寄存器 + 访问宽度 + 源/目标方向」*/
            if (decode_mmio_access(vcpu, guest_rip, &acc) &&
                decode_matches_qual(qual, &acc)) {
                uint64_t val = acc.imm;      /* 立即数写 */

                /* ⚠️ 判据必须是 has_reg，不能写 `acc.reg != 0` —— 0 就是
                 * %rax，那样会把所有从 RAX 传值的写当成立即数 0（见结构体
                 * 注释里的实测症状）。*/
                if (acc.is_write && acc.has_reg) {
                    uint64_t *src = x86_gpr_ptr(vcpu, acc.reg);
                    if (src)
                        val = *src;
                }

                /*
                 * vLAPIC 的 MMIO 窗口先于总线处理。
                 * 0xFEE00000 在 guest RAM 之外 → EPT 里是无效项 → 每次
                 * LAPIC 访问天然走到这里，不需要 VMX 的 APIC-access page。
                 */
                /*
                 * IO-APIC（0xFEC00000，同样在 guest RAM 之外）。
                 * 我们只投 vLAPIC 的 timer，没有任何真外部中断，所以要给
                 * Linux 一个「所有重定向项都屏蔽」的自洽视图：它读 IOAPICVER
                 * 拿到版本和表项数、读写重定向表做屏蔽/解屏蔽，然后认定
                 * 没有可用的外部中断 —— 这正是我们要它相信的。
                 *
                 * 没有这个桩时每次访问都落到下面的「未处理 EPT」，只靠
                 * 步进 RIP 蒙混过去：**目标寄存器根本没人写**，guest 读回
                 * 的是上一条指令的残留值（实测 IOAPICVER 读成 0，内核于是
                 * 把表项数算错、打出 "version 0, GSI 0-95"）。
                 */
                if (gpa >= IOAPIC_MMIO_BASE &&
                    gpa <  IOAPIC_MMIO_BASE + IOAPIC_MMIO_SIZE) {
                    if (!chip->ioapic_inited) {
                        chip->ioapic_inited = 1;
                        for (int i = 0; i < IOAPIC_NENT; i++)
                            chip->ioapic_rt[i * 2] = IOAPIC_RT_MASKED;
                    }
                    /* ⚠️ 必须按**页内偏移**区分两个寄存器。曾经写成
                     * `(gpa & ~0xFFF) == BASE && (gpa & 0xf) == 0` ——
                     * 0xFEC00010 也满足 `& 0xf == 0`，于是所有 IOWIN
                     * 访问都被当成 IOREGSEL，数据寄存器永远读不回真值，
                     * Linux 拿它去建 irq_cfg 直接空指针崩在 setup_IO_APIC。*/
                    uint32_t off = (uint32_t)(gpa - IOAPIC_MMIO_BASE);
                    uint32_t reg = chip->ioapic_sel & 0xff;
                    uint32_t v32 = (uint32_t)val;
                    if (off == 0x00) {          /* IOREGSEL */
                        if (acc.is_write) chip->ioapic_sel = v32;
                        else              v32 = chip->ioapic_sel;
                    } else {                     /* 0x10 IOWIN */
                        if (acc.is_write) ioapic_reg_write(chip, reg, v32);
                        else              v32 = ioapic_reg_read(chip, reg);
                    }
                    if (!acc.is_write) {
                        uint64_t *dst = x86_gpr_ptr(vcpu, acc.reg);
                        if (dst)
                            *dst = v32;
                    }
                    vmcs_write(GUEST_RIP, guest_rip + acc.inst_len);
                    return EL2_RESUME;
                }

                if (gpa >= VLAPIC_MMIO_BASE &&
                    gpa <  VLAPIC_MMIO_BASE + VLAPIC_MMIO_SIZE) {
                    uint64_t lval = acc.is_write ? val : 0;
                    if (vlapic_mmio_handle(vcpu->vm, gpa, acc.is_write,
                                           acc.size, &lval)) {
                        if (!acc.is_write) {
                            uint64_t *dst = x86_gpr_ptr(vcpu, acc.reg);
                            if (dst)
                                *dst = lval;
                        }
                        vmcs_write(GUEST_RIP, guest_rip + acc.inst_len);
                        return EL2_RESUME;
                    }
                }

                if (mmio_bus_handle(vcpu->vm->mmio_bus, gpa, is_write,
                                    acc.size, val,
                                    (uint32_t)vcpu->vcpu_id, &out)) {
                    if (!acc.is_write) {
                        /* 读：写回目标 GPR */
                        uint64_t *dst = x86_gpr_ptr(vcpu, acc.reg);
                        if (dst)
                            *dst = out;
                    }
                    KLOG_INFO("[VMX] MMIO %s GPA=0x%llx size=%u reg=%u (vcpu%d)\n",
                              is_write ? "write" : "read", gpa,
                              acc.size, acc.reg, vcpu->vcpu_id);
                    vmcs_write(GUEST_RIP, guest_rip + acc.inst_len);
                    return EL2_RESUME;
                }
            } else {
                KLOG_WARN("[VMX] MMIO inst decode failed at rip=0x%llx "
                          "(vcpu%d), falling back\n", guest_rip, vcpu->vcpu_id);
            }
        }

        /*
         * 未映射且不是已知设备：记一笔再吞掉。
         *
         * ⚠️ 只推进 RIP 是**不够**的 —— 读操作的目标寄存器会保留上一条指令
         * 的残留值，guest 于是拿着一个看似合法、实则陈旧的数继续跑。老版本
         * 就是这么写的，IOAPICVER 读成 0 那类 bug 正是它养出来的
         * （见本文件上面 IO-APIC 那段的注释）。这里至少把目标寄存器写成 0，
         * 让"读到一个设备返回 0"这件事显式化。
         */
        KLOG_ERROR("[VMX] EPT violation: gpa=0x%llx qual=0x%llx rip=0x%llx "
                   "(vcpu%d)\n",
                   gpa, qual, guest_rip, vcpu->vcpu_id);
        if (!is_write) {
            x86_mmio_access_t acc2;
            if (decode_mmio_access(vcpu, guest_rip, &acc2)) {
                uint64_t *dst = x86_gpr_ptr(vcpu, acc2.reg);
                if (dst)
                    *dst = 0;
            }
        }
        vmcs_write(GUEST_RIP, guest_rip + inst_len);
        return EL2_RESUME;
    }

    case VMX_REASON_CR: {
        /*
         * CR 访问陷入。目前只有 CR4.VMXE 一个位被 mask 住（见
         * vmcs_init_guest 的说明），所以这里只需处理「写 CR4」：
         * 把 guest 想要的其它位照收，但 **VMXE 必须保持 1**（否则下一次
         * VM-entry 又会 reason 33），并把 guest 可见的影子同步过去。
         *
         * qualification 低 4 位 = CR 编号，bits 5:4 = 访问类型（0=写CR,
         * 1=读CR, 2=写CR8 hmm 见 SDM），不是 CR4 就原样放行。
         */
        uint64_t qual = vmcs_read(EXI_QUALIFICATION);
        uint32_t cr   = (uint32_t)(qual & 0xf);
        uint32_t type = (uint32_t)((qual >> 4) & 0x3);

        if (cr == 4 && type == 0) {           /* MOV CR4, r */
            uint32_t gpr = (uint32_t)((qual >> 8) & 0xf);
            uint64_t *src = x86_gpr_ptr(vcpu, gpr);
            if (src) {
                uint64_t want = (*src & ~(1ULL << 13)) | (1ULL << 13);
                vmcs_write(GUEST_CR4, want);
                vmcs_write(CR4_READ_SHADOW, want & ~(1ULL << 13));
            }
        }
        vmcs_write(GUEST_RIP, guest_rip + inst_len);
        return EL2_RESUME;
    }

    case VMX_REASON_IO: {
        /* IN/OUT：I/O bitmap 全拦，所以 guest 每次端口访问都到这里。
         * qualification 给端口号/方向/宽度，指令长度由 VMCS 提供。*/
        uint64_t qual  = vmcs_read(EXI_QUALIFICATION);
        uint32_t port  = VMX_IO_QUAL_PORT(qual);
        int      is_in = (qual & VMX_IO_QUAL_IN) != 0;
        uint32_t sz    = VMX_IO_QUAL_SIZE(qual);
        uint32_t bytes = (sz == 0) ? 1 : (sz == 1 ? 2 : 4);
        uint64_t val   = is_in ? 0 : (vcpu->regs.rax & 0xffffffffULL);

        if (x86_pio_handle(vcpu, port, is_in, bytes, &val)) {
            if (is_in)
                vcpu->regs.rax = val;
        } else if (is_in) {
            /* 无设备端口：按「总线浮空」返回全 1（PC 上的惯例）*/
            vcpu->regs.rax = 0xffffffffULL;
        }

        vmcs_write(GUEST_RIP, guest_rip + inst_len);
        return EL2_RESUME;
    }

    case VMX_REASON_MSR_READ: {
        uint32_t msr = (uint32_t)vcpu->regs.rcx;
        uint64_t val = 0;
        msr_emulate_read(vcpu, msr, &val);
        /* rdmsr 结果进 EDX:EAX */
        vcpu->regs.rax = (uint32_t)val;
        vcpu->regs.rdx = (uint32_t)(val >> 32);
        vmcs_write(GUEST_RIP, guest_rip + inst_len);
        return EL2_RESUME;
    }

    case VMX_REASON_MSR_WRITE: {
        uint32_t msr = (uint32_t)vcpu->regs.rcx;
        uint64_t val = ((uint64_t)(uint32_t)vcpu->regs.rdx << 32)
                     | (uint32_t)vcpu->regs.rax;
        msr_emulate_write(vcpu, msr, val);
        vmcs_write(GUEST_RIP, guest_rip + inst_len);
        return EL2_RESUME;
    }

    case VMX_REASON_EXTINT:
        /*
         * 宿主外部中断。PIN_EXTINT 置位后，non-root 里的物理中断一律先
         * 退到这里 —— 否则硬件会按 **guest 的 IDT** 投递，宿主的时钟
         * ISR 永远跑不到、调度器饿死。
         * 让出 CPU 让宿主 ISR 跑完，再原样回 guest。
         * guest 自己的中断走另一条路：vLAPIC → VM-entry 注入。
         */
        {
            /* 让出 CPU：宿主的外部中断 ISR、调度器都要借这次退出跑完。
             * 不 yield 的话 vcpu 任务会一直占着宿主 CPU —— 宿主时钟 ISR
             * 仍在推进，但别的任务（含 guest 控制台轮询）永远排不上队。*/
            task_yield();
        }
        return EL2_RESUME;

    case VMX_REASON_INTR_WINDOW:
        /* guest 刚把 RFLAGS.IF 从 0 变成 1：挂起的事件现在可以投了。
         * 真正的写入在下次 VM-entry 前（vmm_arch_restore_guest_ctx
         * → vmx_inject_pending）。*/
        return EL2_RESUME;

    case VMX_REASON_TRIPLE_FAULT:
        KLOG_ERROR("[VMX] guest triple fault (vcpu%d) rip=0x%llx rsp=0x%llx\n",
                   vcpu->vcpu_id,
                   (unsigned long long)guest_rip,
                   (unsigned long long)vmcs_read(GUEST_RSP));

        /* 回放最近 VMM_EXIT_TRACE_MAX 次退出（最旧在前）。triple fault
         * 往往是"前面某个异常没被正确处理"的终点，这一段比终点本身有用。*/
        {
            int id = (int)vcpu_slot(vcpu);
            KLOG_ERROR("[VMX] last %d VM-exits of vcpu%u (oldest first):\n",
                       VMM_EXIT_TRACE_MAX, id);
            for (uint32_t k = 0; k < VMM_EXIT_TRACE_MAX; k++) {
                uint32_t i = (g_exit_trace_n[id] + k) % VMM_EXIT_TRACE_MAX;
                KLOG_ERROR("[VMX]   +%u reason=%u rip=0x%llx intr=0x%x\n",
                           k, g_exit_trace[id][i].reason,
                           (unsigned long long)g_exit_trace[id][i].rip,
                           g_exit_trace[id][i].intr);
            }
        }
        /*
         * 把出错 RIP 附近的 guest 字节 dump 出来。guest RAM 就在宿主窗口里，
         * 可以直接读 —— 没有这一手就只能对着 "triple fault" 干瞪眼。
         */
        {
            static const char hx[] = "0123456789abcdef";
            char line[3 * 32 + 4];
            uint64_t lim  = guest_mem_size(vcpu);

            /*
             * guest_rip 是**线性地址**：guest 内核 text 在 0xffffffff80000000
             * 之上，直接拿它跟 guest 物理窗口（0..192MiB）比永远不成立 ——
             * 早先的写法就是这样，于是最需要 dump 的时候一行都没打。
             * 按 __pa_symbol 的公式换算：phys = rip - __START_KERNEL_map +
             * phys_base，其中 phys_base = 0x1000000（bzImage 的 pref 地址）。
             */
            uint64_t gp = guest_rip;
            if (guest_rip >= 0xffffffff80000000ULL)
                gp = guest_rip - 0xffffffff80000000ULL + 0x1000000ULL;

            if (gp >= 0x10 && gp < lim) {
                const uint8_t *p = gpa_to_host(vcpu, gp - 0x10);

                if (p) {
                    int o = 0;
                    for (int i = 0; i < 32; i++) {
                        line[o++] = hx[p[i] >> 4];
                        line[o++] = hx[p[i] & 0xf];
                        if (i == 15)
                            line[o++] = ' ';
                    }
                    line[o] = 0;
                    KLOG_ERROR("[VMX]   guest@0x%llx-0x10: %s\n",
                               (unsigned long long)guest_rip, line);
                }
            }
        }
        return EL2_EXIT;

    case VMX_REASON_EXC_NMI: {
        uint64_t intr_info = vmcs_read(EXI_INTR_INFO);
        uint8_t  vector    = (uint8_t)(intr_info & 0xFF);
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

        /*
         * 诊断：guest 的 #PF（异常位图只开了 bit14）。CR2 此刻还是 guest
         * 的值 —— VM-exit 不保存/恢复 CR2，这是唯一能看到缺页地址的机会。
         */
        {
            static unsigned seen[32];
            if (vector < 32 && seen[vector] < 40) {
                uint64_t rsp = vmcs_read(GUEST_RSP);
                const uint64_t *st = NULL;
                if (rsp + 0x40 < guest_mem_size(vcpu))
                    st = (const uint64_t *)gpa_to_host(vcpu, rsp);
                KLOG_INFO("[EXC-DBG] v=%u #%u rip=0x%llx cr2=0x%llx err=0x%llx "
                          "rsp=0x%llx cs=0x%llx rax=0x%llx rcx=0x%llx\n",
                          vector, seen[vector],
                          (unsigned long long)guest_rip,
                          (unsigned long long)cr2,
                          (unsigned long long)vmcs_read(EXI_INTR_ERROR),
                          (unsigned long long)rsp,
                          (unsigned long long)vmcs_read(GUEST_SEL_CS),
                          (unsigned long long)vcpu->regs.rax,
                          (unsigned long long)vcpu->regs.rcx);
                KLOG_INFO("[EXC-REG] rbx=0x%llx rbp=0x%llx rsi=0x%llx rdi=0x%llx "
                          "r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx "
                          "r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx\n",
                          (unsigned long long)vcpu->regs.rbx,
                          (unsigned long long)vcpu->regs.rbp,
                          (unsigned long long)vcpu->regs.rsi,
                          (unsigned long long)vcpu->regs.rdi,
                          (unsigned long long)vcpu->regs.r8,
                          (unsigned long long)vcpu->regs.r9,
                          (unsigned long long)vcpu->regs.r10,
                          (unsigned long long)vcpu->regs.r11,
                          (unsigned long long)vcpu->regs.r12,
                          (unsigned long long)vcpu->regs.r13,
                          (unsigned long long)vcpu->regs.r14,
                          (unsigned long long)vcpu->regs.r15);
                KLOG_INFO("[EXC-IDT] idtr=0x%llx/0x%llx gdtr=0x%llx cr3=0x%llx "
                          "efer=0x%llx cr0=0x%llx\n",
                          (unsigned long long)vmcs_read(GUEST_BASE_IDTR),
                          (unsigned long long)vmcs_read(GUEST_LIMIT_IDTR),
                          (unsigned long long)vmcs_read(GUEST_BASE_GDTR),
                          (unsigned long long)vmcs_read(GUEST_CR3),
                          (unsigned long long)vmcs_read(GUEST_EFER),
                          (unsigned long long)vmcs_read(GUEST_CR0));
                if (st)
                    KLOG_INFO("[EXC-STACK] +00:%016llx +08:%016llx "
                              "+10:%016llx +18:%016llx +20:%016llx +28:%016llx\n",
                              (unsigned long long)st[0], (unsigned long long)st[1],
                              (unsigned long long)st[2], (unsigned long long)st[3],
                              (unsigned long long)st[4], (unsigned long long)st[5]);
                seen[vector]++;
            }
        }
        {
            static unsigned npf;
            if (npf < 1) {
                uint64_t rsp = vmcs_read(GUEST_RSP);
                KLOG_INFO("[PF-DBG] #%u rip=0x%llx cr2=0x%llx err=0x%llx "
                          "cr3=0x%llx rsp=0x%llx\n",
                          npf, (unsigned long long)guest_rip,
                          (unsigned long long)cr2,
                          (unsigned long long)vmcs_read(EXI_INTR_ERROR),
                          (unsigned long long)vmcs_read(GUEST_CR3),
                          (unsigned long long)rsp);
                /* dump guest 栈顶 12 个 qword（物理地址可读 → 可符号化）*/
                const uint64_t *st =
                    (const uint64_t *)gpa_to_host(vcpu, rsp);
                if (st && rsp + 0x60 < guest_mem_size(vcpu)) {
                    for (int i = 0; i < 12; i += 4)
                        KLOG_INFO("[PF-STACK] +%02x: %016llx %016llx "
                                  "%016llx %016llx\n", i * 8,
                                  (unsigned long long)st[i],
                                  (unsigned long long)st[i + 1],
                                  (unsigned long long)st[i + 2],
                                  (unsigned long long)st[i + 3]);
                }
                npf++;
            }
        }

        {
            /* TEMP: 前 3 次缺页的**完整现场**（缺页处理器拿到的就是这些）。
             * 之后不再打印，避免宿主代码把 CR2 冲掉影响后续判断。*/
            static unsigned long pf_n;
            if (pf_n < 3UL) {
                KLOG_WARN("[PFSITE] #%lu rip=0x%llx cr2=0x%llx err=0x%llx rsp=0x%llx\n",
                          pf_n, (unsigned long long)guest_rip,
                          (unsigned long long)cr2,
                          (unsigned long long)vmcs_read(EXI_INTR_ERROR),
                          (unsigned long long)vmcs_read(GUEST_RSP));
                KLOG_WARN("[PFSITE] rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx "
                          "rsi=0x%llx rdi=0x%llx rbp=0x%llx\n",
                          (unsigned long long)vcpu->regs.rax,
                          (unsigned long long)vcpu->regs.rbx,
                          (unsigned long long)vcpu->regs.rcx,
                          (unsigned long long)vcpu->regs.rdx,
                          (unsigned long long)vcpu->regs.rsi,
                          (unsigned long long)vcpu->regs.rdi,
                          (unsigned long long)vcpu->regs.rbp);
                KLOG_WARN("[PFSITE] r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx "
                          "r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx\n",
                          (unsigned long long)vcpu->regs.r8,
                          (unsigned long long)vcpu->regs.r9,
                          (unsigned long long)vcpu->regs.r10,
                          (unsigned long long)vcpu->regs.r11,
                          (unsigned long long)vcpu->regs.r12,
                          (unsigned long long)vcpu->regs.r13,
                          (unsigned long long)vcpu->regs.r14,
                          (unsigned long long)vcpu->regs.r15);
                {
                    /* 缺页现场把 inflate_fast 的**栈帧**读出来：序言把
                     *   end 存在 0x00(%rsp) （aa9017）
                     *   beg 存在 0x10(%rsp) （aa9033）
                     * 于是 op = out - beg 可以直接算，判定是 dist 野还是 beg 歪。*/
                    uint64_t rsp = vmcs_read(GUEST_RSP);
                    const uint64_t *st = (const uint64_t *)gpa_to_host(vcpu, rsp);
                    const uint8_t  *z  = (const uint8_t *)gpa_to_host(vcpu, 0x3422e40u);
                    const uint8_t  *zi = (const uint8_t *)gpa_to_host(vcpu, 0x3422ea0u);

                    /* 按需分页下这几个地址可能还没被 guest 碰过 —— 判 NULL 跳过 */
                    if (st && z && zi) {
                        uint64_t no = 0, ao = 0;
                        __builtin_memcpy(&no, z + 0x18, 8);
                        __builtin_memcpy(&ao, z + 0x20, 4);
                        KLOG_WARN("[FLOCAL] window(0x20%%rsp)=0x%llx "
                                  "state.wsize=0x%x whave=0x%x write=0x%x "
                                  "len(%%rdx)=0x%llx\n",
                                  (unsigned long long)st[4],
                                  *(const uint32_t *)(zi + 0x2c),
                                  *(const uint32_t *)(zi + 0x30),
                                  *(const uint32_t *)(zi + 0x34),
                                  (unsigned long long)vcpu->regs.rdx);
                        KLOG_WARN("[FLOCAL] end=0x%llx beg=0x%llx 0x30=0x%llx "
                                  "out=0x%llx dist=0x%llx\n",
                                  (unsigned long long)st[0],
                                  (unsigned long long)st[2],
                                  (unsigned long long)st[6],
                                  (unsigned long long)vcpu->regs.r11,
                                  (unsigned long long)vcpu->regs.r8);
                        KLOG_WARN("[FLOCAL] strm: next_out=0x%llx avail_out=%llu\n",
                                  (unsigned long long)no, (unsigned long long)ao);
                    }
                }
                pf_n++;
            }
        }
        /* 原样注回 guest：硬件异常，带 error code（如果需要）*/
        if (vector != 0) {   /* 注回 guest：硬件异常原样送回 */
            vcpu->pending_event = VMX_INTR_VALID | VMX_INTR_TYPE_HWEXC
                                | VMX_INTR_VECTOR(vector);
            if (intr_info & (1u << 11)) {
                vcpu->pending_event |= VMX_INTR_ERRCODE_VALID;
                vcpu->pending_errcode = vmcs_read(EXI_INTR_ERROR);
            }
            return EL2_RESUME;
        }

        KLOG_ERROR("[VMX] EXCEPTION vector=%u rip=0x%llx (vcpu%d)\n",
                   vector, guest_rip, vcpu->vcpu_id);
        return EL2_EXIT;
    }

    default:
        KLOG_ERROR("[VMX] Unhandled exit reason=%u rip=0x%llx qual=0x%llx "
                   "(vcpu%d)\n",
                   reason, guest_rip, vmcs_read(EXI_QUALIFICATION),
                   vcpu->vcpu_id);
        if (reason == 33) {   /* VM-entry failure: invalid guest state */
            KLOG_ERROR("[VMX] entry state: intr=0x%llx err=0x%llx ilen=%llu "
                       "msr_cnt=%llu msr_adr=0x%llx\n",
                       vmcs_read(VM_ENTRY_INTR_INFO),
                       vmcs_read(VM_ENTRY_EXC_ERRCODE),
                       vmcs_read(VM_ENTRY_INST_LEN),
                       vmcs_read(VM_ENTRY_MSR_LOAD_COUNT),
                       vmcs_read(VM_ENTRY_MSR_LOAD_ADDR));
            KLOG_ERROR("[VMX]   cr0=0x%llx cr3=0x%llx cr4=0x%llx efer=0x%llx "
                       "rflags=0x%llx\n",
                       vmcs_read(GUEST_CR0), vmcs_read(GUEST_CR3),
                       vmcs_read(GUEST_CR4), vmcs_read(GUEST_EFER),
                       vmcs_read(GUEST_RFLAGS));
            KLOG_ERROR("[VMX]   cs=0x%llx ar=0x%llx gdtr=0x%llx/%llu "
                       "idtr=0x%llx/%llu tr=0x%llx ar=0x%llx\n",
                       vmcs_read(GUEST_SEL_CS), vmcs_read(GUEST_AR_CS),
                       vmcs_read(GUEST_BASE_GDTR), vmcs_read(GUEST_LIMIT_GDTR),
                       vmcs_read(GUEST_BASE_IDTR), vmcs_read(GUEST_LIMIT_IDTR),
                       vmcs_read(GUEST_SEL_TR), vmcs_read(GUEST_AR_TR));
            /* 这三样是「注入外部中断被拒」的头号嫌疑：
             *   actv=1        guest 处于 halted（执行过 hlt）
             *   intr_state    bit0=STI 屏蔽 bit1=MOV-SS 屏蔽 bit3=SMI
             *   inst_error    硬件给出的**具体**失败原因（SDM 表 30-1）
             * 注意 inst_error 是「上次失败的 VM-entry/VMREAD」留下的，
             * 要在这里立刻读，任何其它 vmread/vmwrite 都可能覆盖它。*/
            KLOG_ERROR("[VMX]   actv=0x%llx intr_state=0x%llx\n",
                       vmcs_read(GUEST_ACTV_STATE),
                       vmcs_read(GUEST_INTR_STATE));
            /* inst_error 必须**最后**读：读任何其它 VMCS 字段都可能把它覆盖 */
            KLOG_ERROR("[VMX]   inst_error=0x%llx\n",
                       vmcs_read(VMX_INST_ERROR));
        }
        KLOG_ERROR("  inst_len=%llu RAX=0x%llx RBX=0x%llx RCX=0x%llx\n",
                   inst_len, vcpu->regs.rax, vcpu->regs.rbx, vcpu->regs.rcx);
        return EL2_EXIT;
    }
}

/* ── x86 VM 初始化（由 vmm.c 中 vm_create 调用）──────────── */
int vmx_vm_init(vm_t *vm)
{
    int i;
    int nr = vm->cfg.nr_vcpus;

    if (nr < 1 || nr > MAX_VCPUS) {
        KLOG_ERROR("[VMX] vmx_vm_init: invalid nr_vcpus=%d\n", nr);
        return -1;
    }

    /* 检查 VMX 硬件支持 */
    if (vmx_check_support() != 0)
        return -1;

    /* 全局 VMX 初始化（VMXON）*/
    if (vmx_global_init() != 0)
        return -1;

    /* EPT 二级地址翻译（移植自 kvmm mm/ept.rs）：仅在 VM 请求了独立
     * guest RAM（cfg.mem_size != 0）时建立。
     *
     * ⚠️ 建出来的是**空表**（"空表即全 trap"）：guest RAM 不再预映射，首次
     * 访问才由 EPT violation 处理分配物理页并装映射。宿主自己要写的那几块
     * （bzImage 各段 / initrd / boot_params / 初始页表）由 guest_boot.c 用
     * x86_ept_map_range() 显式映射 —— 与 aarch64/riscv 完全同构。
     *
     * 注：即使建表，也需 CPU_EXEC_CTRL1 的 CPU_EPT 位真正置位才生效
     * （见 vmcs_init_ctrl 协商结果）。*/
    if (vm->cfg.mem_size != 0) {
        x86_ept_vm_init(&vm->ept, (uint32_t)vm->slot,
                        vm->cfg.mem_base, vm->cfg.mem_size);

        mmio_bus_init(&vm->mmio_bus_storage);
        if (uart16550_init(vm, &vm->uart_dev, &vm->mmio_bus_storage) != 0)
            KLOG_WARN("[VMX] uart16550 registration failed\n");
        vm->mmio_bus = &vm->mmio_bus_storage;

        KLOG_INFO("[VMX] EPT enabled (mem=0x%llx+0x%llx)\n",
                  (unsigned long long)vm->cfg.mem_base,
                  (unsigned long long)vm->cfg.mem_size);
        KLOG_INFO("[VMX] MMIO bus ready: virtual 16550A @0x%llx\n",
                  (unsigned long long)UART16550_BASE);
    } else {
        KLOG_INFO("[VMX] EPT not requested (guest shares host CR3)\n");
    }

    /* 初始化每个 vCPU */
    for (i = 0; i < nr; i++) {
        vcpu_t *vcpu = &vm->vcpus[i];
        memset(vcpu, 0, sizeof(*vcpu));
        vcpu->vcpu_id  = i;
        vcpu->launched = 0;
        vcpu->vm       = vm;
    }
    vm->nr_vcpus = nr;

    /*
     * exit 诊断环形缓冲与 vCPU 同生命周期 —— 不清的话第二次启动回放出来的
     * 是上一次启动的尾巴（和 static 限流计数是同一类坑）。
     * 只清**本 VM 这一格**：这些数组现在按 (vm->slot, vcpu_id) 索引，全清会
     * 把另一个正在跑的 VM 的诊断信息一起抹掉。
     */
    for (i = 0; i < nr; i++) {
        uint32_t sl = (uint32_t)(vm->slot * MAX_VCPUS + i);
        memset(g_exit_trace[sl], 0, sizeof(g_exit_trace[sl]));
        g_exit_trace_n[sl] = 0;
        g_entry_dbg_n[sl]  = 0;
    }

    /*
     * ── 复位虚拟设备桩的可变状态 ────────────────────────────────
     *
     * 语义上每次 boot 本就该等于设备上电。从前这些是**文件级 static**，
     * 生命周期是整个内核，于是第二次启动会带着第一次停止那一刻的残留：
     *
     *   - ioapic_inited==1 ⇒ 「把所有重定向项设成屏蔽」那段初始化被**整段
     *     跳过**（它是 if (!inited) 守卫的）；
     *   - ioapic_sel / ioapic_rt[] ⇒ 上次的寄存器选择与路由表；
     *   - PIC 屏蔽字、PIT 通道、端口 61 同理。
     *
     * 症状（x86_64 + helper + SMP=4 实测）：第二次启动的 guest triple fault，
     * RIP 每次都是同一个地址 —— 确定性崩溃，只是"是否踩中"取决于上次停止时
     * 残留了什么，所以表现为概率性。
     *
     * ⚠️ 多 VM 之后这段**不能**再写成全清：vm_t 里每 VM 一份 chipset，但这段
     * 代码是在**新 VM 启动**时跑的，全清就会把另一个正在跑的 VM 的 PIC/PIT/
     * IOAPIC 状态一起抹掉（单 VM 下看着对，多 VM 下就是串台）。
     * （vLAPIC 和 UART 桩各自在 vlapic_init()/uart16550_init() 里已经 memset，
     *   不在这里重复 —— 那两个函数每次 boot 都会被调到。）
     */
    g_fallback_chipset.pic_master_imr = 0xFF;
    g_fallback_chipset.pic_slave_imr  = 0xFF;
    vm->chipset.pic_master_imr = 0xFF;
    vm->chipset.pic_slave_imr  = 0xFF;
    vm->chipset.ioapic_sel     = 0;
    vm->chipset.ioapic_inited  = 0;
    memset(vm->chipset.ioapic_rt, 0, sizeof(vm->chipset.ioapic_rt));
    memset(vm->chipset.pit, 0, sizeof(vm->chipset.pit));
    vm->chipset.port61 = 0;

    KLOG_INFO("[VMX] vmx_vm_init: %d vCPU(s) ready\n", nr);
    return 0;
}

/*
 * vmx_vcpu_setup — 为 vCPU 设置 VMCS（含 guest 入口地址）
 * 必须在 vcpu_task_create 之前调用（或在 vCPU 任务启动时调用）。
 */
int vmx_vcpu_setup(vcpu_t *vcpu, void (*entry)(void))
{
    return vcpu_vmcs_init(vcpu, entry);
}

/* ── 架构钩子实现 ────────────────────────────────────────── */

/*
 * 控制台（COM1 / IRQ4）中断注入
 *
 * 为什么必须做：guest 侧的 8250 驱动只有在**真的收到中断**时才会去取
 * RX FIFO。没有中断它退化成定时器轮询 —— `irq=0` 时 `univ8250_setup_timer()`
 * 起一个 `serial8250_timeout()`，每 `uart_poll_timeout()`（这里 ≈8ms）
 * 自己调一次 handle_irq（8250_core.c:308）。交互式输入因此慢一个数量级，
 * 而且驱动永远停在「等 IRQ4」的退化路径上。
 *
 * aarch64/riscv 两个架构早就在每次入口重拉 vGIC/vPLIC 的 pending
 * （见 el2_run.c 的 aarch64_check_vpl011_rx、hext_run.c 的
 * vcpu_external_irq_on_entry），**只有 x86 这条线是断的**：
 * uart16550_irq_asserted() 定义了却没有任何调用者。
 *
 * 向量从 guest 自己编的 IO-APIC 重定向表里取：ISA IRQ4 → GSI4 → RT 项 4
 * （默认 ISA MP 表是 IRQ n → pin n，IRQ0 例外去 pin2 —— guest 日志里的
 * `..TIMER: vector=0x30 apic1=0 pin1=2` 就是这个表）。表项还没 unmask、
 * 或投递模式不是 fixed 就什么都不投：那是 guest 自己的状态，它 unmask 完
 * 下一次入口我们自然会看见。
 *
 * 电平触发：条件成立就每入口重拉一次（IRR 位在取走时清掉），这样
 * guest 应答中断、FIFO 里却还有字节时不会丢。
 */
static void x86_console_irq_on_entry(vcpu_t *vcpu)
{
    uint32_t rt, vec;

    if (!vcpu->vm || !vmm_console_irq_asserted(vcpu->vm))
        return;

    rt = chipset_of(vcpu)->ioapic_rt[4 * 2];     /* IRQ4 → GSI4 的低 32 位 */
    if ((rt & IOAPIC_RT_MASKED) || (rt & 0x700) != 0)
        return;

    vec = rt & 0xff;
    if (vec < 16)
        return;

    vlapic_raise_irq(vcpu->vm, vec);
}

void vmm_arch_restore_guest_ctx(vcpu_t *vcpu)
{
    /*
     * ── ① 先让**这颗核**进入 VMX operation（VMXON 是每 CPU 一次的）──
     *
     * ⚠️ 必须在下面那步之前：VMCS 初始化里的 VMCLEAR/VMPTRLD 是 VMX 指令，
     * 没开 VMX operation 直接 #UD。helper 模式下做 VMXON 的是 /bin/vmm-run
     * 所在的核，而 vCPU 钉在 CPU0，**SMP>1 时不是同一颗** —— 实测就是
     * `CPU exception #6 at RIP=vcpu_vmcs_init+0x156`（#UD），backtrace
     * 指向 vmm_arch_restore_guest_ctx。vmm_arch_enter_guest() 里那次仍然保留
     * （幂等：s_vmx_on[cpu] 查表），失败那条路由它去报。
     */
    if (vmx_global_init() != 0)
        return;

    /*
     * ── ② 把本 vCPU 的 VMCS 装成"当前 VMCS" ──────────────────────
     *
     * ⚠️ **必须在 vmx_inject_pending() 之前**：那个函数会用 vmcs_write() 写
     * VM_ENTRY_INTR_INFO，而 VMREAD/VMWRITE 操作的永远是**本核当前装载的**
     * 那块 VMCS —— 不是"你想操作的那块"。
     *
     * 单 VM 时当前 VMCS 恰好一直是它的，所以看不出问题；**多 VM 共用一颗核
     * 时就不是了**：vm1 的 vCPU 任务退出 → 让给 vm2 → vm2 进来时当前 VMCS
     * 还是 vm1 的，于是 vm2 的中断信息被写进了 **vm1 的 VMCS**；而
     * vlapic_accept_interrupt() 已经把 ISR 位置在了 vm2 的 vLAPIC 上。
     * 结果：那个向量既没送到 vm2，vm1 也不会去 EOI 它 —— ISR 位**永久**卡住，
     * 之后 vlapic_take_pending()（irr & ~isr）再也挑不出这个向量。
     *
     * 实测症状正是这样：起第二个 VM 之后，第一个 VM 的定时器在第 501 次
     * 触发处停住，LAPIC 快照是 `isr[7]=0x1000`（236 = LOCAL_TIMER_VECTOR）
     * 而 `irr` 全 0 —— 输入能进 guest（那是 IO-APIC 的另一条向量），但
     * guest 拿不到 tick，tty 的 workqueue 不跑，于是"能输入、没回显"。
     *
     * 把装载挪到这里，restore 之后的每一次 vmcs_write/vmcs_read（注入、
     * ENTRY-DBG、vmlaunch 本身）都作用在正确的 VMCS 上。
     */
    {
    vmcs_t *v = (vmcs_t *)g_vmcs_storage[vcpu_slot(vcpu)];
    uint64_t vmcs_pa = virt_to_phys(v);

    /*
     * ── VMCS 初始化搬到**跑 vCPU 的这颗核**上做（每个 VM 一次）────────
     *
     * vmx_vcpu_setup() 内部就是 `VMCLEAR → VMPTRLD → 写全部字段`，这正是
     * 唯一正确的顺序。问题只在于**它在哪颗核上跑**：
     *
     *   - VMCLEAR 只对**本核 current 的** VMCS 有效。原路径在 /bin/vmm-run
     *     所在核（helper 核）上 clear，随后 vCPU 核一 VMPTRLD，那块 VMCS
     *     就"搬"过去了 —— 再在 helper 核上 clear 是空操作，launch state
     *     永远停在 launched ⇒ 第二次启动 VMLAUNCH 报 inst_error=0x4
     *     （VMLAUNCH with non-clear VMCS），guest 一个字节都不输出。
     *   - VMCLEAR 会把这 VMCS 复位成"上次退出时的快照"。所以它必须发生在
     *     写字段**之前**：先 clear 再 setup 不能反。（曾经把 clear 单独挪到
     *     这里、排在 setup 之后 —— 结果是刚写好的入口状态被快照覆盖，guest
     *     从上次断点继续跑、内存却已被 memset 清零 ⇒ 连第一次启动都 triple
     *     fault。）
     *
     * 所以：在这里（vCPU 核上）整个重做一遍，vmcs_ready 保证每个 VM 生命
     * 周期只做一次。HLT yield 那条路径也会 clear + launched=0，但它清的是
     * 运行中的 VMCS，快照即最新状态，不需要（也不该）重做初始化。
     *
     * 只对 Linux 引导路径做：玩具 guest（VMM_TEST）的入口在 vmx_vcpu_setup()
     * 的 entry 参数里，这里拿不到，且它每次 QEMU 只启动一次、没有这个坑。
     */
    if (!vcpu->vmcs_ready && vcpu->g_boot_linux) {
        vmx_vcpu_setup(vcpu, NULL);     /* clear → load → 写全部字段 */
        vcpu->launched   = 0;           /* 刚 clear 过 ⇒ 必须走 VMLAUNCH */
        vcpu->vmcs_ready = 1;
    }

    /*
     * 无论走上面哪条路，进 guest 之前都必须让**本核的 current VMCS** 是这一块：
     *   - vmx_vcpu_setup() 结尾还有一次 flush 用的 VMCLEAR（把 VMWRITE 的结果
     *     真正写回内存），执行完 VMCS 就不再是 current；
     *   - HLT yield 那条路径也会 VMCLEAR。
     * 少了这一下，后面的 VMREAD/VMLAUNCH 全在"没有 current VMCS"的状态下执行，
     * 症状极具误导性：ENTRY-DBG 读出来的是栈上的垃圾、inst_error 是个非法值
     * （实测 0x4400），而日志里只会看到一句 guest entry failed。
     */
    if (vmcs_load_pa(vmcs_pa)) {
        KLOG_ERROR("[VMX] vmptrld failed in vmm_arch_enter_guest (vcpu%d pa=0x%llx)\n",
                   vcpu->vcpu_id, vmcs_pa);
        return 0;
    }
    }

    /*
     * 每次进入 guest 前投递积压的事件（中断/异常）。
     * guest 屏蔽着中断时 vmx_inject_pending 会改成开 interrupt-window
     * exiting，等它开中断的那一刻(exit 7)再回来投。
     */
    /*
     * vLAPIC 虚拟定时器：看 deadline 过没过，过了就排一个外部中断。
     * 轮询节拍由 PIN_EXTINT 提供 —— 宿主 100Hz 时钟本身就让 guest
     * 每 10ms 退出来一次，不需要额外的宿主定时器基础设施。
     */
    vlapic_timer_poll(vcpu->vm);

    /* 控制台：电平触发，每次入口按设备状态重拉（见上面 x86_console_irq_on_entry）*/
    x86_console_irq_on_entry(vcpu);


    if (!vcpu->pending_event) {
        uint32_t vec;
        if (vlapic_sw_enabled(vcpu->vm) && vlapic_take_pending(vcpu->vm, &vec))
            vcpu->pending_event = VMX_INTR_VALID | VMX_INTR_TYPE_EXTINT
                                | VMX_INTR_VECTOR(vec);
    }

    vmx_inject_pending(vcpu);
    /* 其余 guest 系统寄存器由 VMCS 自动保存/恢复 */
}

/*
 * vmx_refresh_host_state — 把宿主的易变状态刷进 VMCS 宿主区
 *
 * 这三个字段 VMCS 不会自动跟着宿主变，而宿主在两次 VM-entry 之间可能已经
 * 换过进程（CR3）、换过 CPU 或改过寄存器（FS/GS base）：
 *
 *   HOST_CR0/CR4  — 宿主开关 CR0/CR4 位（WP/SMEP/SMAP）后必须同步，
 *                   VM-exit 是从这里装回宿主 CR 的。
 *   HOST_CR3      — 切进程就变。不刷的话 VM-exit 会把宿主装回**旧**页表，
 *                   宿主立刻在别人的地址空间里跑，症状是随机的 #PF。
 *   HOST_BASE_FS  — 宿主用户态线程的 TLS 基址。
 *   HOST_BASE_GS  — per-CPU 指针。**最关键的一个**：宿主的 syscall 入口、
 *                   中断入口都靠 %gs 取 per-CPU 数据，装回旧值就全错。
 *
 * ⚠️ 除了 VMCS 宿主区，**VM-exit 的 MSR load 表**里也有宿主的值，而且
 * 硬件是**最后**装那张表的（在宿主状态字段之后），所以表里的值会盖掉
 * HOST_BASE_GS。GS 那一对（GS_BASE/KERNEL_GS_BASE）在 g_msr_list 里
 * （guest 的 swapgs 往返要用），因此这里必须把它们**一起刷**：表里是
 * vmx_msr_lists_init() 时的一次性快照，不刷就等于把宿主装回旧 %gs ——
 * 实测症状是宿主立刻崩、QEMU 反复重启（SeaBIOS → 埋 sentinel → 崩）。
 *
 * 代价是两条 rdmsr + 三条 vmwrite，相对一次世界切换可以忽略，
 * 所以每次入口前无条件刷（x-kernel 的 refresh_host_state 同理）。
 */
static void vmx_refresh_host_state(vcpu_t *vcpu)
{
    int id = (int)vcpu_slot(vcpu);
    int i0 = vmx_msr_index(MSR_GS_BASE);
    int i1 = vmx_msr_index(MSR_KERNEL_GS_BASE);

    vmcs_write(HOST_CR0,     vmx_read_cr0());
    vmcs_write(HOST_CR3,     vmx_read_cr3());
    vmcs_write(HOST_CR4,     vmx_read_cr4());
    vmcs_write(HOST_BASE_FS, vmx_rdmsr(MSR_FS_BASE));
    vmcs_write(HOST_BASE_GS, vmx_rdmsr(MSR_GS_BASE));

    /*
     * 宿主 MSR 载入表（g_msr_host）里的值也必须是**本核**的。
     *
     * VM-exit 时硬件按这张表把宿主 MSR 装回去，而 vmx_msr_lists_init() 只在
     * VM 初始化时、在**调用者所在的核**上抓过一次快照 —— helper 模式下那是
     * /bin/vmm-run 的核，而 vCPU 钉在 BSP 上。于是 SMP>1 且 helper 落在另一颗
     * 核时，每次从 guest 出来宿主都被装上了**别的核**的 MSR；其中
     * MSR_KERNEL_GS_BASE 就是那颗核的 per-CPU 指针（见 g_msr_list）。
     *
     * 宿主自己的 swapgs（SYSCALL 入口）随即把内核的 %gs 指到别的核上，
     * task_current() 于是返回别的核的任务。实测连锁反应（SMP=2）：
     *   - execve 的 fd 继承把 idle 任务那张没初始化过的 fd_table 当父进程，
     *     128 个 fd 池槽被一次吃光（之后宿主里所有 open() 都失败）；
     *   - execve 的 wrapper 把 idle/1 当自己退出掉 → 该核没有 idle → 整机卡死。
     * SMP=1 时 helper 与 vCPU 同核，快照恰好是对的，所以一直没暴露。
     */
    for (int i = 0; i < VMM_MSR_COUNT; i++) {
        if (g_msr_host[id][i].idx)
            g_msr_host[id][i].val = vmx_rdmsr(g_msr_host[id][i].idx);
    }

    /*
     * `swapgs` 往返的另一半：guest 里的 swapgs 直接改硬件 MSR，VMM 看不见；
     * VM-exit 的 store 表把它存进 g_msr_store，这里搬进 entry-load 表
     * （g_msr_guest），下次进入 guest 时再装回去。
     *
     * 少了这一步，guest 的 swapgs 等于没执行：Linux `paranoid_entry` 用
     * `rdmsr MSR_GS_BASE` 判断自己在哪个 GS 就会判错 → 异常死循环（实测卡在
     * `Run /init as init process`，rip 恒为 paranoid_entry+0x93、reason 恒为
     * RDMSR=31、12 万次采样一动不动）。
     */
    if (i1 >= 0) g_msr_guest[id][i1].val = g_msr_store[id][1].val;

    /* GS_BASE 不能走 VMX 的 load 表（放进去 VM-entry 直接以 reason 34
     * "MSR loading" 失败），改走 GUEST_BASE_GS 字段 —— 效果一样：把
     * exit-store 捞回来的、guest swapgs 之后的真实值装回去。*/
    vmcs_write(GUEST_BASE_GS, g_msr_store[id][0].val);
}

int vmm_arch_enter_guest(vcpu_t *vcpu)
{
    /*
     * 跑 vCPU 的这个 CPU 必须自己处于 VMX operation（VMXON 是每 CPU 一次
     * 的）：helper 模式下做 VMXON 的是 /bin/vmm-run 所在的 CPU，而 vCPU 任务
     * 钉在 CPU0 —— 不补这一下，SMP>1 时第一条 vmwrite 就 #UD。
     * 幂等：只是一次按 CPU 的数组查表。
     */
    if (vmx_global_init() != 0)
        return 0;

    /*
     * ⚠️ VMCS 的装载（含 vmcs_ready 那次初始化）**不在这里** —— 它被提到了
     * vmm_arch_restore_guest_ctx() 的最前面。见那边的注释：本函数之前会有
     * 一次 vmcs_write()（中断注入），那次写必须落在**本 vCPU 的** VMCS 上。
     */
    vmx_refresh_host_state(vcpu);


    {   /* 入口状态：每次 VM 启动的头 2 次 entry 打印，用来还原「第二次启动
         * 时 VMCS 里的 guest 状态到底是什么」。计数是 per-vCPU 且随 VM 生命
         * 周期清零 —— 见 g_entry_dbg_n 的注释。*/
        int id = (int)vcpu_slot(vcpu);
        if (g_entry_dbg_n[id] < 2) {
            KLOG_INFO("[ENTRY-DBG] cr0=%llx cr3=%llx cr4=%llx efer=%llx "
                      "rflags=%llx rip=%llx rsp=%llx\n",
                      vmcs_read(GUEST_CR0), vmcs_read(GUEST_CR3),
                      vmcs_read(GUEST_CR4), vmcs_read(GUEST_EFER),
                      vmcs_read(GUEST_RFLAGS), vmcs_read(GUEST_RIP),
                      vmcs_read(GUEST_RSP));
            KLOG_INFO("[ENTRY-DBG] cs=%llx/ar=%llx ss=%llx/ar=%llx "
                      "ds=%llx/ar=%llx tr=%llx/ar=%llx/%llx gdtr=%llx/%llu "
                      "idtr=%llx/%llu\n",
                      vmcs_read(GUEST_SEL_CS), vmcs_read(GUEST_AR_CS),
                      vmcs_read(GUEST_SEL_SS), vmcs_read(GUEST_AR_SS),
                      vmcs_read(GUEST_SEL_DS), vmcs_read(GUEST_AR_DS),
                      vmcs_read(GUEST_SEL_TR), vmcs_read(GUEST_AR_TR),
                      vmcs_read(GUEST_BASE_TR),
                      vmcs_read(GUEST_BASE_GDTR), vmcs_read(GUEST_LIMIT_GDTR),
                      vmcs_read(GUEST_BASE_IDTR), vmcs_read(GUEST_LIMIT_IDTR));
            KLOG_INFO("[ENTRY-DBG] pin=%x cpu0=%x cpu1=%x entry=%x/%x/%x "
                      "eptp=%llx\n",
                      (unsigned)vmcs_read(PIN_CONTROLS),
                      (unsigned)vmcs_read(CPU_EXEC_CTRL0),
                      (unsigned)vmcs_read(CPU_EXEC_CTRL1),
                      (unsigned)vmcs_read(ENT_CONTROLS),
                      (unsigned)vmcs_read(EXI_CONTROLS),
                      (unsigned)vmcs_read(EXC_BITMAP),
                      vmcs_read(EPT_POINTER));
            g_entry_dbg_n[id]++;
        }
    }

    int ret = vmx_enter_guest(vcpu);
    if (!ret) {
        /*
         * vmlaunch/vmresume 早期失败。VMCS 此刻还装载着，可以直接读出
         * 具体原因 —— 没有这一行的话失败是完全静默的（vCPU 任务直接结束，
         * 日志上看只剩「task started」然后就没了）。
         * 编码见 Intel SDM Vol 3C 附录 C。
         */
        KLOG_ERROR("[VMX] VM-entry failed: inst_error=0x%llx launched=%d "
                   "(entry intr=0x%llx, msr_load_cnt=%llu, reason=0x%llx)\n",
                   vmcs_read(VMX_INST_ERROR), vcpu->launched,
                   vmcs_read(VM_ENTRY_INTR_INFO),
                   vmcs_read(VM_ENTRY_MSR_LOAD_COUNT),
                   vmcs_read(EXI_REASON));
        return 0;
    }
    return ret;
}

int vmm_arch_exit_handler(vcpu_t *vcpu)
{
    return vmx_exit_handler(vcpu);
}

void vmm_arch_save_guest_ctx(vcpu_t *vcpu)
{
    (void)vcpu;
    /* x86 VMCS 自动保存/恢复，无需手动操作 */
}
