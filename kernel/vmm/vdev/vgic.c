/*
 * kernel/vmm/vdev/vgic.c — 虚拟 GICv2 中断注入核心实现
 *
 * 移植自 x-kernel: virt/kvmm/src/vdev/aarch64/vgic.rs，适配 Avatar OS：
 *   - Rust Atomic 位图 → 普通静态数组（世界切换窗口内串行访问）
 *   - UnsafeCell<Hw> → 普通静态结构（同上）
 *   - log::info → KLOG_DEBUG
 *
 * 见头文件「当前实现状态」：GICH 硬件写入需先映射窗口；未映射时只做
 * 软件侧整理，避免误写非法地址。
 */

#include "vmm_vgic.h"
#include "vmm_irq_route.h"   /* host_hwirq_for_guest_irq */
#include "klog.h"
#include "string.h"

/* SGI（0-15）恒使能掩码 */
#define SGI_MASK  0xffffULL

/* 首个由宿主后端支撑的 guest 中断（此前为虚拟 SGI/PPI）*/
#define FIRST_HOST_BACKED_GUEST_IRQ  17

/* ── 每 vCPU 核心状态 ─────────────────────────────────────── */
typedef struct {
    uint64_t pending;                /* 挂起位图 */
    uint16_t sgi_sources[16];        /* 每 SGI 的源 vCPU 位图 */
    uint64_t local_enabled;          /* banked SGI/PPI 使能（0-31）*/
    /* GICH 硬件状态缓存（退出时保存、进入时恢复，跨 pCPU 迁移用）*/
    uint32_t hw_vmcr;
    uint32_t hw_apr;
    uint32_t hw_lr[VGIC_MAX_LRS];
} vgic_core_t;

static vgic_core_t g_vgic[VGIC_MAX_VCPUS];
static uint64_t    g_vgic_shared_enabled;   /* SPI 使能（32-63）*/
static uint32_t    g_vgic_nr_vcpus;
static uintptr_t   g_vgic_gich_va;          /* 0 = 未映射 */

/* ── GICH 寄存器访问（仅在已映射时可用）───────────────────── */
static void gich_write(uint32_t off, uint32_t val)
{
    if (!g_vgic_gich_va)
        return;   /* 未映射：跳过，避免误写非法地址 */
    *(volatile uint32_t *)(g_vgic_gich_va + off) = val;
}

static uint32_t gich_read(uint32_t off)
{
    if (!g_vgic_gich_va)
        return 0;
    return *(volatile uint32_t *)(g_vgic_gich_va + off);
}

/* ── 初始化 ───────────────────────────────────────────────── */
int vmm_vgic_init(uint32_t nr_vcpus)
{
    if (nr_vcpus < 1)
        nr_vcpus = 1;
    if (nr_vcpus > VGIC_MAX_VCPUS)
        nr_vcpus = VGIC_MAX_VCPUS;

    memset(g_vgic, 0, sizeof(g_vgic));
    g_vgic_nr_vcpus = nr_vcpus;

    for (uint32_t i = 0; i < VGIC_MAX_VCPUS; i++)
        g_vgic[i].local_enabled = SGI_MASK;   /* SGI 恒使能 */

    KLOG_INFO("[vgic] virtual GICv2 ready (%u vCPU, %u LR)\n",
              nr_vcpus, (unsigned)VGIC_MAX_LRS);
    return 0;
}

void vmm_vgic_set_gich_base(uintptr_t gich_va)
{
    g_vgic_gich_va = gich_va;
    KLOG_INFO("[vgic] GICH base = 0x%lx (%s)\n", (unsigned long)gich_va,
              gich_va ? "hardware sync enabled" : "software-only mode");
}

/* ── 挂起 / 使能 ──────────────────────────────────────────── */
void vmm_vgic_set_pending(uint32_t vcpu_id, uint32_t irq)
{
    if (vcpu_id >= g_vgic_nr_vcpus || irq >= VGIC_MAX_IRQS)
        return;

    if (irq < 16) {
        /* SGI：记录源 vCPU（单 vCPU 场景下源为 0）*/
        g_vgic[vcpu_id].sgi_sources[irq] |= (uint16_t)(1u << 0);
    }
    g_vgic[vcpu_id].pending |= (1ULL << irq);
}

void vmm_vgic_set_enabled(uint32_t vcpu_id, uint32_t irq, int enabled)
{
    if (irq >= VGIC_MAX_IRQS || irq < 16)
        return;   /* SGI 恒使能，不可改 */

    uint64_t mask = 1ULL << irq;

    if (irq < 32) {
        if (vcpu_id >= g_vgic_nr_vcpus)
            return;
        if (enabled)
            g_vgic[vcpu_id].local_enabled |= mask;
        else
            g_vgic[vcpu_id].local_enabled &= ~mask;
    } else {
        if (enabled)
            g_vgic_shared_enabled |= mask;
        else
            g_vgic_shared_enabled &= ~mask;
    }
}

/* ── LR 值构造 ────────────────────────────────────────────── */
static uint32_t pending_lr_value(uint32_t irq, uint32_t source_vcpu)
{
    uint32_t lr = LR_PRIORITY | LR_STATE_PENDING | irq;

    if (irq < FIRST_HOST_BACKED_GUEST_IRQ) {
        if (irq < 16)
            lr |= (source_vcpu & 0x7) << LR_SGI_SRC_SHIFT;
        return lr;
    }
    /* 宿主后端中断（如 vtimer）：若 irq_route 已注册宿主路由，
     * 则置 LR 的 HW 位并带上宿主 INTID，由硬件在 ack 时直接出中断。*/
    uint32_t hwirq = vmm_irq_route_host_hwirq_for_guest_irq(irq);
    if (hwirq != 0)
        lr |= LR_HW | (hwirq << LR_PHYSID_SHIFT);

    return lr;
}

/* ── 进入 guest：整理挂起中断进 LR ────────────────────────── */
void vmm_vgic_sync_entry(uint32_t vcpu_id)
{
    if (vcpu_id >= g_vgic_nr_vcpus)
        return;

    vgic_core_t *core = &g_vgic[vcpu_id];
    uint32_t *lr = core->hw_lr;

    /* 只把「已使能」的挂起位排入空闲 LR；未使能的保持挂起，
     * guest 使能后即可投递。*/
    uint64_t enabled = (core->local_enabled | SGI_MASK) | g_vgic_shared_enabled;
    uint64_t pending = core->pending & enabled;

    while (pending != 0) {
        /* 取最低置位对应的中断号 */
        uint32_t irq = 0;
        uint64_t t = pending & (~pending + 1);
        while (t > 1) { t >>= 1; irq++; }
        pending &= pending - 1;

        if (irq < 16) {
            /* SGI：按源逐个排队 */
            uint16_t sources = core->sgi_sources[irq];
            while (sources != 0) {
                uint32_t src = 0;
                uint16_t st = (uint16_t)(sources & (~sources + 1));
                while (st > 1) { st >>= 1; src++; }
                sources &= sources - 1;

                /* 已在该 LR 中排队则跳过 */
                int queued = 0;
                for (int i = 0; i < VGIC_MAX_LRS; i++) {
                    if ((lr[i] & LR_VINTID_MASK) == irq &&
                        ((lr[i] >> LR_SGI_SRC_SHIFT) & 0x7) == src &&
                        (lr[i] & LR_STATE_MASK) != 0) {
                        queued = 1;
                        break;
                    }
                }
                if (queued)
                    continue;

                int slot = -1;
                for (int i = 0; i < VGIC_MAX_LRS; i++) {
                    if ((lr[i] & LR_STATE_MASK) == 0) { slot = i; break; }
                }
                if (slot < 0)
                    break;   /* 无空闲 LR */

                core->sgi_sources[irq] &= (uint16_t)~(1u << src);
                if (core->sgi_sources[irq] == 0)
                    core->pending &= ~(1ULL << irq);
                lr[slot] = pending_lr_value(irq, src);
            }
            continue;
        }

        /* 非 SGI：已排队则跳过 */
        int queued = 0;
        for (int i = 0; i < VGIC_MAX_LRS; i++) {
            if ((lr[i] & LR_VINTID_MASK) == irq && (lr[i] & LR_STATE_MASK) != 0) {
                queued = 1;
                break;
            }
        }
        if (queued)
            continue;

        int slot = -1;
        for (int i = 0; i < VGIC_MAX_LRS; i++) {
            if ((lr[i] & LR_STATE_MASK) == 0) { slot = i; break; }
        }
        if (slot < 0)
            break;

        core->pending &= ~(1ULL << irq);
        lr[slot] = pending_lr_value(irq, 0);
    }

    /* 写入 GICH（仅在已映射 GICH 窗口时生效）*/
    gich_write(GICH_VMCR, core->hw_vmcr);
    gich_write(GICH_APR, core->hw_apr);
    for (int i = 0; i < VGIC_MAX_LRS; i++)
        gich_write(GICH_LR0 + (uint32_t)i * 4, lr[i]);
    gich_write(GICH_HCR, GICH_HCR_EN);
}

/* ── 退出 guest：保存并清空 GICH 状态 ─────────────────────── */
void vmm_vgic_sync_exit(uint32_t vcpu_id)
{
    if (vcpu_id >= g_vgic_nr_vcpus)
        return;

    vgic_core_t *core = &g_vgic[vcpu_id];
    uint32_t *lr = core->hw_lr;

    core->hw_vmcr = gich_read(GICH_VMCR);
    core->hw_apr  = gich_read(GICH_APR);

    uint32_t elsr0 = gich_read(GICH_ELSR0);

    for (int i = 0; i < VGIC_MAX_LRS; i++) {
        lr[i] = gich_read(GICH_LR0 + (uint32_t)i * 4);
        uint32_t irq = lr[i] & LR_VINTID_MASK;

        /* 未投递完的 SGI 重新挂起，供下次进入继续投递 */
        if (irq < 16 && (lr[i] & LR_STATE_MASK) != 0) {
            if (lr[i] & LR_STATE_PENDING) {
                uint32_t src = (lr[i] >> LR_SGI_SRC_SHIFT) & 0x7;
                core->sgi_sources[irq] |= (uint16_t)(1u << src);
                core->pending |= (1ULL << irq);
            }
            lr[i] = 0;
            gich_write(GICH_LR0 + (uint32_t)i * 4, 0);
            continue;
        }
        /* 清空非活动 LR：残留 VINTID + 活跃 LR 同 ID 在 GICv2 下未定义 */
        if (elsr0 & (1u << i))
            lr[i] = 0;
        gich_write(GICH_LR0 + (uint32_t)i * 4, 0);
    }

    gich_write(GICH_VMCR, 0);
    gich_write(GICH_APR, 0);
    gich_write(GICH_HCR, 0);
}
