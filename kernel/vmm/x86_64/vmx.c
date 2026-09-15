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

#include "vmm.h"
#include "klog.h"
#include "string.h"
#include "task/task.h"
#include "x86_64/vmx.h"
#include "x86_64/ept.h"  /* EPT 二级翻译 */
#include "vmm_mmio.h"    /* MMIO 总线 */
#include "vmm_uart16550.h"
#include "mm_vm.h"   /* virt_to_phys */

/* ── 静态存储（4KB 对齐）──────────────────────────────────── */

/* VMXON 区域（4KB）*/
static uint8_t g_vmxon_region[4096] __attribute__((aligned(4096)));

/* 每个 vCPU 的 VMCS（4KB）*/
static uint8_t g_vmcs_storage[MAX_VCPUS][4096] __attribute__((aligned(4096)));

/* 每个 vCPU 的 guest 栈（4KB）*/
static uint8_t g_guest_stack[MAX_VCPUS][4096] __attribute__((aligned(16)));

/* VMX capability（init 时从 MSR 读取）*/
static vmx_basic_t   g_vmx_basic;
static vmx_ctrl_msr_t g_pin_rev, g_cpu_rev[2], g_exi_rev, g_ent_rev;

static uint32_t g_ctrl_pin;
static uint32_t g_ctrl_cpu[2];
static uint32_t g_ctrl_exit;
static uint32_t g_ctrl_enter;

/* ── 全局 MMIO 总线与虚拟设备（静态存储，单 VM）───────────── */
static mmio_bus_t    g_x86_mmio_bus;
static mmio_device_t g_x86_uart_dev;

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

/* 从 guest 内存取字节（经 EPT 翻译 GPA→HPA→内核直接映射）*/
static int guest_fetch_bytes(uint64_t gpa, uint8_t *buf, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint64_t hpa;
        if (!x86_ept_gpa_to_hpa(gpa + i, &hpa))
            return 0;
        buf[i] = *(volatile uint8_t *)phys_to_virt(hpa);
    }
    return 1;
}

/* 解码结果 */
typedef struct {
    int      is_write;
    uint8_t  size;       /* 访问字节数 */
    uint32_t reg;        /* GPR 号（0-15）*/
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

static int decode_mmio_access(uint64_t rip_gpa, x86_mmio_access_t *acc)
{
    uint8_t  buf[24];
    uint32_t i = 0;
    int      rex_w = 0, opsize16 = 0;

    if (!guest_fetch_bytes(rip_gpa, buf, sizeof(buf)))
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
        acc->reg  = reg_field;
        acc->inst_len = i + modrm_used;
        return 1;
    }
    case 0x8A: case 0x8B: {          /* MOV r, r/m  → 读 */
        if (!parse_modrm(buf + i, (uint32_t)(sizeof(buf) - i),
                         &reg_field, &modrm_used))
            return 0;
        acc->is_write = 0;
        acc->size = (op == 0x8A) ? 1 : (opsize16 ? 2 : (rex_w ? 8 : 4));
        /* 读方向：目标寄存器在 reg 字段，需带 REX.R */
        acc->reg  = reg_field | (rex_w ? 8u : 0u);
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
        acc->reg  = 0;               /* 立即数写：无源寄存器 */
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
static int vmx_global_init(void)
{
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

    /* 写 VMXON 区域版本号 */
    memset(g_vmxon_region, 0, sizeof(g_vmxon_region));
    *(uint32_t *)g_vmxon_region = g_vmx_basic.revision;

    /* vmxon 需要物理地址 */
    uint64_t vmxon_pa = virt_to_phys(g_vmxon_region);
    if (vmx_on(vmxon_pa)) {
        KLOG_ERROR("[VMX] VMXON failed (pa=0x%llx)\n", vmxon_pa);
        return -1;
    }
    KLOG_INFO("[VMX] VMXON success (revision=0x%x)\n", g_vmx_basic.revision);
    return 0;
}

/* ── VMCS 控制字段初始化 ─────────────────────────────────── */
static void vmcs_init_ctrl(void)
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

    /* 期望控制位 */
    g_ctrl_pin    = PIN_NMI | PIN_VIRT_NMI;
    g_ctrl_exit   = EXI_HOST_64 | EXI_LOAD_EFER | EXI_SAVE_EFER;
    g_ctrl_enter  = ENT_GUEST_64 | ENT_LOAD_EFER;
    g_ctrl_cpu[0] = CPU_HLT;           /* HLT 陷入 VMM */

    /* Secondary controls：启用 EPT（移植自 kvmm arch/x86_64/mod.rs）。
     * 前提是 primary controls 允许位 31（CPU_SECONDARY）且 EPT 被允许；
     * 否则回落到「共享 CR3、无 EPT」的原路径（guest 仍可运行）。*/
    g_ctrl_cpu[1] = 0;
    if ((g_cpu_rev[0].clr & CPU_SECONDARY) && (g_cpu_rev[1].clr & CPU_EPT)) {
        g_ctrl_cpu[0] |= CPU_SECONDARY;
        g_ctrl_cpu[1]  = CPU_EPT;
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
    vmcs_write(EXC_BITMAP,    0);   /* 不捕获 guest 异常 */
    vmcs_write(PF_ERROR_MASK, 0);
    vmcs_write(PF_ERROR_MATCH, 0);
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
    vmcs_write(HOST_BASE_FS,    0);
    vmcs_write(HOST_BASE_GS,    0);

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

    /* ── RIP / RSP / RFLAGS ── */
    vmcs_write(GUEST_RIP,    (uint64_t)(uintptr_t)entry);
    vmcs_write(GUEST_RSP,    (uint64_t)(uintptr_t)
               (g_guest_stack[vcpu->vcpu_id] + sizeof(g_guest_stack[0])));
    vmcs_write(GUEST_RFLAGS, 0x2); /* RF=0, reserved bit 1 = 1 */

    vmcs_write(GUEST_ACTV_STATE,  ACTV_ACTIVE);
    vmcs_write(GUEST_INTR_STATE,  0);
    vmcs_write(GUEST_DEBUGCTL,    0);
}

/* ── VMCS 每 vCPU 初始化 ─────────────────────────────────── */
static int vcpu_vmcs_init(vcpu_t *vcpu, void (*entry)(void))
{
    vmcs_t *v = (vmcs_t *)g_vmcs_storage[vcpu->vcpu_id];
    memset(v, 0, 4096);
    v->hdr.revision_id = g_vmx_basic.revision;
    v->hdr.shadow_vmcs = 0;

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

    vmcs_init_ctrl();
    vmcs_init_host();
    vmcs_init_guest(vcpu, entry);

    /* 若已协商启用 EPT，写入 EPTP（EPT 根页表指针）*/
    if (g_ctrl_cpu[0] & CPU_SECONDARY && g_ctrl_cpu[1] & CPU_EPT) {
        vmcs_write(EPT_POINTER, x86_ept_eptp());
    }

    /*
     * Flush: 把 VMWRITE 写的字段从 CPU 内部缓存写回内存，
     * 同时将 launch state 重置为 "clear"，
     * 这样后续 vmptrld + vmlaunch 才能正确工作。
     * (参考 Intel SDM 和 ref/bare-vm/arch/x86_64/vm.c:arch_vm_init)
     */
    if (vmcs_clear_pa(vmcs_pa)) {
        KLOG_ERROR("[VMX] vmcs_clear(flush) failed for vcpu%d\n", vcpu->vcpu_id);
        return -1;
    }

    KLOG_INFO("[VMX] VMCS initialized for vcpu%d, entry=%p\n",
              vcpu->vcpu_id, entry);
    return 0;
}

/* ── exit handler ────────────────────────────────────────── */
static int vmx_exit_handler(vcpu_t *vcpu)
{
    uint32_t reason    = (uint32_t)(vmcs_read(EXI_REASON) & 0xff);
    uint64_t guest_rip = vmcs_read(GUEST_RIP);
    uint64_t inst_len  = vmcs_read(EXI_INST_LEN);

    /* 保存 guest rflags（从 VMCS 读取，不从寄存器）*/
    vcpu->regs.rflags = vmcs_read(GUEST_RFLAGS);

    switch (reason) {
    case VMX_REASON_HLT:
        /* HLT 类似 WFI：步进 RIP，yield，resume。
         * 指令长度取自 VMCS EXI_INST_LEN（移植自 kvmm handle_hlt），
         * 而非硬编码 +1 —— HLT 实际为 1 字节，但统一用 inst_len 更稳妥。*/
        vmcs_write(GUEST_RIP, guest_rip + inst_len);
        /* kvmm 在让出前执行 vmclear：VMCS 的「当前加载」状态不能跨线程
         * 存续，清除后由 vmm_arch_enter_guest 重新 vmptrld。*/
        vmcs_clear_pa(virt_to_phys((vmcs_t *)g_vmcs_storage[vcpu->vcpu_id]));
        vcpu->launched = 0;
        task_yield();
        return EL2_RESUME;

    case VMX_REASON_VMCALL: {
        /* VMCALL: rdi = hypercall no, rsi = arg1 */
        uint64_t no   = vcpu->regs.rdi;
        uint64_t arg1 = vcpu->regs.rsi;
        /* 步进 RIP 跳过 vmcall（长度取自 VMCS，移植自 kvmm handle_vmcall）*/
        vmcs_write(GUEST_RIP, guest_rip + inst_len);

        switch (no) {
        case VMX_HYPERCALL_PRINT:
            if (arg1 % 20 == 0)
                KLOG_INFO("[VMX] VMCALL_PRINT: iter=%llu (vcpu%d)\n",
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

    case VMX_REASON_CPUID:
        /* CPUID：步进 RIP（长度取自 VMCS），保持 rax/rbx/rcx/rdx 不变。
         * TODO: 对标 kvmm handle_cpuid，返回零值叶（当前依赖 guest 自填）。*/
        vmcs_write(GUEST_RIP, guest_rip + inst_len);
        return EL2_RESUME;

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

        if (vcpu->vm && vcpu->vm->mmio_bus) {
            uint64_t out = 0;
            /* 清零：imm 仅立即数写路径才赋值，其余路径保持 0 */
            x86_mmio_access_t acc = {0};

            /* 解码出错指令（GUEST_RIP 处，CISC 变长）以取得
             * 「数据寄存器 + 访问宽度 + 源/目标方向」*/
            if (decode_mmio_access(guest_rip, &acc) &&
                decode_matches_qual(qual, &acc)) {
                uint64_t val = acc.imm;      /* 立即数写 */

                if (acc.is_write && acc.reg != 0 && acc.imm == 0) {
                    uint64_t *src = x86_gpr_ptr(vcpu, acc.reg);
                    if (src)
                        val = *src;
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

        KLOG_ERROR("[VMX] EPT violation: gpa=0x%llx qual=0x%llx rip=0x%llx "
                   "(vcpu%d)\n",
                   gpa, qual, guest_rip, vcpu->vcpu_id);
        vmcs_write(GUEST_RIP, guest_rip + inst_len);
        return EL2_RESUME;
    }

    case VMX_REASON_EXC_NMI: {
        uint64_t intr_info = vmcs_read(EXI_INTR_INFO);
        uint8_t  vector    = (uint8_t)(intr_info & 0xFF);
        KLOG_ERROR("[VMX] EXCEPTION vector=%u rip=0x%llx (vcpu%d)\n",
                   vector, guest_rip, vcpu->vcpu_id);
        return EL2_EXIT;
    }

    default:
        KLOG_ERROR("[VMX] Unhandled exit reason=%u rip=0x%llx qual=0x%llx "
                   "(vcpu%d)\n",
                   reason, guest_rip, vmcs_read(EXI_QUALIFICATION),
                   vcpu->vcpu_id);
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
     * guest RAM（cfg.mem_size != 0）时建立。hpa_base 暂用 identity
     * （= mem_base），与 riscv64 gstage 一致。
     * 注：即使建表，也需 CPU_EXEC_CTRL1 的 CPU_EPT 位真正置位才生效
     * （见 vmcs_init_ctrl 协商结果）。*/
    if (vm->cfg.mem_size != 0) {
        x86_ept_init(vm->cfg.mem_base, vm->cfg.mem_size,
                     vm->cfg.mem_base /* hpa_base = identity */);
        /* 只映射 guest RAM，其余无效 → 设备访问触发 EPT violation → 模拟 */
        x86_ept_enable_mmio_trap();

        mmio_bus_init(&g_x86_mmio_bus);
        if (uart16550_init(&g_x86_uart_dev, &g_x86_mmio_bus) != 0)
            KLOG_WARN("[VMX] uart16550 registration failed\n");
        vm->mmio_bus = &g_x86_mmio_bus;

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

void vmm_arch_restore_guest_ctx(vcpu_t *vcpu)
{
    (void)vcpu;
    /* x86 VMCS 自动保存/恢复 guest 系统寄存器，无需手动操作 */
}

int vmm_arch_enter_guest(vcpu_t *vcpu)
{
    /* 重新装载该 vCPU 的 VMCS（yield 后调度回来时需要）*/
    vmcs_t *v = (vmcs_t *)g_vmcs_storage[vcpu->vcpu_id];
    uint64_t vmcs_pa = virt_to_phys(v);
    if (vmcs_load_pa(vmcs_pa)) {
        KLOG_ERROR("[VMX] vmptrld failed in vmm_arch_enter_guest (vcpu%d pa=0x%llx)\n",
                   vcpu->vcpu_id, vmcs_pa);
        return 0;
    }
    return vmx_enter_guest(vcpu);
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
