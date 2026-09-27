/*
 * kernel/vmm/vdev/vgicv3/vgicr3.c — 虚拟 GICv3 Redistributor（GICR）模拟
 *
 * GICv3 里每个核有一个 128KB 的 Redistributor，分成两个 64KB 帧：
 *   RD_base  (0x00000)：GICR_CTLR / TYPER / WAKER / PROPBASER …
 *   SGI_base (0x10000)：这个核的 SGI 和 PPI（INTID 0..31）
 *
 * GICv2 把这些放在 GICD 的 banked 寄存器里，所以这是 GICv3 新增的一块。
 * 关键点：
 *   - GICR_WAKER.ChildrenAsleep 必须读回 0，否则 guest 的
 *     gic_enable_redist() 会一直等下去
 *   - GICR_TYPER.PLPIS/VLPIS 必须为 0（我们不实现 LPI），
 *     Last 置位表示这是最后一个 redistributor
 *   - guest 使能 PPI 27（虚拟定时器）时要把宿主侧的定时器注入路由打开
 */

#include "vmm/vmm_vgicv3.h"
#include "vmm/vmm_irq_route.h"
#include "irq/gicv3.h"      /* GICR_WAKER_* 等 GICv3 寄存器位定义 */
#include "klog.h"
#include "string.h"

/*
 * 下面这些是**本核 redistributor 窗口内的偏移**，不是宿主 GIC 的 MMIO 地址，
 * 所以统一加 VGIC3R_ 前缀 —— driver/irq/gicv3.h 里的 GICR_* 是宿主地址宏，
 * 同名会互相覆盖（编译会报 "redefined"）。
 *
 * SGI 帧的寄存器在下面 switch 里直接用 0x100xx 字面量，语义一目了然。
 */
#define VGIC3R_RD_CTLR       0x00000
#define VGIC3R_RD_IIDR       0x00004
#define VGIC3R_RD_TYPER      0x00008
#define VGIC3R_RD_STATUSR    0x00010
#define VGIC3R_RD_WAKER      0x00014
#define VGIC3R_RD_PROPBASER  0x00070
#define VGIC3R_RD_PENDBASER  0x00078
#define VGIC3R_RD_PIDR2      0x0ffe8
#define VGIC3R_SGI_PIDR2     0x1ffe8

#define GICR_CTLR_RWP   (1u << 31)

/* 取出本次访问落在哪个 vCPU 的 redistributor 上 */
static int redist_cpu(uint64_t off, const vgic3_t *vgic, uint32_t vcpu_id)
{
    uint32_t cpu = (uint32_t)(off / VGIC3R_STRIDE);

    if (cpu < vgic->nr_vcpus)
        return (int)cpu;
    if (vcpu_id < vgic->nr_vcpus)
        return (int)vcpu_id;   /* 偏移越界时退回访问者所属 vCPU */
    return 0;
}

static uint64_t vgic3r_read(mmio_device_t *dev, uint64_t off, uint8_t size,
                            uint32_t vcpu_id)
{
    vgic3_t *vgic = (vgic3_t *)dev->priv;
    /* 本核窗口内的偏移（0x00000..0x1ffff），不能只取低 16 位 ——
     * 那会把 SGI 帧的 0x10000 选择位一起掩掉，所有 SGI/PPI 访问都会
     * 落进 RD 帧分支被静默丢弃。*/
    uint32_t word = (uint32_t)(off & (VGIC3R_STRIDE - 1u));

    (void)size;

    if (!vgic)
        return 0;

    int cpu = redist_cpu(off, vgic, vcpu_id);
    vgic3_vcpu_t *vcpu = &vgic->vcpu[cpu];

    if (word >= VGIC3R_SGI_OFF) {
        /* ── SGI 帧 ── */
        switch (word - VGIC3R_SGI_OFF) {
        case 0x080:  /* GICR_IGROUPR0 */
            return vcpu->sgi_igroupr0;
        case 0x100: case 0x180:   /* ISENABLER0 / ICENABLER0 */
            return vmm_vgic3_enabled_word(vgic, (uint32_t)cpu, 0);
        case 0x200: case 0x280:   /* ISPENDR0 / ICPENDR0 */
            return vmm_vgic3_pending_word(vgic, (uint32_t)cpu, 0);
        case 0x300: case 0x380:   /* ISACTIVER0 / ICACTIVER0 */
            return vmm_vgic3_active_word(vgic, (uint32_t)cpu, 0);
        case 0x400: case 0x404: case 0x408: case 0x40c:
        case 0x410: case 0x414: case 0x418: case 0x41c: {
            uint32_t n = (word - 0x400u) / 4u;
            uint64_t v = 0;
            for (uint32_t i = 0; i < 4; i++)
                v |= (uint64_t)vcpu->prio0[n * 4u + i] << (8 * i);
            return v;
        }
        case 0xc00: return vcpu->sgi_icfgr[0];
        case 0xc04: return vcpu->sgi_icfgr[1];
        case 0xd00: return vcpu->sgi_igrpmodr0;
        default:
            if (word == (VGIC3R_SGI_PIDR2 - VGIC3R_SGI_OFF))
                return 0x30u;
            return 0;
        }
    }

    /* ── RD_base 帧 ── */
    switch (word) {
    case VGIC3R_RD_CTLR:
        /* EnableLPIs(bit0) 强制读回 0 —— 本模型不支持 LPI（GICR_TYPER.PLPIS=0、
         * GICD_TYPER.LPIS=0），按规范这一位就是 RAZ/WI。
         * RWP(bit31) 恒 0：所有写立即生效。 */
        return vcpu->rd_ctlr & ~(uint64_t)(GICR_CTLR_RWP | 1u);

    case VGIC3R_RD_IIDR:
        return 0x0202043Bu;

    case VGIC3R_RD_TYPER:
        /* [4]=Last（每个核一个 redistributor，最后一个是自己）；
         * PLPIS/VLPIS=0；ProcessorNumber/位图放在 [31:8]/[63:32]。*/
        return ((uint64_t)cpu << 8) |
               (((uint64_t)cpu) << 32) |
               (((uint32_t)cpu == vgic->nr_vcpus - 1u) ? (1ULL << 4) : 0ULL);

    case VGIC3R_RD_WAKER:
        /* ChildrenAsleep(bit2) 恒 0：唤醒立即完成（背后没有真实硬件）。
         * ProcessorSleep 按 guest 写进去的值回读，语义与硬件一致。*/
        return vcpu->rd_waker & GICR_WAKER_PROCESSOR_SLEEP;

    case VGIC3R_RD_PROPBASER:
        return vcpu->rd_propbaser;

    case VGIC3R_RD_PENDBASER:
        return vcpu->rd_pendbaser;

    case VGIC3R_RD_STATUSR:
        return 0;

    default:
        if (word == VGIC3R_RD_PIDR2)
            return 0x30u;
        return 0;
    }
}

static void vgic3r_write(mmio_device_t *dev, uint64_t off, uint8_t size,
                         uint64_t value, uint32_t vcpu_id)
{
    vgic3_t *vgic = (vgic3_t *)dev->priv;
    uint32_t word = (uint32_t)(off & (VGIC3R_STRIDE - 1u));
    uint32_t value32 = (uint32_t)value;

    (void)size;

    if (!vgic)
        return;

    int cpu = redist_cpu(off, vgic, vcpu_id);
    vgic3_vcpu_t *vcpu = &vgic->vcpu[cpu];

    if (word >= VGIC3R_SGI_OFF) {
        switch (word - VGIC3R_SGI_OFF) {
        case 0x080:
            vcpu->sgi_igroupr0 = value32;
            return;

        case 0x100:   /* GICR_ISENABLER0 */
            for (uint32_t bit = 0; bit < 32; bit++)
                if (value32 & (1u << bit))
                    vmm_vgic3_set_enabled(vgic, (uint32_t)cpu, bit, 1);
            if (value32 & (1u << VIRQ_VTIMER))
                vmm_irq_route_set_vtimer_enabled(1);
            KLOG_DEBUG("[vgic3r] cpu%u ISENABLER0=0x%x\n", cpu, value32);
            return;

        case 0x180:   /* GICR_ICENABLER0 */
            for (uint32_t bit = 0; bit < 32; bit++)
                if (value32 & (1u << bit))
                    vmm_vgic3_set_enabled(vgic, (uint32_t)cpu, bit, 0);
            if (value32 & (1u << VIRQ_VTIMER))
                vmm_irq_route_set_vtimer_enabled(0);
            return;

        case 0x200:   /* GICR_ISPENDR0 */
            for (uint32_t bit = 0; bit < 32; bit++)
                if (value32 & (1u << bit))
                    vmm_vgic3_set_pending(vgic, (uint32_t)cpu, bit);
            return;

        case 0x280:   /* GICR_ICPENDR0 */
            vmm_vgic3_clear_pending_word(vgic, (uint32_t)cpu, 0, value32);
            return;

        case 0x300:   /* GICR_ISACTIVER0 */
            for (uint32_t bit = 0; bit < 32; bit++)
                if (value32 & (1u << bit))
                    vgic->vcpu[cpu].active0 |= (1u << bit);
            return;

        case 0x380:   /* GICR_ICACTIVER0 */
            vmm_vgic3_clear_active_word(vgic, (uint32_t)cpu, 0, value32);
            return;

        case 0x400: case 0x404: case 0x408: case 0x40c:
        case 0x410: case 0x414: case 0x418: case 0x41c: {
            uint32_t n = (word - 0x400u) / 4u;
            for (uint32_t i = 0; i < 4; i++)
                vcpu->prio0[n * 4u + i] = (uint8_t)(value >> (8 * i));
            return;
        }

        case 0xc00: vcpu->sgi_icfgr[0] = value32; return;
        case 0xc04: vcpu->sgi_icfgr[1] = value32; return;
        case 0xd00: vcpu->sgi_igrpmodr0 = value32; return;
        default: return;
        }
    }

    switch (word) {
    case VGIC3R_RD_CTLR:
        vcpu->rd_ctlr = value32 & ~GICR_CTLR_RWP;
        return;

    case VGIC3R_RD_WAKER:
        /* 只跟踪 ProcessorSleep；写 0 表示唤醒，之后 ChildrenAsleep 读到 0 */
        vcpu->rd_waker = value32 & GICR_WAKER_PROCESSOR_SLEEP;
        return;

    case VGIC3R_RD_PROPBASER:
        vcpu->rd_propbaser = value;
        return;

    case VGIC3R_RD_PENDBASER:
        vcpu->rd_pendbaser = value;
        return;

    default:
        return;
    }
}

static const mmio_dev_ops_t g_vgic3r_ops = {
    .name           = "vgic3r",
    .base           = VGIC3R_BASE,
    .size           = VGIC3R_STRIDE * VGIC3_MAX_VCPUS,
    .read           = NULL,
    .write          = NULL,
    .read_for_vcpu  = vgic3r_read,
    .write_for_vcpu = vgic3r_write,
};

int vgic3r_init(mmio_device_t *dev, mmio_bus_t *bus, vgic3_t *vgic)
{
    if (!dev || !bus || !vgic)
        return -1;

    dev->ops  = &g_vgic3r_ops;
    dev->priv = vgic;

    if (mmio_bus_register(bus, dev) != 0)
        return -1;

    KLOG_INFO("[vgic3r] VM redistributors ready (%u vCPU, IPA 0x%llx)\n",
              vgic->nr_vcpus, (unsigned long long)VGIC3R_BASE);
    return 0;
}
