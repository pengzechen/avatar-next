/*
 * driver/irq/gicv3.c — 宿主 GICv3 驱动（AArch64 @ EL2 / VHE）
 *
 * 与 gicv2.c 的分工差异：
 *   - GICv2：GICD 统管全部 INTID，GICC 是 MMIO 的 CPU interface，
 *            vGIC 走 GICH/GICV 的 MMIO。
 *   - GICv3：GICD 只管 SPI（ARE 模式），SGI/PPI 在 per-CPU 的 GICR 里，
 *            CPU interface 是 ICC_* 系统寄存器，vGIC 走 ICH_* 系统寄存器。
 *
 * VHE（E2H=1）说明：宿主跑在 EL2，但访问 `ICC_PMR_EL1` / `ICC_IAR1_EL1`
 * 这类 `S3_0_*` 编码时，E2H=1 会把它们重定向到 EL2 视角的物理 CPU
 * interface。因此这里的寄存器名和普通 EL1 内核一样，不需要写 `_EL2` 版。
 * ICC_SRE_EL2 没有 EL1 别名语义（它是 EL2 专有寄存器），用 op1=4 编码显式访问。
 */

#include "gicv3.h"
#include "mmio.h"
#include "barrier.h"
#include "klog.h"
#include "timer/timer.h"
#include "aarch64/sysreg.h"   /* SYSREG_READ / SYSREG_WRITE */

/* GICv3 模块内基地址 */
uintptr_t gicv3_gicd_base = 0;
uintptr_t gicv3_gicr_base = 0;

struct gicv3_t _gicv3;

/* 所有 SPI/SGI/PPI 的默认优先级：0xA0，低于 PMR（0xFF）故可投递 */
#define GICV3_DEFAULT_PRIORITY 0xA0u

/* ── 系统寄存器读写封装 ──────────────────────────────────────── */
#define ICC_READ(reg)     SYSREG_READ(reg)
#define ICC_WRITE(reg, v) SYSREG_WRITE(reg, (v))

/* ── Redistributor 定位 ──────────────────────────────────────── */

/*
 * gicv3_find_cpu_gicr - 按 MPIDR 亲和性在 GICR 链中找到当前核的 Redistributor
 *
 * GICR 是一串 128KB 对齐的块，块的 GICR_TYPER[63:32] 保存该核的亲和性，
 * [4]=Last 表示链尾。找不到（或遍历到 Last）时回退到 gicv3_gicr_base。
 */
static uintptr_t
gicv3_find_cpu_gicr(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));

    /* MPIDR Aff3:Aff2:Aff1:Aff0 → GICR_TYPER[63:32] 同名字段 */
    uint32_t mpidr_aff = (uint32_t)(((mpidr >> 32) & 0xFFu) << 24) |
                         (uint32_t)(((mpidr >> 16) & 0xFFu) << 16) |
                         (uint32_t)(((mpidr >> 8) & 0xFFu) << 8) |
                         (uint32_t)(mpidr & 0xFFu);
    uintptr_t gicr = gicv3_gicr_base;

    for (uint32_t step = 0; step < 16u; step++) {
        uint64_t typer = read64((const volatile void *)(gicr + 0x0008u));
        if ((uint32_t)(typer >> 32) == mpidr_aff)
            return gicr;
        if (typer & (1u << 4))   /* GICR_TYPER.Last */
            break;
        gicr += GICR_STRIDE;
    }
    return gicv3_gicr_base;
}

/* ── 唤醒本核 Redistributor ──────────────────────────────────── */
static void gicv3_wake_redistributor(uintptr_t gicr)
{
    uint32_t waker = read32((void *)(gicr + 0x0014)); /* GICR_WAKER */
    waker &= ~GICR_WAKER_PROCESSOR_SLEEP;
    write32(waker, (void *)(gicr + 0x0014));
    barrier_sync();   /* 必须确实到达 GIC 设备 */

    uint32_t timeout = 1000000u;
    while ((read32((void *)(gicr + 0x0014)) & GICR_WAKER_CHILDREN_ASLEEP) &&
           timeout-- > 0)
        timer_spin(1);

    if (read32((void *)(gicr + 0x0014)) & GICR_WAKER_CHILDREN_ASLEEP)
        KLOG_WARN("[gicv3] GICR_WAKER.ChildrenAsleep timeout (GICR=0x%lx)\n",
                  (unsigned long)gicr);
}

/*
 * 把本核 SGI/PPI 全部划到非安全 Group1 并设默认优先级。
 * PPI 默认属于 Group0；只有 ICC_IGRPEN1_EL1 使能时 Group0 的 PPI 会被
 * CPU interface 过滤掉，宿主就永远收不到 timer。GICv2 那边靠
 * gic_set_ipriority() 里的 bit7 顺手做了这件事，GICv3 的 group 是
 * GICR_IGROUPR0 独立寄存器，必须显式写。
 */
static void gicv3_config_sgi_frame(uintptr_t gicr)
{
    uintptr_t sgi = gicr + GICR_SGI_OFFSET;

    write32(0xFFFFFFFFu, (void *)(sgi + 0x0080u)); /* GICR_IGROUPR0 */
    /*
     * IGRPMODR0 必须一起写 1。
     *
     * 中断的组由两个位共同决定：IGROUPR=1 且 IGRPMODR=0 → **Group 1 Secure**，
     * 只有两个都为 1 才是 Guest 用的 Group 1 Non-secure。IGRPMODR 复位值是 0，
     * 只写 IGROUPR 会把 PPI 全划进 Secure 组：无 EL3 时这类中断根本不会以
     * IRQ 形式送到 CPU interface，表现为「GICR_ISENABLER0 已置位、PPI 已
     * pending、PMR/IGRPEN1 都对，但永远收不到中断」（实测 QEMU trace 里
     * gicv3_cpuif_set_irqs 始终是 IRQ 0）。
     */
    write32(0xFFFFFFFFu, (void *)(sgi + 0x0d00u)); /* GICR_IGRPMODR0 */

    for (uint32_t off = 0; off < 32u; off += 4u)
        write32(GICV3_DEFAULT_PRIORITY * 0x01010101u,
                (void *)(sgi + 0x0400u + off));    /* GICR_IPRIORITYRn */

    barrier_sync();
}

/*
 * 使能 CPU interface 的系统寄存器访问。
 *
 * VHE 下不能访问 ICC_SRE_EL2 的 EL2 编码（UNDEFINED），EL2 视角的
 * ICC_SRE_EL1 就是它 —— Enable(bit3) 是「允许 EL1 访问 ICC_SRE_EL1」的闸门，
 * SRE(bit0) 选择系统寄存器接口，DIB/DFB 关掉两个内存屏障暗示。
 * 没有 Enable，后续对 ICC_PMR_EL1 / ICC_IGRPEN1_EL1 的写入不会生效。
 */
static void gicv3_enable_cpuif_sysreg(void)
{
    uint64_t sre = ICC_READ(ICC_SRE_EL1);

    sre |= ICC_SRE_EL2_SRE | ICC_SRE_EL2_DIB | ICC_SRE_EL2_DFB |
           ICC_SRE_EL2_ENABLE;
    ICC_WRITE(ICC_SRE_EL1, sre);
    barrier_instr_full();   /* 写完 ICC_SRE 必须 isb 才对后续访问生效 */
}

/* ── 宿主 BSP 初始化 ─────────────────────────────────────────── */
void gicv3_init(void)
{
    gicv3_gicd_base = platform_get_mmio("irq", "gicd");
    gicv3_gicr_base = platform_get_mmio("irq", "gicr");

    KLOG_GIC("GICv3: Initializing (gicd=0x%llx gicr=0x%llx)\n",
             (unsigned long long)gicv3_gicd_base,
             (unsigned long long)gicv3_gicr_base);

    if (gicv3_gicd_base == 0 || gicv3_gicr_base == 0) {
        logger_error("GICv3: missing MMIO base (gicd/gicr not in platform.conf)\n");
        return;
    }

    _gicv3.irq_nr = GICD_TYPER_IRQS(read32((void *)GICD_TYPER));
    _gicv3.nr_lrs = gicv3_vtr_nr_lrs();
    logger_info("GICv3: %u IRQ lines, %u list registers\n",
                _gicv3.irq_nr, _gicv3.nr_lrs);

    /*
     * GICD_CTLR：ARE（亲和性路由，SPI 目标改用 IROUTER）+
     * EnableGrp1NS + **EnableGrp1A**。
     *
     * EnableGrp1A(bit1) 是 Group1 中断「从 distributor 转发到 CPU interface」
     * 的总闸门，只写 EnableGrp1NS 不够：实测（`make GIC=v3` + SM 自检）
     * 少了这一位时 PPI 26 在 GICR 里 pending/enabled 都正常、PMR/IGRPEN1
     * 也对，但 `ICC_IAR1_EL1` 永远返回 spurious(1023)，timer ISR 一次都进不来。
     * 逐位扫描 GICD_CTLR 确认：{0x31,0x11,0x41,0x51} 都收不到，
     * {0x33,0x37,0x13}（都含 bit1）立刻开始 tick。
     * Linux 的 gic_dist_init() 同样同时写 GICD_CTLR_ENABLE_G1 和 _G1A。
     */
    write32(GICD_CTLR_ENABLE_G1NS_BIT | GICD_CTLR_ENABLE_G1A_BIT |
            GICD_CTLR_ARE_S_BIT | GICD_CTLR_ARE_NS_BIT, (void *)GICD_CTLR);

    /* ARE 生效需要等 RWP 清零，否则后续 SPI 配置会被丢弃 */
    barrier_sync();
    for (int i = 0; i < 1000000; i++) {
        if (!(read32((void *)GICD_CTLR) & GICD_CTLR_RWP_BIT))
            break;
    }
    if (read32((void *)GICD_CTLR) & GICD_CTLR_RWP_BIT)
        logger_warn("GICv3: GICD_CTLR.RWP stuck\n");

    /* SPI：默认优先级 0xA0、Group1、全部禁止 */
    for (uint32_t n = 0; n < (32u + _gicv3.irq_nr) / 32u; n++) {
        write32(0xFFFFFFFFu, (void *)GICD_IGROUPR(n));
        write32(0xA0A0A0A0u, (void *)(GICD_IPRIORITYR + n * 4u));
    }
    barrier_sync();

    /* 本核 Redistributor：先设 SRE，再唤醒（顺序错会死锁，见函数注释）*/
    gicv3_enable_cpuif_sysreg();

    uintptr_t gicr = gicv3_find_cpu_gicr();
    gicv3_wake_redistributor(gicr);
    gicv3_config_sgi_frame(gicr);

    /* CPU interface：EOImode=0（EOI 一步完成 drop + deactivate）*/
    ICC_WRITE(ICC_CTLR_EL1, 0);
    ICC_WRITE(ICC_PMR_EL1, 0xFFu);
    ICC_WRITE(ICC_IGRPEN1_EL1, 1u);
    barrier_instr_full();

    KLOG_GIC("GICv3: Init done\n");
}

/*
 * gicv3_init_secondary - 次级核 per-CPU GICv3 初始化
 *
 * ARM GICv3 规范规定的唤醒顺序：
 *   1. 先使能 CPU interface（ICC_SRE），然后 ISB
 *   2. 再清 GICR_WAKER.ProcessorSleep，等 ChildrenAsleep=0
 *   3. 最后配置 ICC_CTLR / ICC_PMR / ICC_IGRPEN1
 * 若先等 ChildrenAsleep 而未设 ICC_SRE，CPU interface 未激活，
 * redistributor 的唤醒握手完不成 → 死锁。
 */
void gicv3_init_secondary(void)
{
    if (gicv3_gicr_base == 0)
        return;

    uintptr_t gicr = gicv3_find_cpu_gicr();

    gicv3_enable_cpuif_sysreg();
    gicv3_wake_redistributor(gicr);
    gicv3_config_sgi_frame(gicr);

    ICC_WRITE(ICC_CTLR_EL1, 0);
    ICC_WRITE(ICC_PMR_EL1, 0xFFu);
    ICC_WRITE(ICC_IGRPEN1_EL1, 1u);
    barrier_instr_full();

    KLOG_GIC("[gicv3] secondary init done (GICR=0x%lx)\n",
             (unsigned long)gicr);
}

/* ── 中断使能 / 查询 ─────────────────────────────────────────── */

void gicv3_enable_int(int int_id, bool enable)
{
    uint32_t mask = 1u << (int_id % 32);

    if (int_id < 32) {
        /* SGI/PPI：per-CPU banked，写本核 GICR SGI frame */
        uintptr_t sgi = gicv3_find_cpu_gicr() + GICR_SGI_OFFSET;
        write32(mask, (void *)(sgi + (enable ? 0x100u : 0x180u)));
    } else {
        uint32_t reg = (uint32_t)int_id / 32;
        write32(mask, (void *)(uint64_t)(enable ? GICD_ISENABLER(reg)
                                                : GICD_ICENABLER(reg)));
    }
    logger_gic_debug("GICv3: %s int %d\n", enable ? "enable" : "disable", int_id);
}

bool gicv3_is_int_enabled(int int_id)
{
    uint32_t mask = 1u << (int_id % 32);
    uint32_t val;

    if (int_id < 32) {
        uintptr_t sgi = gicv3_find_cpu_gicr() + GICR_SGI_OFFSET;
        val = read32((void *)(sgi + 0x100u));   /* GICR_ISENABLER0 */
    } else {
        val = read32((void *)(uint64_t)GICD_ISENABLER((uint32_t)int_id / 32));
    }
    return (val & mask) != 0;
}

void gicv3_set_int_trigger(uint32_t int_id, int edge)
{
    uint32_t reg   = int_id / 16;
    uint32_t shift = (int_id % 16) * 2;
    uint32_t val;

    if (int_id < 32) {
        /* SGI/PPI 的 ICFGR 在 GICR SGI frame（每核一份，共 2 个字）*/
        uintptr_t sgi = gicv3_find_cpu_gicr() + GICR_SGI_OFFSET;
        uintptr_t addr = sgi + 0x0c00u + (reg & 1u) * 4u;
        val = read32((void *)addr);
        if (edge) val |=  (1u << (shift + 1));
        else      val &= ~(1u << (shift + 1));
        write32(val, (void *)addr);
        return;
    }

    uintptr_t addr = GICD_ICFGR(reg);
    val = read32((void *)addr);
    if (edge) val |=  (1u << (shift + 1));
    else      val &= ~(1u << (shift + 1));
    write32(val, (void *)addr);
}

/*
 * ARE 模式下 SPI 的目标不再是 8 位掩码 + ITARGETSR，而是 GICD_IROUTER<n>
 * 里的完整亲和性（Aff3:Aff2:Aff1:Aff0）+ IRM。
 * 这里把 target_cpu_mask 的**最低置位**当作目标核编号，换算成亲和性；
 * 与 GICv2 语义（一位一个核）保持一致。
 */
void gicv3_set_int_target(uint32_t int_id, uint8_t target_cpu_mask)
{
    if (int_id < 32)
        return;   /* SGI/PPI 是 per-CPU，没有目标配置 */

    uint32_t cpu = 0;
    while (cpu < 8 && !(target_cpu_mask & (1u << cpu)))
        cpu++;
    if (cpu >= 8)
        cpu = 0;

    write64((uint64_t)cpu, (void *)(uint64_t)GICD_IROUTER(int_id));
}

/* ── CPU interface 应答 / 结束 ───────────────────────────────── */

uint32_t
gicv3_read_iar(void)
{
    return (uint32_t)ICC_READ(ICC_IAR1_EL1);
}

uint32_t
gicv3_iar_irqnr(uint32_t iar)
{
    return iar & ICC_IAR_INTID_MASK;
}

void gicv3_write_eoir(uint32_t irqstat)
{
    ICC_WRITE(ICC_EOIR1_EL1, irqstat);
}

/*
 * GICv2 兼容接口：timer_aarch64_impl.h / exception.c 直接 extern 调用。
 *
 * gic_set_ipriority：GICv3 下 INTID < 32 的优先级在 **GICR** 而不是 GICD。
 * 之前这里无条件写 GICD_IPRIORITYR，导致宿主 PPI 26（CNTHP）的优先级
 * 根本没写进去。
 */
void gic_set_ipriority(uint32_t int_id, uint32_t priority)
{
    uint32_t reg   = int_id / 4;
    uint32_t shift = (int_id % 4) * 8;
    uint8_t  pri   = (uint8_t)((priority << 3) & 0xF8u);
    uint32_t val;

    if (int_id < 32) {
        uintptr_t sgi = gicv3_find_cpu_gicr() + GICR_SGI_OFFSET;
        uintptr_t addr = sgi + 0x400u + reg * 4u;   /* GICR_IPRIORITYRn */
        val = read32((void *)addr);
        val &= ~(0xFFu << shift);
        val |= (uint32_t)pri << shift;
        write32(val, (void *)addr);
        return;
    }

    uintptr_t addr = GICD_IPRIORITYR + reg * 4u;
    val = read32((void *)addr);
    val &= ~(0xFFu << shift);
    val |= (uint32_t)pri << shift;
    write32(val, (void *)addr);
}

/*
 * gic_write_dir：GICv2 的「撤销激活」。
 * GICv3 宿主用 EOImode=0，ICC_EOIR1_EL1 一步完成优先级下降 + deactivate，
 * 没有 GICC_DIR 这个 MMIO 寄存器，因此这里是空操作。
 */
void gic_write_dir(uint32_t irqstat)
{
    (void)irqstat;
}

/* ── vGIC：ICH_LR<n>_EL2 访问 ───────────────────────────────── */

uint32_t
gicv3_vtr_nr_lrs(void)
{
    uint64_t vtr = ICC_READ(ICH_VTR_EL2);
    uint32_t n = (uint32_t)(vtr & 0xFu) + 1u;   /* ListRegs[3:0] */
    return n > GICV3_MAX_LRS ? GICV3_MAX_LRS : n;
}

/*
 * ICH_LR<n>_EL2 的寄存器编号不连续：n=0..7 在 CRm=12，n=8..15 在 CRm=13。
 */
void gicv3_write_lr(int32_t n, uint64_t value)
{
    if (n < 0 || n >= GICV3_MAX_LRS)
        return;

    switch (n) {
    case 0:  ICC_WRITE("S3_4_C12_C12_0", value); break;
    case 1:  ICC_WRITE("S3_4_C12_C12_1", value); break;
    case 2:  ICC_WRITE("S3_4_C12_C12_2", value); break;
    case 3:  ICC_WRITE("S3_4_C12_C12_3", value); break;
    case 4:  ICC_WRITE("S3_4_C12_C12_4", value); break;
    case 5:  ICC_WRITE("S3_4_C12_C12_5", value); break;
    case 6:  ICC_WRITE("S3_4_C12_C12_6", value); break;
    case 7:  ICC_WRITE("S3_4_C12_C12_7", value); break;
    case 8:  ICC_WRITE("S3_4_C12_C13_0", value); break;
    case 9:  ICC_WRITE("S3_4_C12_C13_1", value); break;
    case 10: ICC_WRITE("S3_4_C12_C13_2", value); break;
    case 11: ICC_WRITE("S3_4_C12_C13_3", value); break;
    case 12: ICC_WRITE("S3_4_C12_C13_4", value); break;
    case 13: ICC_WRITE("S3_4_C12_C13_5", value); break;
    case 14: ICC_WRITE("S3_4_C12_C13_6", value); break;
    case 15: ICC_WRITE("S3_4_C12_C13_7", value); break;
    default: break;
    }
}

uint64_t
gicv3_read_lr(int32_t n)
{
    switch (n) {
    case 0:  return ICC_READ("S3_4_C12_C12_0");
    case 1:  return ICC_READ("S3_4_C12_C12_1");
    case 2:  return ICC_READ("S3_4_C12_C12_2");
    case 3:  return ICC_READ("S3_4_C12_C12_3");
    case 4:  return ICC_READ("S3_4_C12_C12_4");
    case 5:  return ICC_READ("S3_4_C12_C12_5");
    case 6:  return ICC_READ("S3_4_C12_C12_6");
    case 7:  return ICC_READ("S3_4_C12_C12_7");
    case 8:  return ICC_READ("S3_4_C12_C13_0");
    case 9:  return ICC_READ("S3_4_C12_C13_1");
    case 10: return ICC_READ("S3_4_C12_C13_2");
    case 11: return ICC_READ("S3_4_C12_C13_3");
    case 12: return ICC_READ("S3_4_C12_C13_4");
    case 13: return ICC_READ("S3_4_C12_C13_5");
    case 14: return ICC_READ("S3_4_C12_C13_6");
    case 15: return ICC_READ("S3_4_C12_C13_7");
    default: return 0;
    }
}

uint64_t gicv3_read_elrsr(void) { return ICC_READ(ICH_ELRSR_EL2); }
uint64_t gicv3_read_eisr(void)  { return ICC_READ(ICH_EISR_EL2);  }
uint64_t gicv3_read_misr(void)  { return ICC_READ(ICH_MISR_EL2);  }
uint64_t gicv3_read_hcr(void)   { return ICC_READ(ICH_HCR_EL2);   }
void gicv3_write_hcr(uint64_t v) { ICC_WRITE(ICH_HCR_EL2, v); barrier_instr_full(); }

/* ── 杂项 ────────────────────────────────────────────────────── */

uint32_t
gicv3_get_typer(void)
{
    return read32((void *)GICD_TYPER);
}

uint32_t
gicv3_get_iidr(void)
{
    return read32((void *)GICD_IIDR);
}

uint32_t
cpu_num(void)
{
    return GICD_TYPER_CPU_NUM(read32((void *)GICD_TYPER));
}

/* GICv3 用 ICC_SGI1R_EL1 生成 SGI（GICv2 的 GICD_SGIR 已废弃）*/
void gicv3_ipi_send_single(int32_t irq, int32_t cpu)
{
    if (irq < 0 || irq > 15 || cpu < 0 || cpu >= 16) {
        logger_error("GICv3: bad IPI irq=%d cpu=%d\n", irq, cpu);
        return;
    }

    uint64_t sgi = ((uint64_t)irq & 0xFu) |
                   ((uint64_t)1 << (16 + cpu));   /* TargetList: Aff0 位图 */
    ICC_WRITE(ICC_SGI1R_EL1, sgi);
}
