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
#include "aarch64/stage2.h"
#include "aarch64/sysreg.h"
#include "task/task.h"

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
    restore_sysregs_el12(vcpu->sysregs);
}

int vmm_arch_enter_guest(vcpu_t *vcpu)
{
    el2_enter_guest(vcpu);
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
