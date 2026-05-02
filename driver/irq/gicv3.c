
#include "gicv3.h"

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
    logger_info("GICv3: Initializing...\n");

    // ---- Distributor ----
    write32(0xFFFFFFFF, (void *)(GICD_IGROUPR)); // GICD_IGROUPR0 etc.

    uint32_t gicd_ctrlr = read32((void *)(uint64_t)GICD_CTLR);
    // 启用 组零，组一，ARE
    gicd_ctrlr |= GICD_CTLR_ENS_BIT | GICD_CTLR_ENNS_BIT | GICD_CTLR_ARE_NS_BIT;
    write32(gicd_ctrlr, (void *)GICD_CTLR);

    // ---- Redistributor ----
    uint32_t val = read32((void *)GICR_WAKER);
    val &= ~(1 << 1); // Clear ProcessorSleep
    write32(val, (void *)GICR_WAKER);
    while (read32((void *)GICR_WAKER) & (1 << 2))
        ;

    // ---- CPU interface ----
    uint64_t sre = read_sysreg(ICC_SRE_EL1);
    sre |= 0x7; // SRE=1, DIB=1, DFB=1
    write_sysreg(ICC_SRE_EL1, sre);

    write_sysreg(ICC_CTLR_EL1, 0x0);

    write_sysreg(ICC_PMR_EL1, 0xFF); // 允许所有优先级

    write_sysreg(ICC_IGRPEN1_EL1, 0x1); // gicc ctrl r

    logger_info("GICv3: Init done\n");
}

void gicv3_enable_int(int int_id, bool enable)
{
    uint32_t cpu_id = 0; // TODO: 多核时需获取当前 CPU 核心 ID

    uint32_t mask = 1u << (int_id % 32);

    logger_info("enable_int: int_id=%d, enable=%d\n", int_id, enable);

    if (int_id < 32)
    {
        // SGI/PPI (per-core)
        if (enable)
            write32(mask, (void *)(uint64_t)GICR_ISENABLER0(cpu_id));
        else
            write32(mask, (void *)(uint64_t)GICR_ICENABLER0(cpu_id));
    }
    else
    {
        // SPI (shared)
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