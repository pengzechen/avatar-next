/*
 * kernel/vmm/vdev/vgicv3/vgicv3.c — VM 级虚拟 GICv3 核心
 *
 * 职责划分（对应 GICv2 版的 vgic.c + vgicc.c 的 LR 部分）：
 *   - 维护 guest 中断的 pending / enabled / active 软件状态
 *   - 把这些状态通过 ICH_LR<n>_EL2 推给硬件（guest 侧由 ICV_* 直接消费）
 *   - 退出 guest 后从 LR.State / ICH_ELRSR_EL2 回推软件状态
 *
 * GICv2 版是「VMM 替代 guest 应答」（GICC_IAR 被陷入后由软件返回 IRQ 号）；
 * GICv3 的 CPU interface 是系统寄存器，VMM 无法陷入，只能做上面这套
 * 「写入 LR + 回读状态」的硬件辅助模型。
 */

#include "vmm/vmm_vgicv3.h"
#include "vmm/vmm_irq_route.h"
#include "irq/gicv3.h"
#include "klog.h"
#include "string.h"
#include "aarch64/sysreg.h"

#define SGI_MASK 0xffffu

static int valid_vcpu(const vgic3_t *vgic, uint32_t vcpu_id)
{
    return vgic && vcpu_id < vgic->nr_vcpus;
}

static int valid_irq(uint32_t irq)
{
    return irq < VGIC3_MAX_IRQS;
}

static uint32_t irq_bit(uint32_t irq)
{
    return 1u << (irq & 31u);
}

/* ── LR 访问 ─────────────────────────────────────────────────── */

static uint32_t lr_vintid(uint64_t lr)
{
    return (uint32_t)(lr & ICH_LR_VINTID_MASK);
}

/* 返回 State 字段的原始取值（0..3），不是掩码 */
static uint32_t lr_state(uint64_t lr)
{
    return (uint32_t)((lr >> ICH_LR_STATE_SHIFT) & 3u);
}

/*
 * vgic3_lr_value — 组一个「纯软件注入」的 Group1 中断 LR
 *
 * State=Pending，HW=0（没有对应的物理中断需要硬件联动），
 * 优先级取 guest 自己给该 INTID 配的值 —— guest 的 ICH_VMCR_EL2.VPMR
 * 也是它自己写的，两者同源才能保证「配置合法就该投递」。
 */
static uint64_t vgic3_lr_value(const vgic3_t *vgic, uint32_t vcpu_id,
                               uint32_t irq)
{
    uint8_t prio = VGIC3_DEFAULT_PRIO;

    if (irq < 32)
        prio = vgic->vcpu[vcpu_id].prio0[irq];
    else if (irq < VGIC3D_REG_SIZE - 0x400)
        prio = vgic->dist_regs[0x400 + irq];

    return ((uint64_t)ICH_LR_ST_PENDING << ICH_LR_STATE_SHIFT) |
           ICH_LR_GROUP1 |
           ((uint64_t)prio << ICH_LR_PRIO_SHIFT) |
           ((uint64_t)irq & ICH_LR_VINTID_MASK);
}

static int lr_has_irq(const vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq)
{
    const vgic3_vcpu_t *vcpu = &vgic->vcpu[vcpu_id];

    for (uint32_t i = 0; i < _gicv3.nr_lrs && i < VGIC3_MAX_LRS; i++)
        if (vcpu->lr[i] && lr_vintid(vcpu->lr[i]) == irq)
            return 1;
    return 0;
}

static int lr_empty_slot(const vgic3_t *vgic, uint32_t vcpu_id)
{
    const vgic3_vcpu_t *vcpu = &vgic->vcpu[vcpu_id];
    uint32_t n = _gicv3.nr_lrs;

    if (n > VGIC3_MAX_LRS)
        n = VGIC3_MAX_LRS;

    for (uint32_t i = 0; i < n; i++)
        if (vcpu->lr[i] == 0)
            return (int)i;
    return -1;
}

/* ── 初始化 ─────────────────────────────────────────────────── */

int vmm_vgic3_init(vgic3_t *vgic, uint32_t nr_vcpus)
{
    if (!vgic)
        return -1;
    if (nr_vcpus < 1)
        nr_vcpus = 1;
    if (nr_vcpus > VGIC3_MAX_VCPUS)
        nr_vcpus = VGIC3_MAX_VCPUS;

    memset(vgic, 0, sizeof(*vgic));
    vgic->nr_vcpus = nr_vcpus;

    for (uint32_t i = 0; i < VGIC3_MAX_VCPUS; i++) {
        /* SGIs 永远使能（GICv3 下 SGI 的使能位同样是 banked 的，但 Linux
         * 从不单独使能 SGI，GICD_TYPER 也不报告它们）；PPI 初始为禁止。*/
        vgic->vcpu[i].enabled0 = SGI_MASK;
        memset(vgic->vcpu[i].prio0, VGIC3_DEFAULT_PRIO, 32);
        vgic->vcpu[i].sgi_igroupr0 = 0xFFFFFFFFu;
        vgic->vcpu[i].rd_waker = GICR_WAKER_PROCESSOR_SLEEP;
    }

    KLOG_INFO("[vgicv3] VM vGIC ready (%u vCPU, %u IRQs, %u LR)\n",
              nr_vcpus, (unsigned)VGIC3_MAX_IRQS, (unsigned)_gicv3.nr_lrs);
    return 0;
}

/*
 * vmm_vgic3_hw_init — 打开 ICH_HCR_EL2.En
 *
 * 不开这个位，QEMU / 硬件在任何 ICC_IAR1_EL1 读上都只会返回 spurious，
 * guest 永远收不到虚拟中断。
 */
void vmm_vgic3_hw_init(void)
{
    uint64_t hcr = gicv3_read_hcr();

    /* En=1；维护中断相关位保持 0 —— 我们在每次 VM exit 主动轮询
     * ICH_ELRSR_EL2/ICH_EISR_EL2，不需要 guest EOI 时打断自己。*/
    hcr |= ICH_HCR_EN;
    hcr &= ~(uint64_t)ICH_HCR_UIEN;
    gicv3_write_hcr(hcr);

    KLOG_INFO("[vgicv3] ICH_HCR_EL2=0x%llx (En=1, %u LRs)\n",
              (unsigned long long)gicv3_read_hcr(), (unsigned)_gicv3.nr_lrs);
}

/* ── 状态写入接口 ───────────────────────────────────────────── */

void vmm_vgic3_set_dist_enabled(vgic3_t *vgic, int enabled)
{
    if (vgic)
        vgic->dist_enabled = enabled ? 1 : 0;
}

int vmm_vgic3_dist_enabled(const vgic3_t *vgic)
{
    return vgic ? vgic->dist_enabled : 0;
}

void vmm_vgic3_set_sgi_pending(vgic3_t *vgic, uint32_t vcpu_id,
                               uint32_t source_vcpu, uint32_t irq)
{
    if (!valid_vcpu(vgic, vcpu_id) || irq >= 16)
        return;
    if (source_vcpu >= VGIC3_MAX_VCPUS)
        source_vcpu = 0;

    vgic->vcpu[vcpu_id].sgi_sources[irq] |= (uint16_t)(1u << source_vcpu);
    vgic->vcpu[vcpu_id].pending0 |= irq_bit(irq);
}

void vmm_vgic3_set_pending(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq)
{
    if (!vgic || !valid_irq(irq) || !valid_vcpu(vgic, vcpu_id))
        return;

    if (irq < 16) {
        vmm_vgic3_set_sgi_pending(vgic, vcpu_id, 0, irq);
    } else if (irq < 32) {
        vgic->vcpu[vcpu_id].pending0 |= irq_bit(irq);
    } else {
        vgic->spi_pending[vcpu_id][irq / 32] |= irq_bit(irq);
    }
}

void vmm_vgic3_set_enabled(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq,
                           int enabled)
{
    if (!vgic || !valid_irq(irq) || irq < 16)
        return;   /* SGI 使能位不参与投递判定，忽略 */

    if (irq < 32) {
        if (!valid_vcpu(vgic, vcpu_id))
            return;
        if (enabled)
            vgic->vcpu[vcpu_id].enabled0 |= irq_bit(irq);
        else
            vgic->vcpu[vcpu_id].enabled0 &= ~irq_bit(irq);
    } else if (enabled) {
        vgic->enabled[irq / 32] |= irq_bit(irq);
    } else {
        vgic->enabled[irq / 32] &= ~irq_bit(irq);
    }
}

uint32_t vmm_vgic3_enabled_word(const vgic3_t *vgic, uint32_t vcpu_id,
                                uint32_t word)
{
    if (!vgic || word >= VGIC3_MAX_WORDS)
        return 0;
    if (word == 0)
        return valid_vcpu(vgic, vcpu_id) ?
               (vgic->vcpu[vcpu_id].enabled0 | SGI_MASK) : SGI_MASK;
    return vgic->enabled[word];
}

uint32_t vmm_vgic3_pending_word(const vgic3_t *vgic, uint32_t vcpu_id,
                                uint32_t word)
{
    if (!vgic || word >= VGIC3_MAX_WORDS || !valid_vcpu(vgic, vcpu_id))
        return 0;
    if (word == 0)
        return vgic->vcpu[vcpu_id].pending0;
    return vgic->spi_pending[vcpu_id][word];
}

uint32_t vmm_vgic3_active_word(const vgic3_t *vgic, uint32_t vcpu_id,
                               uint32_t word)
{
    if (!vgic || word >= VGIC3_MAX_WORDS || !valid_vcpu(vgic, vcpu_id))
        return 0;
    if (word == 0)
        return vgic->vcpu[vcpu_id].active0;
    return vgic->spi_active[vcpu_id][word];
}

void vmm_vgic3_clear_pending_word(vgic3_t *vgic, uint32_t vcpu_id,
                                  uint32_t word, uint32_t bits)
{
    if (!vgic || word >= VGIC3_MAX_WORDS || !valid_vcpu(vgic, vcpu_id))
        return;
    if (word == 0)
        vgic->vcpu[vcpu_id].pending0 &= ~bits;
    else
        vgic->spi_pending[vcpu_id][word] &= ~bits;
}

void vmm_vgic3_clear_active_word(vgic3_t *vgic, uint32_t vcpu_id,
                                 uint32_t word, uint32_t bits)
{
    if (!vgic || word >= VGIC3_MAX_WORDS || !valid_vcpu(vgic, vcpu_id))
        return;
    if (word == 0)
        vgic->vcpu[vcpu_id].active0 &= ~bits;
    else
        vgic->spi_active[vcpu_id][word] &= ~bits;
}

/* ── 软件 active / pending 置位（内部）──────────────────────── */

static void set_active(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq)
{
    if (irq < 32)
        vgic->vcpu[vcpu_id].active0 |= irq_bit(irq);
    else
        vgic->spi_active[vcpu_id][irq / 32] |= irq_bit(irq);
}

static void clear_active(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq)
{
    if (irq < 32)
        vgic->vcpu[vcpu_id].active0 &= ~irq_bit(irq);
    else
        vgic->spi_active[vcpu_id][irq / 32] &= ~irq_bit(irq);
}

static void clear_pending(vgic3_t *vgic, uint32_t vcpu_id, uint32_t irq)
{
    if (irq < 16) {
        vgic->vcpu[vcpu_id].pending0 &= ~irq_bit(irq);
        vgic->vcpu[vcpu_id].sgi_sources[irq] = 0;
    } else if (irq < 32) {
        vgic->vcpu[vcpu_id].pending0 &= ~irq_bit(irq);
    } else {
        vgic->spi_pending[vcpu_id][irq / 32] &= ~irq_bit(irq);
    }
}

/* ── 进入 guest 前：把可投递中断写进空 LR ───────────────────── */

void vmm_vgic3_sync_entry(vgic3_t *vgic, uint32_t vcpu_id)
{
    if (!valid_vcpu(vgic, vcpu_id))
        return;

    vgic3_vcpu_t *vcpu = &vgic->vcpu[vcpu_id];

    /*
     * 1) 先按 ICH_ELRSR_EL2 回收槽位。
     *    ELRSR[i]=1 表示硬件认为 LR i 已空（guest 已 EOI，或从未被占用），
     *    此时清掉镜像，后续才可能把新的中断排进去。
     */
    uint64_t elrsr = gicv3_read_elrsr();
    uint32_t n = _gicv3.nr_lrs > VGIC3_MAX_LRS ? VGIC3_MAX_LRS : _gicv3.nr_lrs;

    for (uint32_t i = 0; i < n; i++)
        if ((elrsr >> i) & 1)
            vcpu->lr[i] = 0;

    /*
     * 2) 扫全部 INTID，把 pending & enabled & !active 且尚未排入 LR 的
     *    写进空槽位。
     */
    for (uint32_t word = 0; word < VGIC3_MAX_WORDS; word++) {
        uint32_t ready = vmm_vgic3_pending_word(vgic, vcpu_id, word) &
                         vmm_vgic3_enabled_word(vgic, vcpu_id, word) &
                         ~vmm_vgic3_active_word(vgic, vcpu_id, word);

        while (ready) {
            uint32_t bit  = ready & (~ready + 1u);
            uint32_t off  = 0;
            uint32_t tmp  = bit;

            while (tmp > 1u) { tmp >>= 1; off++; }
            ready &= ~bit;

            uint32_t irq = word * 32u + off;
            if (irq >= VGIC3_MAX_IRQS)
                break;
            if (lr_has_irq(vgic, vcpu_id, irq))
                continue;

            int slot = lr_empty_slot(vgic, vcpu_id);
            if (slot < 0)
                return;   /* LR 用满 */

            uint64_t value = vgic3_lr_value(vgic, vcpu_id, irq);
            vcpu->lr[slot] = value;
            gicv3_write_lr(slot, value);
        }
    }
}

/* ── 退出 guest 后：从硬件回推软件状态 ─────────────────────── */

/*
 * LR.State 由硬件维护：
 *   Pending          —— 已排入、guest 还没应答
 *   Active&Pending   —— guest 已读 IAR（ack），还没写 EOIR
 *   (槽位消失)        —— guest 已写 EOIR（硬件把 LR 置 Invalid，
 *                       ELRSR 对应位置 1）
 *
 * 所以：
 *   槽位仍在 && State 含 Active  → 这次 guest 应答了 → pending 转 active
 *   槽位消失 && 之前有镜像        → guest 完成了 EOI    → 清 active，
 *                                   等设备侧下次置 pending 再投
 */
void vmm_vgic3_sync_exit(vgic3_t *vgic, uint32_t vcpu_id)
{
    if (!valid_vcpu(vgic, vcpu_id))
        return;

    vgic3_vcpu_t *vcpu = &vgic->vcpu[vcpu_id];
    uint64_t elrsr = gicv3_read_elrsr();
    uint32_t n = _gicv3.nr_lrs > VGIC3_MAX_LRS ? VGIC3_MAX_LRS : _gicv3.nr_lrs;

    for (uint32_t i = 0; i < n; i++) {
        uint64_t prev = vcpu->lr[i];

        if (!prev)
            continue;

        if ((elrsr >> i) & 1) {
            /* 槽位已空：guest 走完了 ack+EOI（或硬件丢弃）*/
            uint32_t irq = lr_vintid(prev);
            clear_active(vgic, vcpu_id, irq);
            vcpu->lr[i] = 0;
            continue;
        }

        /* 槽位仍被占用：回读硬件状态 */
        uint64_t hw = gicv3_read_lr(i);
        uint32_t irq = lr_vintid(prev);

        if (lr_state(hw) & ICH_LR_ST_ACTIVE) {
            /* guest 已应答：挂起位交给 active 记帐，避免重复投递 */
            clear_pending(vgic, vcpu_id, irq);
            set_active(vgic, vcpu_id, irq);
        }
        vcpu->lr[i] = hw;
    }
}

/* ── 定时器注入（宿主侧设备 -> guest）──────────────────────── */

void vmm_vgic3_inject_timer(vgic3_t *vgic, uint32_t vcpu_id)
{
    vmm_vgic3_set_pending(vgic, vcpu_id, VGIC3_VTIMER_IRQ);
}
