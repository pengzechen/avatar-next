/*
 * kernel/vmm/vdev/vgicd.c — 虚拟 GICv2 分发器（GICD）实现
 *
 * 移植自 x-kernel: virt/kvmm/src/vdev/aarch64/vgicd.rs，适配 Avatar OS：
 *   - Rust Box<[u8; N]> → 静态 BSS 数组
 *   - log::info → KLOG_INFO
 *   - 中断挂起/使能状态用位图维护；投递到 guest 需 GICH/GICV（见头文件注）
 */

#include "vmm_vgicd.h"
#include "vmm_vgic.h"
#include "vmm_irq_route.h"   /* 宿主 vtimer IRQ 路由注册 */
#include "klog.h"
#include "string.h"

/* ── GICD 寄存器偏移 ─────────────────────────────────────── */
#define GICD_CTLR        0x000
#define GICD_TYPER       0x004
#define GICD_IIDR        0x008
#define GICD_IGROUPR     0x080
#define GICD_ISENABLER   0x100    /* .. 0x17c */
#define GICD_ICENABLER   0x180    /* .. 0x1fc */
#define GICD_ISPENDR     0x200    /* .. 0x27c */
#define GICD_ICPENDR     0x280    /* .. 0x2fc */
#define GICD_IPRIORITYR  0x400
#define GICD_ITARGETSR   0x800    /* .. 0xbfc（0x800..0x820 banked 只读）*/
#define GICD_ICFGR       0xc00
#define GICD_SGIR        0xf00

/* 备份存储大小（覆盖 CTLR..ICFGR；guest 不会探测更高地址）*/
#define REG_SIZE     0x1000
#define ENABLE_WORDS 32       /* 32 * 32 = 1024 个中断 */

/* ── 设备私有状态 ─────────────────────────────────────────── */
typedef struct {
    uint8_t  regs[REG_SIZE];                       /* 字节可寻址后备存储 */
    uint64_t enabled[VGICD_MAX_VCPUS];             /* 每 vCPU 使能位图     */
    uint64_t pending[VGICD_MAX_VCPUS];             /* 每 vCPU 挂起位图     */
    uint8_t  targets[VGICD_MAX_IRQS];              /* SPI 目标 CPU 掩码    */
    uint32_t nr_vcpus;
} vgicd_state_t;

static vgicd_state_t g_vgicd;

/* ── 后备存储读写 ─────────────────────────────────────────── */
static uint64_t vgicd_rd(uint32_t off, uint8_t size)
{
    uint64_t v = 0;
    for (uint32_t i = 0; i < size; i++) {
        if (off + i < REG_SIZE)
            v |= (uint64_t)g_vgicd.regs[off + i] << (8 * i);
    }
    return v;
}

static void vgicd_wr(uint32_t off, uint8_t size, uint64_t val)
{
    for (uint32_t i = 0; i < size; i++) {
        if (off + i < REG_SIZE)
            g_vgicd.regs[off + i] = (uint8_t)(val >> (8 * i));
    }
}

/* 判断 off 是否落在 [base, base + 32*4) 的使能/挂起字窗口内 */
static int word_index(uint64_t off, uint64_t base, uint32_t *idx_out)
{
    if (off >= base && off < base + (uint64_t)ENABLE_WORDS * 4) {
        *idx_out = (uint32_t)((off - base) / 4);
        return 1;
    }
    return 0;
}

/* ── 公开注入接口 ─────────────────────────────────────────── */
void vgicd_set_pending(uint32_t vcpu_id, uint32_t irq)
{
    if (vcpu_id >= VGICD_MAX_VCPUS || irq >= VGICD_MAX_IRQS)
        return;
    g_vgicd.pending[vcpu_id] |= (1ULL << (irq & 63));
}

int vgicd_is_enabled(uint32_t vcpu_id, uint32_t irq)
{
    if (vcpu_id >= VGICD_MAX_VCPUS || irq >= VGICD_MAX_IRQS)
        return 0;
    if (irq < 16)
        return 1;                          /* SGI 恒使能 */
    return (g_vgicd.enabled[vcpu_id] & (1ULL << (irq & 63))) != 0;
}

/* ── SGI（软件生成中断）分发 ──────────────────────────────── */
static void handle_sgir(uint32_t value, uint32_t source_vcpu)
{
    uint32_t irq    = value & 0xf;
    uint32_t filter = (value >> 24) & 0x3;
    uint32_t mask;

    switch (filter) {
    case 0:  mask = (value >> 16) & 0xff; break;
    case 1:  mask = ((1u << g_vgicd.nr_vcpus) - 1) & ~(1u << source_vcpu); break;
    case 2:  mask = 1u << source_vcpu; break;
    default: mask = 0; break;              /* 3 = 本 CPU */
    }

    KLOG_INFO("[vgicd] SGIR: src=%u irq=%u filter=%u mask=0x%x\n",
              source_vcpu, irq, filter, mask);

    for (uint32_t cpu = 0; cpu < g_vgicd.nr_vcpus && cpu < 32; cpu++) {
        if (mask & (1u << cpu))
            vgicd_set_pending(cpu, irq);
    }
}

static void vgicd_note_unhandled(uint64_t off, int is_write);

/* ── MMIO 读写回调 ────────────────────────────────────────── */
static uint64_t vgicd_read(mmio_device_t *dev, uint64_t off, uint8_t size)
{
    (void)dev;
    uint32_t w;

    vgicd_note_unhandled(off, 0);

    switch (off) {
    case GICD_TYPER:
        /* ITLinesNumber[4:0] | CPU 数 [7:5]；0x3 → 支持 SGI/PPI */
        return (((uint64_t)(g_vgicd.nr_vcpus - 1) & 0x7) << 5) | 0x3;
    case GICD_IIDR:
        return 0;
    case GICD_ISENABLER:
    case GICD_ICENABLER:
        /* 返回当前使能字（仅返回 32 位窗口内的位）*/
        if (word_index(off, GICD_ISENABLER, &w) ||
            word_index(off, GICD_ICENABLER, &w)) {
            /* 简化：以 vCPU0 视图返回低 64 位 */
            return (uint64_t)(uint32_t)(g_vgicd.enabled[0] >> (w * 32));
        }
        return 0;
    default:
        break;
    }

    /* ITARGETSR 对 SGI/PPI（IRQ 0-31）为 banked 只读，guest 据此
     * 发现每 CPU 的目标掩码对 SGI/IPI 很重要 */
    if (off >= GICD_ITARGETSR && off < GICD_ITARGETSR + 0x20) {
        uint32_t mask = 1u;   /* 单 vCPU：CPU0 */
        return (uint64_t)(mask | (mask << 8) | (mask << 16) | (mask << 24));
    }

    vgicd_note_unhandled(off, 0);
    vgicd_note_unhandled(off, 0);
    return vgicd_rd((uint32_t)off, size);
}

/* 诊断：记录未被显式处理的 GICD 偏移（guest 卡死时定位用）*/
static void vgicd_note_unhandled(uint64_t off, int is_write)
{
    /* 诊断：打印前若干次 GICD 访问，用于确认 guest 是否在轮询 GICD、
     * 以及轮询哪个寄存器（guest 卡死定位用）。*/
    static uint32_t n_r, n_w;

    if (!is_write) {
        if (n_r++ < 24)
            KLOG_INFO("[vgicd] READ  off=0x%llx\n", (unsigned long long)off);
    } else {
        if (n_w++ < 24)
            KLOG_INFO("[vgicd] WRITE off=0x%llx\n", (unsigned long long)off);
    }
}

static void vgicd_write(mmio_device_t *dev, uint64_t off, uint8_t size,
                        uint64_t value)
{
    (void)dev;
    uint32_t v = (uint32_t)value;
    uint32_t w;

    vgicd_note_unhandled(off, 1);

    /* 使能/禁用 */
    if (word_index(off, GICD_ISENABLER, &w)) {
        g_vgicd.enabled[0] |= (uint64_t)v << (w * 32);
        for (uint32_t b = 0; b < 32; b++) {
            if (v & (1u << b))
                vmm_vgic_set_enabled(0, w * 32 + b, 1);
        }
        /* guest 使能了虚拟定时器 PPI → 注册宿主 IRQ 路由
         * （对标 vgicd.rs 的 set_host_vtimer_irq_enabled(true)）*/
        if (w == 0 && (v & (1u << VGICD_VTIMER_IRQ)))
            vmm_irq_route_set_vtimer_enabled(1);
        return;
    }
    if (word_index(off, GICD_ICENABLER, &w)) {
        g_vgicd.enabled[0] &= ~((uint64_t)v << (w * 32));
        for (uint32_t b = 0; b < 32; b++) {
            if (v & (1u << b))
                vmm_vgic_set_enabled(0, w * 32 + b, 0);
        }
        if (w == 0 && (v & (1u << VGICD_VTIMER_IRQ)))
            vmm_irq_route_set_vtimer_enabled(0);
        return;
    }

    /* 置挂起 */
    if (word_index(off, GICD_ISPENDR, &w)) {
        for (uint32_t b = 0; b < 32; b++) {
            if (v & (1u << b)) {
                uint32_t irq = w * 32 + b;
                uint32_t mask = (irq < 32) ? 1u : g_vgicd.targets[irq < VGICD_MAX_IRQS ? irq : 0];
                if (mask == 0)
                    mask = 1;
                for (uint32_t cpu = 0; cpu < g_vgicd.nr_vcpus && cpu < 32; cpu++) {
                    if (mask & (1u << cpu))
                        vgicd_set_pending(cpu, irq);
                }
            }
        }
        return;
    }

    /* 清挂起：尽力而为，本模型忽略 */
    if (word_index(off, GICD_ICPENDR, &w))
        return;

    if (off == GICD_SGIR) {
        handle_sgir(v, 0);
        return;
    }

    /* IPRIORITYR / ITARGETSR(SPI) / ICFGR / CTLR 等：存后备供读回 */
    if (off >= GICD_ITARGETSR && off < GICD_ITARGETSR + 0x20)
        return;   /* banked 只读，忽略写 */

    if (off >= GICD_ITARGETSR && off < GICD_ITARGETSR + VGICD_MAX_IRQS) {
        uint32_t idx = (uint32_t)(off - GICD_ITARGETSR);
        if (idx < VGICD_MAX_IRQS)
            g_vgicd.targets[idx] = (uint8_t)v;
    }

    vgicd_note_unhandled(off, 1);
    vgicd_wr((uint32_t)off, size, value);
}

static const mmio_dev_ops_t g_vgicd_ops = {
    .name  = "vgicd",
    .base  = VGICD_BASE,
    .size  = VGICD_SIZE,
    .read  = vgicd_read,
    .write = vgicd_write,
};

int vgicd_init(mmio_device_t *dev, mmio_bus_t *bus, uint32_t nr_vcpus)
{
    if (!dev || !bus)
        return -1;
    if (nr_vcpus < 1)
        nr_vcpus = 1;
    if (nr_vcpus > VGICD_MAX_VCPUS)
        nr_vcpus = VGICD_MAX_VCPUS;

    memset(&g_vgicd, 0, sizeof(g_vgicd));
    g_vgicd.nr_vcpus = nr_vcpus;

    dev->ops  = &g_vgicd_ops;
    dev->priv = &g_vgicd;

    KLOG_INFO("[vgicd] virtual GICv2 distributor ready (%u vCPU)\n", nr_vcpus);
    return mmio_bus_register(bus, dev);
}
