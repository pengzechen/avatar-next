/*
 * kernel/vmm/aarch64/el2_run.c — VMM 主循环与 VM exit 分发
 *
 * 对标 ref/bare-vm/arch/aarch64/el2_run.c，适配 Avatar OS。
 *
 * Phase 2 支持的 exit 类型（ESR_EL2.EC）：
 *   0x01 — WFI/WFE trap（HCR_EL2.TWI=1）：跳过指令，resume
 *   0x16 — HVC 超级调用：dispatcher 处理 HVC_DONE / HVC_PRINT
 *   0x24 — Stage-2 Data Abort：权限故障处理
 *   其余 — 未处理，打印诊断信息后退出
 */

#include "vmm.h"
#include "klog.h"
#include "string.h"
#include "mm_vm.h"
#include "aarch64/stage2.h"
#include "aarch64/sysreg.h"
#include "task/task.h"

/* ── guest 专用栈（每个 vCPU 独立）─────────────────────────── */
#define GUEST_STACK_SIZE  4096
static uint8_t g_guest_stacks[MAX_VCPUS][GUEST_STACK_SIZE]
    __attribute__((aligned(16)));

/*
 * el2_vcpu_setup — 初始化 AArch64 vCPU 软件 VMCS
 *
 * 设置 guest 初始状态，使得首次 el2_enter_guest 后：
 *   - guest PC = entry（guest EL1 入口）
 *   - guest PSTATE = EL1h, AArch64, IRQ/FIQ/SError 开
 *   - guest SP_EL1 = 每个 vcpu 独立的 4KB 栈顶
 *   - guest GPRs = 全 0
 */
int el2_vcpu_setup(vcpu_t *vcpu, void (*entry)(void))
{
    memset(vcpu->r, 0, sizeof(vcpu->r));

    /*
     * guest 入口地址：必须用物理地址。
     * Stage-2 以 IPA→PA identity map，EL1 MMU off 时 VA 直接作为 IPA，
     * 内核 VA (0xffff...) 超出 Stage-2 IPA 范围（T0SZ=32, 4GB），
     * 必须转换为 PA 才能被 Stage-2 正确映射。
     */
    vcpu->elr  = virt_to_phys((uint64_t)entry);

    /*
     * SPSR_EL2 for guest EL1h (AArch64):
     *   M[3:0] = 0b0101 = EL1h
     *   F=0, I=0, A=0, D=0  (IRQ/FIQ/SError/Debug 不屏蔽)
     * = 0x3C5: 保留所有中断使能，EL1h 模式
     * 注意：D bit(9)=1 防止首次进入时 debug exception，其余=0
     */
    vcpu->spsr = 0x3C5ULL;

    /* guest SP_EL1：每个 vcpu 独立的 4KB 栈 */
    uint32_t id = (uint32_t)vcpu->vcpu_id;
    if (id >= MAX_VCPUS) id = 0;
    /* SP_EL1 同样需要物理地址（EL1 MMU off 时直接用作 IPA）*/
    vcpu->sp_el1 = virt_to_phys((uint64_t)(g_guest_stacks[id] + GUEST_STACK_SIZE));

    KLOG_INFO("[VMM] el2_vcpu_setup: vcpu%d entry=0x%llx sp_el1=0x%llx spsr=0x%llx\n",
              vcpu->vcpu_id, vcpu->elr, vcpu->sp_el1, vcpu->spsr);
    return 0;
}

/* ── 读取 ESR / ELR / FAR / HPFAR ────────────────────────── */
static inline uint64_t read_esr_el2(void)   { return READ_ESR_EL2(); }
static inline uint64_t read_far_el2(void)   { return READ_FAR_EL2(); }
static inline uint64_t read_hpfar_el2(void) { return READ_HPFAR_EL2(); }

/* ── 步进 guest PC 越过陷入指令 ───────────────────────────── */
/*
 * el2_advance_pc — 使 vcpu->elr 跳过当前陷入指令
 * AArch64: ESR.IL(bit 25)=1 → 32-bit 指令(+4), =0 → 16-bit(+2, Thumb)
 * 注意：HVC 陷入时 ELR_EL2 硬件已指向 HVC 下一条，无需调用此函数。
 */
static void el2_advance_pc(vcpu_t *vcpu, uint64_t esr)
{
    vcpu->elr += ((esr >> 25) & 1) ? 4 : 2;
}

/* ── Test 1: WFI/WFE 陷入 ─────────────────────────────────── */
static int handle_wfi(vcpu_t *vcpu, uint64_t esr)
{
    KLOG_INFO("[VMM] WFI trap: ELR=0x%llx (vcpu%d)\n",
              vcpu->elr, vcpu->vcpu_id);
    el2_advance_pc(vcpu, esr);
    KLOG_INFO("[VMM] WFI handled, guest PC advanced to 0x%llx\n", vcpu->elr);
    return EL2_RESUME;
}

/* ── Stage-2 Data Abort（页权限故障）────────────────────── */
static int handle_dabt(vcpu_t *vcpu, uint64_t esr)
{
    uint64_t far   = read_far_el2();
    uint64_t hpfar = read_hpfar_el2();
    /* HPFAR[39:4] = IPA[47:12] → IPA_base = hpfar << 8 */
    uint64_t ipa   = (hpfar << 8) | (far & 0xFFFULL);
    int      wnr   = (esr >> 6) & 1;

    KLOG_INFO("[VMM] Stage-2 %s fault: IPA=0x%llx FAR=0x%llx ELR=0x%llx\n",
              wnr ? "write" : "read", ipa, far, vcpu->elr);
    /* 恢复写权限，guest 重试 */
    stage2_restore(ipa & ~0xFFFULL);
    KLOG_INFO("[VMM] Stage-2 permission restored, guest retries\n");
    return EL2_RESUME;
}

/* ── HVC 超级调用 ─────────────────────────────────────────── */
static int handle_hvc(vcpu_t *vcpu, uint64_t esr)
{
    (void)esr;
    uint64_t no = vcpu->r[0];   /* HVC number from guest x0 */

    switch (no) {
    case HVC_PRINT: {
        uint64_t iter = vcpu->r[1];
        /* 每 100 次打印一次，避免刷屏 */
        if (iter % 100 == 0)
            KLOG_INFO("[VMM] HVC_PRINT: iter=%llu (vcpu%d)\n",
                      iter, vcpu->vcpu_id);
        /* 主动 yield：让出 CPU 给其他线程 */
        task_yield();
        return EL2_RESUME;
    }

    case HVC_DONE: {
        uint64_t tests = vcpu->r[1];
        KLOG_INFO("[VMM] HVC_DONE: vcpu%d reports %llu tests complete\n",
                  vcpu->vcpu_id, tests);
        return EL2_VMEXIT;
    }

    default:
        KLOG_WARN("[VMM] Unknown HVC no=%llu (vcpu%d)\n",
                  no, vcpu->vcpu_id);
        return EL2_RESUME;
    }
}

/* ── 统一 exit 分发 ────────────────────────────────────────── */
static int vmm_exit_handler(vcpu_t *vcpu)
{
    uint64_t esr = read_esr_el2();
    uint32_t ec  = (uint32_t)(esr >> 26);

    switch (ec) {
    case 0x01:  return handle_wfi(vcpu, esr);   /* WFI/WFE            */
    case 0x16:  return handle_hvc(vcpu, esr);   /* HVC                */
    case 0x24:  return handle_dabt(vcpu, esr);  /* Stage-2 Data Abort */
    default:
        KLOG_ERROR("[VMM] Unhandled exit: EC=0x%x ESR=0x%llx ELR=0x%llx SPSR=0x%llx\n",
                   ec, esr, vcpu->elr, vcpu->spsr);
        for (int i = 0; i < 8; i += 2)
            KLOG_ERROR("  x%d=0x%llx  x%d=0x%llx\n",
                       i, vcpu->r[i], i+1, vcpu->r[i+1]);
        return EL2_EXIT;
    }
}

/* ── AArch64 VMM 架构钩子实现 ───────────────────────────────── */

void vmm_arch_restore_guest_ctx(vcpu_t *vcpu)
{
    /*
     * tpidr_el1 在 VHE 下是 host per-CPU 指针，与真实 EL1 寄存器共享，
     * 没有 _el12 别名。restore_sysregs_el12 会将 vcpu->sysregs 里的 0
     * 写入 TPIDR_EL1，导致 cpu_current() 返回 NULL 。
     * 必须在调用前后保存/恢复 host 的实际值。
     */
    uint64_t host_tpidr;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(host_tpidr));
    restore_sysregs_el12(vcpu->sysregs);
    __asm__ volatile("msr tpidr_el1, %0\nisb" :: "r"(host_tpidr) : "memory");
}

int vmm_arch_enter_guest(vcpu_t *vcpu)
{
    /*
     * VTCR_EL2 和 VTTBR_EL2 都是 per-CPU 寄存器。
     * stage2_init 只在 cpu0 上写了它们，次级核必须在进 guest 前重新写入：
     *   - VTCR 控制 Stage-2 翻译粒度（T0SZ/SL0 等），若为 0 则 T0SZ=0，
     *     需要 L0 页表，而我们只有 4-entry L1，立即 IFSC=0x04 fault。
     *   - VTTBR 指向根页表物理地址，为 0 时同样 fault。
     */
    __asm__ volatile(
        "msr vtcr_el2,  %0\n"
        "msr vttbr_el2, %1\n"
        "isb\n"
        :: "r"(vcpu->vm->vtcr), "r"(vcpu->page_table_base) : "memory"
    );

    /*
     * guest 运行期间可能修改 TPIDR_EL1（host/guest 共享该寄存器），
     * el2_trap_exit 不会自动恢复。必须在返回后恢复 host 値。
     */
    uint64_t host_tpidr;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(host_tpidr));
    el2_enter_guest(vcpu);
    __asm__ volatile("msr tpidr_el1, %0\nisb" :: "r"(host_tpidr) : "memory");
    return 1;   /* el2_enter_guest 总是成功返回（否则直接崩溃）*/
}

int vmm_arch_exit_handler(vcpu_t *vcpu)
{
    return vmm_exit_handler(vcpu);
}

void vmm_arch_save_guest_ctx(vcpu_t *vcpu)
{
    save_sysregs_el12(vcpu->sysregs);
}
