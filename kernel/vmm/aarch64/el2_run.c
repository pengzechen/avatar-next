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
#include "task/switch.h"
#include "vmm_mmio.h"      /* MMIO 总线分发 */
#include "vmm_vgic.h"      /* vGIC 中断注入 */
#include "vmm_irq_route.h" /* 宿主 IRQ → vCPU 任务唤醒 */

/* ── 读取 ESR / ELR / FAR / HPFAR ────────────────────────── */
static inline uint64_t read_esr_el2(void)   { return READ_ESR_EL2(); }
extern void handle_irq_exception(uint64_t *stack_pointer);

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

/* ── Trapped AArch64 system-register access ───────────────────────────
 * HCR_EL2.TID3 traps EL1 reads of ID registers. Do not expose the host
 * CPU feature matrix directly: Linux combines many ID fields into arm64
 * capabilities, and QEMU/host values can advertise duplicate or unsupported
 * virtual features. Present a small, coherent ARMv8.0 profile instead. */
#define SYSREG_ID(op0, op1, crn, crm, op2) \
    ((((op0) & 3u) << 14) | (((op1) & 7u) << 11) | \
     (((crn) & 15u) << 7) | (((crm) & 15u) << 3) | ((op2) & 7u))

#define ID_AA64PFR0_FP_SIMD  ((0ULL << 16) | (0ULL << 20))
#define ID_AA64PFR0_ELS      ((1ULL << 0) | (1ULL << 4))
#define ID_AA64MMFR0_BASE    ((1ULL << 0) | (0xFULL << 20) | (0xFULL << 24))

static uint64_t aarch64_vcpu_idreg_read(uint32_t key)
{
    switch (key) {
    case SYSREG_ID(3, 0, 0, 0, 0): return 0x410fd0b0ULL; /* MIDR: Cortex-A76-like */
    case SYSREG_ID(3, 0, 0, 0, 5): return 0x80000000ULL; /* MPIDR: uniprocessor */
    case SYSREG_ID(3, 0, 0, 0, 6): return 0;            /* REVIDR */

    case SYSREG_ID(3, 0, 0, 4, 0): return ID_AA64PFR0_ELS | ID_AA64PFR0_FP_SIMD;
    case SYSREG_ID(3, 0, 0, 4, 1): return 0; /* ID_AA64PFR1_EL1 */
    case SYSREG_ID(3, 0, 0, 4, 4): return 0; /* ID_AA64ZFR0_EL1 */
    case SYSREG_ID(3, 0, 0, 4, 5): return 0; /* ID_AA64SMFR0_EL1 */

    case SYSREG_ID(3, 0, 0, 5, 0): return 0x0000000000000006ULL; /* ID_AA64DFR0_EL1 */
    case SYSREG_ID(3, 0, 0, 5, 1): return 0; /* ID_AA64DFR1_EL1 */

    case SYSREG_ID(3, 0, 0, 6, 0): return 0; /* ID_AA64ISAR0_EL1 */
    case SYSREG_ID(3, 0, 0, 6, 1): return 0; /* ID_AA64ISAR1_EL1: no PAuth */
    case SYSREG_ID(3, 0, 0, 6, 2): return 0; /* ID_AA64ISAR2_EL1: no newer PAuth */

    case SYSREG_ID(3, 0, 0, 7, 0): return ID_AA64MMFR0_BASE;
    case SYSREG_ID(3, 0, 0, 7, 1): return 0; /* ID_AA64MMFR1_EL1 */
    case SYSREG_ID(3, 0, 0, 7, 2): return 0; /* ID_AA64MMFR2_EL1 */
    case SYSREG_ID(3, 0, 0, 7, 3): return 0; /* ID_AA64MMFR3_EL1 */

    case SYSREG_ID(3, 0, 0, 1, 0): return 0; /* ID_PFR0_EL1 */
    case SYSREG_ID(3, 0, 0, 1, 1): return 0; /* ID_PFR1_EL1 */
    case SYSREG_ID(3, 0, 0, 1, 2): return 0; /* ID_DFR0_EL1 */
    case SYSREG_ID(3, 0, 0, 1, 3): return 0; /* ID_AFR0_EL1 */
    case SYSREG_ID(3, 0, 0, 1, 4): return 0; /* ID_MMFR0_EL1 */
    case SYSREG_ID(3, 0, 0, 1, 5): return 0; /* ID_MMFR1_EL1 */
    case SYSREG_ID(3, 0, 0, 1, 6): return 0; /* ID_MMFR2_EL1 */
    case SYSREG_ID(3, 0, 0, 1, 7): return 0; /* ID_MMFR3_EL1 */
    case SYSREG_ID(3, 0, 0, 2, 0): return 0; /* ID_ISAR0_EL1 */
    case SYSREG_ID(3, 0, 0, 2, 1): return 0; /* ID_ISAR1_EL1 */
    case SYSREG_ID(3, 0, 0, 2, 2): return 0; /* ID_ISAR2_EL1 */
    case SYSREG_ID(3, 0, 0, 2, 3): return 0; /* ID_ISAR3_EL1 */
    case SYSREG_ID(3, 0, 0, 2, 4): return 0; /* ID_ISAR4_EL1 */
    case SYSREG_ID(3, 0, 0, 2, 5): return 0; /* ID_ISAR5_EL1 */
    case SYSREG_ID(3, 0, 0, 2, 6): return 0; /* ID_MMFR4_EL1 */
    case SYSREG_ID(3, 0, 0, 2, 7): return 0; /* ID_ISAR6_EL1 */
    case SYSREG_ID(3, 0, 0, 3, 0): return 0; /* MVFR0_EL1 */
    case SYSREG_ID(3, 0, 0, 3, 1): return 0; /* MVFR1_EL1 */
    case SYSREG_ID(3, 0, 0, 3, 2): return 0; /* MVFR2_EL1 */
    case SYSREG_ID(3, 0, 0, 3, 4): return 0; /* ID_PFR2_EL1 */
    case SYSREG_ID(3, 0, 0, 3, 5): return 0; /* ID_DFR1_EL1 */
    case SYSREG_ID(3, 0, 0, 3, 6): return 0; /* ID_MMFR5_EL1 */
    default: return 0;
    }
}

static int handle_sysreg(vcpu_t *vcpu, uint64_t esr)
{
    uint32_t op0 = (uint32_t)((esr >> 20) & 0x3);
    uint32_t op2 = (uint32_t)((esr >> 17) & 0x7);
    uint32_t op1 = (uint32_t)((esr >> 14) & 0x7);
    uint32_t crn = (uint32_t)((esr >> 10) & 0xF);
    uint32_t rt  = (uint32_t)((esr >> 5) & 0x1F);
    uint32_t crm = (uint32_t)((esr >> 1) & 0xF);
    uint32_t dir = (uint32_t)(esr & 1);
    uint32_t key = SYSREG_ID(op0, op1, crn, crm, op2);

    if (dir && op0 == 3 && crn == 0) {
        if (rt != 31)
            vcpu->r[rt] = aarch64_vcpu_idreg_read(key);
        el2_advance_pc(vcpu, esr);
        return EL2_RESUME;
    }

    KLOG_WARN("[VMM] Unhandled sysreg %s op0=%u op1=%u crn=%u crm=%u op2=%u rt=%u\n",
              dir ? "read" : "write", op0, op1, crn, crm, op2, rt);
    el2_advance_pc(vcpu, esr);
    return EL2_RESUME;
}

/* ── Test 1: WFI/WFE 陷入 ─────────────────────────────────── */
static int handle_wfi(vcpu_t *vcpu, uint64_t esr)
{
    /* WFI 在网络/定时器驱动的 guest 里会高频触发（guest 空闲时几乎持续
     * 执行 WFI）。之前每条都打 INFO 日志，实测 25 秒产生 30 万行，会把
     * guest 控制台输出彻底淹没，故降为 DEBUG。*/
    static uint64_t wfi_count;

    if ((++wfi_count & 0x3FFFF) == 1) {
        KLOG_INFO("[VMM] guest idle heartbeat: wfi=%llu ELR=0x%llx (vcpu%d)\n",
                  (unsigned long long)wfi_count,
                  (unsigned long long)vcpu->elr, vcpu->vcpu_id);
    }

    el2_advance_pc(vcpu, esr);
    return EL2_RESUME;
}

/* ── Stage-2 Data Abort ──────────────────────────────────────
 *
 * 两种来源：
 *   1) 设备 MMIO：stage2_enable_mmio_trap() 把非 RAM 区间置为无效，
 *      guest 访问设备 → 此处分发到 MMIO 总线（移植自 kvmm stage2.rs +
 *      el2 的 data-abort 处理）。
 *   2) 权限故障：Stage-2 页被 stage2_set_ro() 设为只读，写触发故障 →
 *      恢复写权限让 guest 重试（既有行为，保持不变）。
 *
 * 判别：先尝试 MMIO 总线；命中则由设备处理并步进 guest PC，否则按权限
 * 故障恢复。Data Abort 陷入时 ELR_EL2 指向**出错指令**（需步进）。
 */
static int handle_dabt(vcpu_t *vcpu, uint64_t esr)
{
    uint64_t far   = vcpu->far;
    uint64_t hpfar = vcpu->hpfar;
    /* HPFAR[39:4] = IPA[47:12] → IPA_base = hpfar << 8 */
    uint64_t ipa   = (hpfar << 8) | (far & 0xFFFULL);
    int      wnr   = (esr >> 6) & 1;

    /* 1) 先尝试 MMIO 设备分发 */
    if (vcpu->vm && vcpu->vm->mmio_bus) {
        uint64_t out = 0;
        /* 访问宽度：ESR.ISS.SAS[23:22]（0=byte,1=half,2=word,3=dword）*/
        uint8_t size = (uint8_t)(1u << ((esr >> 22) & 3));
        /* 写数据：从 ISS.SRT[20:16] 指定的通用寄存器取 */
        uint64_t val = 0;
        if (wnr) {
            uint32_t srt = (uint32_t)((esr >> 16) & 0x1F);
            val = vcpu->r[srt];
        }

        if (mmio_bus_handle(vcpu->vm->mmio_bus, ipa, wnr, size, val,
                            (uint32_t)vcpu->vcpu_id, &out)) {
            if (!wnr) {
                uint32_t srt = (uint32_t)((esr >> 16) & 0x1F);
                if (srt != 31)   /* XZR 丢弃 */
                    vcpu->r[srt] = out;
            }
            el2_advance_pc(vcpu, esr);
            return EL2_RESUME;
        }
    }

    /* x-kernel behavior: unmapped MMIO reads return 0 and writes are ignored.
     * Linux probes optional devices early; retrying a non-emulated IPA would
     * fault forever. */
    if (!wnr) {
        uint32_t srt = (uint32_t)((esr >> 16) & 0x1F);
        if (srt != 31)
            vcpu->r[srt] = 0;
    }
    el2_advance_pc(vcpu, esr);
    return EL2_RESUME;
}

/* ── 虚拟 PSCI（guest CPU/电源管理）───────────────────────────
 *
 * 移植自 x-kernel vdev/aarch64/vpsci.rs。guest 通过 HVC 或 SMC 发起 PSCI 调用，
 * x0=function ID（0x8400_00xx=32 位调用约定 / 0xC400_00xx=64 位）。
 * 返回 EL2_RESUME（继续）或 EL2_VMEXIT（SYSTEM_OFF/RESET）。
 */
#define PSCI_VERSION_FID    0x84000000ULL
#define PSCI_CPU_ON_32      0x84000003ULL
#define PSCI_CPU_ON_64      0xC4000003ULL
#define PSCI_SYSTEM_OFF     0x84000008ULL
#define PSCI_SYSTEM_RESET   0x84000009ULL
#define PSCI_FEATURES_FID   0x8400000AULL

#define PSCI_RET_SUCCESS        0ULL
#define PSCI_RET_NOT_SUPPORTED  ((uint64_t)-1)
#define PSCI_RET_INVALID_PARAMS ((uint64_t)-2)
#define PSCI_RET_ALREADY_ON     ((uint64_t)-4)

static int psci_fid_supported(uint64_t fid)
{
    switch (fid) {
    case PSCI_VERSION_FID: case PSCI_CPU_ON_32: case PSCI_CPU_ON_64:
    case PSCI_SYSTEM_OFF:  case PSCI_SYSTEM_RESET: case PSCI_FEATURES_FID:
        return 1;
    default:
        return 0;
    }
}

/* 返回 EL2_RESUME / EL2_VMEXIT；非 PSCI（前缀不符）时返回 -1 表示未处理 */
static int handle_psci(vcpu_t *vcpu)
{
    uint64_t fid    = vcpu->r[0];
    uint8_t  prefix = (uint8_t)(fid >> 24);

    if (prefix != 0x84 && prefix != 0xC4)
        return -1;   /* 非 PSCI 调用 */

    switch (fid) {
    case PSCI_VERSION_FID:
        vcpu->r[0] = 0x00000002;   /* PSCI v0.2 */
        return EL2_RESUME;

    case PSCI_CPU_ON_32:
    case PSCI_CPU_ON_64:
        /* 多 vCPU guest 拉起：当前单 vCPU 模型未支持，忠实记录后返回。
         * TODO(Phase 3): 为 target_cpu 创建第二个 vcpu_task。 */
        KLOG_WARN("[vpsci] CPU_ON target=0x%llx entry=0x%llx not supported (single vCPU)\n",
                  (unsigned long long)vcpu->r[1], (unsigned long long)vcpu->r[2]);
        vcpu->r[0] = PSCI_RET_NOT_SUPPORTED;
        return EL2_RESUME;

    case PSCI_SYSTEM_OFF:
    case PSCI_SYSTEM_RESET:
        KLOG_INFO("[vpsci] guest shutdown fid=0x%llx (vcpu%d)\n",
                  (unsigned long long)fid, vcpu->vcpu_id);
        return EL2_VMEXIT;

    case PSCI_FEATURES_FID:
        vcpu->r[0] = psci_fid_supported(vcpu->r[1])
                     ? PSCI_RET_SUCCESS : PSCI_RET_NOT_SUPPORTED;
        return EL2_RESUME;

    default:
        KLOG_DEBUG("[vpsci] unsupported function 0x%llx\n", (unsigned long long)fid);
        vcpu->r[0] = PSCI_RET_NOT_SUPPORTED;
        return EL2_RESUME;
    }
}

/* ── HVC 超级调用 ─────────────────────────────────────────── */
static int handle_hvc(vcpu_t *vcpu, uint64_t esr)
{
    (void)esr;
    uint64_t no = vcpu->r[0];   /* HVC number from guest x0 */

    /* 先尝试 PSCI（function ID 前缀 0x84/0xC4，与 HVC_PRINT/DONE 不冲突）*/
    int psci = handle_psci(vcpu);
    if (psci != -1)
        return psci;

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
    uint64_t esr = vcpu->esr;
    uint32_t ec  = (uint32_t)(esr >> 26);

    if ((vcpu->exit_type & 0xFF) != 0) {
        static uint64_t async_count;
        if (((++async_count) & 0xFFFF) == 1) {
            KLOG_INFO("[VMM] async exit type=%llu count=%llu ELR=0x%llx\n",
                      (unsigned long long)vcpu->exit_type,
                      (unsigned long long)async_count,
                      (unsigned long long)vcpu->elr);
        }
        return EL2_RESUME;
    }

    switch (ec) {
    case 0x01:  return handle_wfi(vcpu, esr);   /* WFI/WFE            */
    case 0x16:  return handle_hvc(vcpu, esr);   /* HVC                */
    case 0x17: {                                /* SMC (PSCI conduit) */
        /* SMC 陷入时 ELR_EL2 指向 SMC 指令本身，需手动步进；
         * 而 HVC 陷入时硬件已指向下一条。 */
        int r = handle_psci(vcpu);
        if (r == -1) {
            KLOG_WARN("[VMM] Unhandled SMC fid=0x%llx (vcpu%d)\n",
                      (unsigned long long)vcpu->r[0], vcpu->vcpu_id);
            vcpu->r[0] = PSCI_RET_NOT_SUPPORTED;
            r = EL2_RESUME;
        }
        if (r == EL2_RESUME)
            el2_advance_pc(vcpu, esr);
        return r;
    }
    case 0x18:  return handle_sysreg(vcpu, esr); /* trapped sysreg     */
    case 0x24:  return handle_dabt(vcpu, esr);   /* Stage-2 Data Abort */
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

/*
 * ── guest 虚拟定时器（vtimer）投递 ──────────────────────────
 *
 * 移植自 x-kernel vdev/aarch64/vtimer.rs 的 check_vtimer：
 *   1. 读 guest 的 CNTV_CTL，未使能或被屏蔽（IMASK）则无到期
 *   2. 到期判据：CNTPCT_EL0 >= CNTV_CVAL + CNTVOFF_EL2
 *      （CVAL 在虚拟计数域，加偏移换算到物理计数域）
 *   3. 到期则把 PPI 27 注入 guest（经 vGIC）
 *
 * 注：kvmm 在**世界切换出口存根**里保存 CNTV_CTL/CVAL 到 vcpu，此处直接
 * 读当前寄存器——在单 vCPU、且 VMM 与 guest 同核运行的 avatar 模型下等价。
 * 若将来支持 vCPU 跨核迁移，需改为在 vcpu_t 中保存/恢复这两个值。
 */
#define VTIMER_PPI_IRQ      27
#define CNTV_CTL_ENABLE     (1ULL << 0)
#define CNTV_CTL_IMASK      (1ULL << 1)

static void aarch64_check_vtimer(vcpu_t *vcpu)
{
    uint64_t ctl = vcpu->cntv_ctl;

    if ((ctl & CNTV_CTL_ENABLE) == 0 || (ctl & CNTV_CTL_IMASK) != 0)
        return;

    uint64_t now  = READ_CNTPCT_EL0();
    uint64_t cval = vcpu->cntv_cval;
    uint64_t off  = READ_CNTVOFF_EL2();

    if (now >= cval + off) {
        static uint64_t inject_count;
        if (((++inject_count) & 0xFFFF) == 1) {
            KLOG_INFO("[VMM] inject vtimer count=%llu ctl=0x%llx cval=0x%llx now=0x%llx off=0x%llx\n",
                       (unsigned long long)inject_count,
                       (unsigned long long)ctl,
                       (unsigned long long)cval,
                       (unsigned long long)now,
                       (unsigned long long)off);
        }
        vmm_vgic_set_pending((uint32_t)vcpu->vcpu_id, VTIMER_PPI_IRQ);
    }
}

void vmm_arch_restore_guest_ctx(vcpu_t *vcpu)
{
    uint64_t vmpidr = (1ULL << 31) | (uint64_t)vcpu->vcpu_id;
    __asm__ volatile("msr vmpidr_el2, %0" :: "r"(vmpidr) : "memory");

    restore_sysregs_el12(vcpu->sysregs);

    /* 对标 kvmm HostVtimerHook::on_entry：先登记「本 pCPU 的 vCPU 承载任务 /
     * vCPU id」，这样宿主 vtimer 中断到来时 ISR 能直接注入虚拟中断；
     * 再检查定时器到期并同步 vGIC。*/
    vmm_irq_route_publish_owner();
    vmm_irq_route_publish_vcpu((uint32_t)vcpu->vcpu_id);
    aarch64_check_vtimer(vcpu);

    /* vIRQ：把可投递中断排入 GICH LR，让硬件产生虚拟 IRQ。*/
    vmm_vgic_sync_entry((uint32_t)vcpu->vcpu_id);
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
    vmm_vgic_sync_exit((uint32_t)vcpu->vcpu_id);
}
