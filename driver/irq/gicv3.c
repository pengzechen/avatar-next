
#include "gicv3.h"
#include "mmio.h"
#include "klog.h"

/* GICv3 模块内基地址 */
uintptr_t gicv3_gicd_base = 0;
uintptr_t gicv3_gicr_base = 0;

struct gicv3_t _gicv3;

// 支持 GICv3 系统寄存器访问
#define WRITE_SYSREG(reg, val)                                 \
    do                                                         \
    {                                                          \
        if (__builtin_strcmp(reg, ICC_SRE_EL1) == 0)           \
        {                                                      \
            asm volatile("msr S3_0_C12_C12_5, %0" ::"r"(val)); \
        }                                                      \
        else if (__builtin_strcmp(reg, ICC_PMR_EL1) == 0)      \
        {                                                      \
            asm volatile("msr S3_0_C4_C6_0, %0" ::"r"(val));   \
        }                                                      \
        else if (__builtin_strcmp(reg, ICC_EOIR1_EL1) == 0)    \
        {                                                      \
            asm volatile("msr S3_0_C12_C12_1, %0" ::"r"(val)); \
        }                                                      \
        else if (__builtin_strcmp(reg, ICC_CTLR_EL1) == 0)     \
        {                                                      \
            asm volatile("msr S3_0_C12_C12_4, %0" ::"r"(val)); \
        }                                                      \
        else if (__builtin_strcmp(reg, ICC_IGRPEN1_EL1) == 0)  \
        {                                                      \
            asm volatile("msr S3_0_C12_C12_7, %0" ::"r"(val)); \
        }                                                      \
    } while (0)

#define READ_SYSREG(reg, outval)                                   \
    do                                                             \
    {                                                              \
        if (__builtin_strcmp(reg, ICC_IAR1_EL1) == 0)              \
        {                                                          \
            asm volatile("mrs %0, S3_0_C12_C12_0" : "=r"(outval)); \
        }                                                          \
    } while (0)

static inline void
write_sysreg(const char *reg, uint64_t val)
{
    WRITE_SYSREG(reg, val);
}

static inline uint64_t
read_sysreg(const char *reg)
{
    uint64_t val = 0;
    READ_SYSREG(reg, val);
    return val;
}

void gicv3_init(void)
{
    gicv3_gicd_base = platform_get_mmio("irq", "gicd");
    gicv3_gicr_base = platform_get_mmio("irq", "gicr");

    logger_info("GICv3: Initializing...\n");

    // ---- Distributor ----
    write32(0xFFFFFFFF, (void *)(GICD_IGROUPR)); // GICD_IGROUPR0 etc.

    uint32_t gicd_ctrlr = read32((void *)(uint64_t)GICD_CTLR);
    // 启用 组零，组一，ARE
    gicd_ctrlr |= GICD_CTLR_ENS_BIT | GICD_CTLR_ENNS_BIT | GICD_CTLR_ARE_NS_BIT;
    write32(gicd_ctrlr, (void *)GICD_CTLR);

    // 等待 GICD_CTLR.RWP (bit31) 清零 — ARE_NS 生效需要时间
    __asm__ volatile("dsb sy" ::: "memory");
    for (int rwp_to = 1000000; rwp_to > 0; rwp_to--) {
        if (!(read32((void *)GICD_CTLR) & (1u << 31)))
            break;
    }

    // ---- Redistributor ----
    uint32_t val = read32((void *)GICR_WAKER);
    val &= ~(1u << 1); // Clear ProcessorSleep
    write32(val, (void *)GICR_WAKER);
    __asm__ volatile("dsb sy" ::: "memory");
    int waker_to = 1000000;
    while ((read32((void *)GICR_WAKER) & (1u << 2)) && --waker_to > 0)
        ;
    if (waker_to <= 0)
        logger_info("GICv3: WARN GICR_WAKER ChildrenAsleep stuck, continuing\n");

    /*
     * 把本核 GICR SGI frame 的 SGI/PPI 全部标记为 NS Group1，并把所有 SGI/PPI
     * 优先级设为 0xA0（< PMR=0xFF）。
     * RK3588 上 ATF 默认会把 PPI 26（CNTP timer）放进 Group0(S)，仅 enable
     * ICC_IGRPEN1_EL1 时 CPU interface 会过滤掉，导致 BSP 收不到 timer。
     */
    {
        uintptr_t sgi_base = gicv3_gicr_base + 0x10000u; /* CPU0 SGI frame */
        write32(0xFFFFFFFFu, (void *)(sgi_base + 0x0080u)); /* GICR_IGROUPR0 */
        for (uint32_t off = 0; off < 32u; off += 4u)
            write32(0xA0A0A0A0u, (void *)(sgi_base + 0x0400u + off)); /* GICR_IPRIORITYRn */
        __asm__ volatile("dsb sy" ::: "memory");
    }

    // ---- CPU interface ----
    uint64_t sre = read_sysreg(ICC_SRE_EL1);
    sre |= 0x7; // SRE=1, DIB=1, DFB=1
    write_sysreg(ICC_SRE_EL1, sre);

    write_sysreg(ICC_CTLR_EL1, 0x0);

    write_sysreg(ICC_PMR_EL1, 0xFF); // 允许所有优先级

    write_sysreg(ICC_IGRPEN1_EL1, 0x1); // gicc ctrl r

    logger_info("GICv3: Init done\n");
}

/*
 * gicv3_find_cpu_gicr - 通过 MPIDR 亲和性在 GICR 链中找到当前核的 Redistributor 基地址
 *
 * 按 128KB stride 遍历 GICR 列表，匹配 GICR_TYPER[63:32] == MPIDR 亲和性字段。
 * 若未找到（GICR_TYPER.Last 提前结束），回退到 gicv3_gicr_base（CPU0 的 GICR）。
 */
static uintptr_t
gicv3_find_cpu_gicr(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    /*
     * 将 MPIDR_EL1 的 Aff3:Aff2:Aff1:Aff0 打包为 GICR_TYPER[63:32] 的格式：
     *   [63:56]=Aff3  [55:48]=Aff2  [47:40]=Aff1  [39:32]=Aff0
     * MPIDR_EL1: Aff3=[39:32], Aff2=[23:16], Aff1=[15:8], Aff0=[7:0]
     */
    uint32_t mpidr_aff = (uint32_t)(((mpidr >> 32) & 0xFFu) << 24) |
                         (uint32_t)(((mpidr >> 16) & 0xFFu) << 16) |
                         (uint32_t)(((mpidr >>  8) & 0xFFu) <<  8) |
                         (uint32_t)(  mpidr        & 0xFFu);
    uintptr_t gicr = gicv3_gicr_base;
    for (uint32_t step = 0; step < 16u; step++) {
        uint64_t typer = read64((const volatile void *)(gicr + 0x0008u));
        if ((uint32_t)(typer >> 32) == mpidr_aff)
            return gicr;
        if (typer & (1u << 4))   /* GICR_TYPER.Last: 无更多 redistributor */
            break;
        gicr += 0x20000UL;      /* GICv3 redistributor stride = 128 KB */
    }
    return gicv3_gicr_base;     /* 未找到时回退到 CPU0 GICR */
}

/*
 * gicv3_init_secondary - 次级核 per-CPU GICv3 初始化
 *
 * ARM GICv3 规范规定的唤醒顺序：
 *   1. 先使能 ICC_SRE_EL1（激活 CPU interface），然后 ISB
 *   2. 再清 GICR_WAKER.ProcessorSleep（DSB 确保写入到达设备），等待 ChildrenAsleep=0
 *   3. 最后配置 ICC_CTLR / ICC_PMR / ICC_IGRPEN1
 *
 * 若先等 ChildrenAsleep 而未设 ICC_SRE，CPU interface 处于未激活态，
 * GIC Redistributor 无法完成唤醒握手，ChildrenAsleep 永远不会变 0 → 死锁。
 */
void gicv3_init_secondary(void)
{
    uintptr_t gicr = gicv3_find_cpu_gicr();

    KLOG_INFO("[gicv3] secondary init: GICR=0x%lx\n", gicr);

    /* Step 1: 先使能 CPU interface 系统寄存器（ARM GICv3 唤醒前提条件） */
    uint64_t sre;
    __asm__ volatile("mrs %0, S3_0_C12_C12_5" : "=r"(sre)); /* ICC_SRE_EL1 */
    sre |= 0x7u;  /* SRE=1, DIB=1, DFB=1 */
    __asm__ volatile("msr S3_0_C12_C12_5, %0" :: "r"(sre));
    __asm__ volatile("isb");  /* ISB required after ICC_SRE_EL1 write */

    /* Step 2: 唤醒本核 Redistributor */
    uint32_t waker = read32((void *)(gicr + 0x0014)); /* GICR_WAKER */
    waker &= ~(1u << 1); /* Clear ProcessorSleep */
    write32(waker, (void *)(gicr + 0x0014));
    __asm__ volatile("dsb sy" ::: "memory");  /* 确保写入传播到 GIC 设备 */

    int waker_to = 1000000;
    while ((read32((void *)(gicr + 0x0014)) & (1u << 2)) && --waker_to > 0)
        ; /* Wait ChildrenAsleep = 0 */
    if (waker_to <= 0)
        KLOG_WARN("[gicv3] GICR_WAKER ChildrenAsleep timeout (GICR=0x%lx), continuing\n", gicr);

    /*
     * 把本核 GICR SGI frame 的 SGI/PPI 全部标记为 NS Group1，并设默认优先级 0xA0。
     * 必须在唤醒 Redistributor 之后、enable IGRPEN1 之前。
     */
    {
        uintptr_t sgi_base = gicr + 0x10000u;
        write32(0xFFFFFFFFu, (void *)(sgi_base + 0x0080u)); /* GICR_IGROUPR0 */
        for (uint32_t off = 0; off < 32u; off += 4u)
            write32(0xA0A0A0A0u, (void *)(sgi_base + 0x0400u + off)); /* GICR_IPRIORITYRn */
        __asm__ volatile("dsb sy" ::: "memory");
    }

    /* Step 3: 配置 ICC 寄存器 */
    __asm__ volatile("msr S3_0_C12_C12_4, %0" :: "r"((uint64_t)0));    /* ICC_CTLR_EL1 */
    __asm__ volatile("msr S3_0_C4_C6_0,   %0" :: "r"((uint64_t)0xFFu)); /* ICC_PMR_EL1 */
    __asm__ volatile("msr S3_0_C12_C12_7, %0" :: "r"((uint64_t)1u));    /* ICC_IGRPEN1_EL1 */
    __asm__ volatile("isb");

    KLOG_INFO("[gicv3] secondary init done\n");
}

void gicv3_enable_int(int int_id, bool enable)
{
    uint32_t mask = 1u << (int_id % 32);

    if (int_id < 32) {
        /*
         * SGI/PPI：每核 banked，必须写当前核的 GICR SGI frame。
         * 通过 MPIDR 亲和性遍历找到本核 GICR，偏移 0x10000 进入 SGI frame。
         */
        uintptr_t sgi_base = gicv3_find_cpu_gicr() + 0x10000u;
        if (enable)
            write32(mask, (void *)(sgi_base + 0x100u)); /* GICR_ISENABLER0 */
        else
            write32(mask, (void *)(sgi_base + 0x180u)); /* GICR_ICENABLER0 */
    } else {
        /* SPI：GICD 全局寄存器 */
        uint32_t reg = int_id / 32;
        if (enable)
            write32(mask, (void *)(uint64_t)GICD_ISENABLERn(reg));
        else
            write32(mask, (void *)(uint64_t)GICD_ICENABLERn(reg));
    }
}

bool gicv3_is_int_enabled(int int_id)
{
    uint32_t cpu_id = 0; // TODO: 多核时需获取当前 CPU ID
    uint32_t mask = 1u << (int_id % 32);
    uint32_t val;

    if (int_id < 32)
    {
        // SGI / PPI
        val = read32((void *)(uint64_t)GICR_ISENABLER0(cpu_id));
    }
    else
    {
        // SPI
        uint32_t reg = int_id / 32;
        val = read32((void *)(uint64_t)GICD_ISENABLERn(reg));
    }

    return (val & mask) ? true : false;
}

void gicv3_set_int_trigger(uint32_t int_id, int edge)
{
    // edge = 0: level, edge = 1: edge
    uint32_t reg = int_id / 16;
    uint32_t shift = (int_id % 16) * 2;

    uint32_t val = read32((void *)(uint64_t)GICD_ICFGR(reg));
    if (edge)
        val |= (1 << (shift + 1)); // 设置 bit1 = 1 → 边沿
    else
        val &= ~(1 << (shift + 1)); // 设置 bit1 = 0 → 电平
    write32(val, (void *)(uint64_t)GICD_ICFGR(reg));
}

void gicv3_set_int_target(uint32_t int_id, uint8_t target_cpu_mask)
{
    if (int_id < 32)
        return; // SGI/PPI 是 per-core，不用配置这里

    uint32_t reg = int_id / 4;    // 每个寄存器控制 4 个 SPI
    uint32_t offset = int_id % 4; // 在寄存器内的偏移
    uint32_t val = read32((void *)(uint64_t)GICD_ITARGETSR(reg));

    val &= ~(0xFF << (offset * 8));                     // 清空原有目标
    val |= ((uint32_t)target_cpu_mask << (offset * 8)); // 设置目标 CPU
    write32(val, (void *)(uint64_t)GICD_ITARGETSR(reg));
}

uint32_t
gicv3_read_iar(void)
{
    return (uint32_t)read_sysreg(ICC_IAR1_EL1);
}

uint32_t
gicv3_iar_irqnr(uint32_t iar)
{
    return iar & ICC_IAR_INTID_MASK;
}

void gicv3_write_eoir(uint32_t irqstat)
{
    write_sysreg(ICC_EOIR1_EL1, irqstat);
}

/*
 * GICv2 compatibility shims ─────────────────────────────────────────────────
 * timer_aarch64_impl.h 和 exception.c 通过 extern 声明直接调用这两个函数。
 * GICv3 中提供兼容实现，避免链接错误。
 */

/**
 * gic_set_ipriority - 设置中断优先级（GICv2 兼容接口）
 * 在 GICv3 中，优先级寄存器居址布局与 GICv2 相同。
 */
void gic_set_ipriority(uint32_t int_id, uint32_t priority)
{
    uint32_t reg   = int_id / 4;
    uint32_t shift = (int_id % 4) * 8;
    uint32_t val   = read32((void *)(uint64_t)GICD_IPRIORITYR(reg));
    val &= ~(0xFFu << shift);
    val |= (priority & 0xFFu) << shift;
    write32(val, (void *)(uint64_t)GICD_IPRIORITYR(reg));
}

/**
 * gic_write_dir - 撤销激活中断（GICv2 兼容接口）
 * GICv3 下 ICC_CTLR_EL1.EOImode=0：EOIR 写入同时完成 priority-drop + deactivate，
 * 无需额外撤销流程，此函数为空操作。
 */
void gic_write_dir(uint32_t irqstat)
{
    (void)irqstat;  /* EOImode=0: 取消已在 irq_eoi() 写 ICC_EOIR1_EL1 时完成 */
}