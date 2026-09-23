/*
 * kernel/vmm/vdev/vgicv3/vgicd3.c — 虚拟 GICv3 分发器（GICD）模拟
 *
 * guest 通过 MMIO 配置 GICD。GICv3 打开 ARE 之后 GICD **只管 SPI**
 * （INTID >= 32），SGI/PPI 全在 GICR（见 vgicr3.c），ITARGETSR 变成 RES0，
 * SPI 目标改用 IROUTER。
 *
 * 三个必须读回正确值的地方（否则 guest 的探测会卡住）：
 *   - GICD_CTLR.RWP(bit31)：ARE 生效的握手位，必须读回 0
 *   - GICD_TYPER：ITLinesNumber / CPUNumber / LPIS=0 决定 guest 分配多少 IRQ
 *   - IGROUPR / IPRIORITYR / ICFGR：字节可寻址后备存储，读回写进去的值
 *
 * 真实的宿主 GIC 分发器永不触碰。
 */

#include "vmm/vmm_vgicv3.h"
#include "klog.h"
#include "string.h"

#define GICD_CTLR        0x0000
#define GICD_TYPER       0x0004
#define GICD_IIDR        0x0008
#define GICD_IGROUPR     0x0080
#define GICD_ISENABLER   0x0100
#define GICD_ICENABLER   0x0180
#define GICD_ISPENDR     0x0200
#define GICD_ICPENDR     0x0280
#define GICD_ISACTIVER   0x0300
#define GICD_ICACTIVER   0x0380
#define GICD_IPRIORITYR  0x0400
#define GICD_ITARGETSR   0x0800
#define GICD_ICFGR       0x0c00
#define GICD_IGRPMODR    0x0d00
#define GICD_IROUTER     0x6000
#define GICD_PIDR2       0xffe8

#define GICD_CTLR_RWP    (1u << 31)
#define GICD_REG_WORDS   32          /* 每个 word-indexed 寄存器块 32 个字 */

/* 一个 word-indexed 寄存器块覆盖的字节数 */
#define GICD_BLOCK_SIZE  (GICD_REG_WORDS * 4u)

static int word_index(uint64_t off, uint32_t base, uint32_t *index)
{
    if (off < base || off >= base + GICD_BLOCK_SIZE)
        return 0;
    *index = (uint32_t)((off - base) / 4);
    return 1;
}

static uint64_t reg_read(const vgic3_t *vgic, uint32_t off, uint8_t size)
{
    uint64_t value = 0;

    for (uint32_t i = 0; i < size; i++)
        if (off + i < VGIC3D_REG_SIZE)
            value |= (uint64_t)vgic->dist_regs[off + i] << (8 * i);
    return value;
}

static void reg_write(vgic3_t *vgic, uint32_t off, uint8_t size, uint64_t value)
{
    for (uint32_t i = 0; i < size; i++)
        if (off + i < VGIC3D_REG_SIZE)
            vgic->dist_regs[off + i] = (uint8_t)(value >> (8 * i));
}

static uint64_t vgic3d_read(mmio_device_t *dev, uint64_t off, uint8_t size,
                            uint32_t vcpu_id)
{
    vgic3_t *vgic = (vgic3_t *)dev->priv;
    uint32_t word;

    if (!vgic)
        return 0;

    /* RWP 恒 0：所有写操作立即生效 */
    if (off == GICD_CTLR)
        return reg_read(vgic, GICD_CTLR, size) & ~(uint64_t)GICD_CTLR_RWP;

    if (off == GICD_TYPER) {
        /* ITLinesNumber=31 → 1024 个 INTID；CPUNumber=nr_vcpus-1；
         * LPIS/ESPI/安全扩展全 0（没有 LPI，也没有 SPI 之外的扩展）；
         * IDbits=15 → 16 位 INTID，与 QEMU virt 的 GICv3 一致。*/
        return ((uint64_t)(VGIC3_MAX_IRQS / 32 - 1) & 0x1fu) |
               (((uint64_t)(vgic->nr_vcpus - 1) & 0x7u) << 5) |
               (0xFu << 19);
    }

    if (off == GICD_IIDR)
        return 0x0202043Bu;   /* ARM, GICv3 架构修订 3 */

    if (off == GICD_PIDR2)
        return 0x30u;   /* 架构修订 = GICv3 */

    if (word_index(off, GICD_ISENABLER, &word) ||
        word_index(off, GICD_ICENABLER, &word))
        return vmm_vgic3_enabled_word(vgic, vcpu_id, word);

    if (word_index(off, GICD_ISPENDR, &word) ||
        word_index(off, GICD_ICPENDR, &word))
        return vmm_vgic3_pending_word(vgic, vcpu_id, word);

    if (word_index(off, GICD_ISACTIVER, &word) ||
        word_index(off, GICD_ICACTIVER, &word))
        return vmm_vgic3_active_word(vgic, vcpu_id, word);

    /* ARE 模式下 ITARGETSR 是 RES0 —— guest 读它只会得到 0，
     * CPU 目标改从 IROUTER 读。*/
    if (off >= GICD_ITARGETSR && off < GICD_ITARGETSR + 0x400u)
        return 0;

    return reg_read(vgic, (uint32_t)off, size);
}

static void vgic3d_write(mmio_device_t *dev, uint64_t off, uint8_t size,
                         uint64_t value, uint32_t vcpu_id)
{
    vgic3_t *vgic = (vgic3_t *)dev->priv;
    uint32_t word;
    uint32_t value32 = (uint32_t)value;

    if (!vgic)
        return;

    if (off == GICD_CTLR) {
        uint32_t ctlr = value32 & ~GICD_CTLR_RWP;

        vmm_vgic3_set_dist_enabled(vgic, (ctlr & 1u) != 0);
        reg_write(vgic, GICD_CTLR, size, ctlr);
        return;
    }

    if (word_index(off, GICD_ISENABLER, &word)) {
        for (uint32_t bit = 0; bit < 32; bit++)
            if (value32 & (1u << bit))
                vmm_vgic3_set_enabled(vgic, vcpu_id, word * 32u + bit, 1);
        return;
    }

    if (word_index(off, GICD_ICENABLER, &word)) {
        for (uint32_t bit = 0; bit < 32; bit++)
            if (value32 & (1u << bit))
                vmm_vgic3_set_enabled(vgic, vcpu_id, word * 32u + bit, 0);
        return;
    }

    if (word_index(off, GICD_ISPENDR, &word)) {
        for (uint32_t bit = 0; bit < 32; bit++)
            if (value32 & (1u << bit))
                vmm_vgic3_set_pending(vgic, vcpu_id, word * 32u + bit);
        return;
    }

    if (word_index(off, GICD_ICPENDR, &word)) {
        vmm_vgic3_clear_pending_word(vgic, vcpu_id, word, value32);
        return;
    }

    if (word_index(off, GICD_ICACTIVER, &word)) {
        vmm_vgic3_clear_active_word(vgic, vcpu_id, word, value32);
        return;
    }

    if (word_index(off, GICD_ISACTIVER, &word)) {
        /* guest 手工置 active：只做记录，不影响投递 */
        for (uint32_t bit = 0; bit < 32; bit++)
            if (value32 & (1u << bit))
                vgic->spi_active[vcpu_id][word] |= (1u << bit);
        return;
    }

    if (off >= GICD_ITARGETSR && off < GICD_ITARGETSR + 0x400u)
        return;   /* ARE: RES0 */

    /* IROUTER：8 字节/SPI，直接存进后备存储即可（本 VM 没有 SPI）*/
    reg_write(vgic, (uint32_t)off, size, value);
}

static const mmio_dev_ops_t g_vgic3d_ops = {
    .name           = "vgic3d",
    .base           = VGIC3D_BASE,
    .size           = VGIC3D_SIZE,
    .read           = NULL,
    .write          = NULL,
    .read_for_vcpu  = vgic3d_read,
    .write_for_vcpu = vgic3d_write,
};

int vgic3d_init(mmio_device_t *dev, mmio_bus_t *bus, vgic3_t *vgic)
{
    if (!dev || !bus || !vgic)
        return -1;

    dev->ops  = &g_vgic3d_ops;
    dev->priv = vgic;

    if (mmio_bus_register(bus, dev) != 0)
        return -1;

    KLOG_INFO("[vgic3d] VM distributor ready (%u vCPU)\n", vgic->nr_vcpus);
    return 0;
}
