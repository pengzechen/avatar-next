#ifndef __GICV3_H__
#define __GICV3_H__

#include "types.h"
#include "platform_cfg.h"

/* GICv3 模块内基地址（由 gicv3_init() 从 platform_get_mmio 填充） */
extern uintptr_t gicv3_gicd_base;
extern uintptr_t gicv3_gicr_base;

// GICv3 Distributor base
#define GICD_CTLR (gicv3_gicd_base + 0x0000)
#define GICD_TYPER (gicv3_gicd_base + 0x0004)
#define GICD_IIDR (gicv3_gicd_base + 0x0008)

#define GICD_IGROUPR (gicv3_gicd_base + 0x80)
#define GICD_ISENABLERn(n) (gicv3_gicd_base + 0x100 + (n) * 4)
#define GICD_ICENABLERn(n) (gicv3_gicd_base + 0x180 + (n) * 4)

#define GICD_IPRIORITYR(n) (gicv3_gicd_base + 0x400 + 4 * (n))
#define GICD_ITARGETSR(n) (gicv3_gicd_base + 0x800 + 4 * (n))
#define GICD_ICFGR(n) (gicv3_gicd_base + 0xc00 + 4 * (n))

// GICD bits
#define GICD_CTLR_ENNS_BIT (1u << 1)
#define GICD_CTLR_ENS_BIT (1u << 0)
#define GICD_CTLR_ARE_NS_BIT (1u << 4) // 开启 ARE_NS 之后，GICD 只负责 SPI，

// GICv3 Redistributor base (需根据平台定义)
#define GICR_CTLR (gicv3_gicr_base + 0x0000)
#define GICR_WAKER (gicv3_gicr_base + 0x0014)
#define GICR_IPRIORITYR(n) (gicv3_gicr_base + 0x0400 + 4 * (n))
#define GICR_SGI_BASE(cpu) (gicv3_gicr_base + 0x20000 * (cpu))
#define GICR_ISENABLER0(cpu) (GICR_SGI_BASE(cpu) + 0x10000 + 0x100)
#define GICR_ICENABLER0(cpu) (GICR_SGI_BASE(cpu) + 0x10000 + 0x180)

// System register interface
#define ICC_SRE_EL1 "S3_0_C12_C12_5"
#define ICC_PMR_EL1 "S3_0_C4_C6_0"
#define ICC_IAR1_EL1 "S3_0_C12_C12_0"
#define ICC_EOIR1_EL1 "S3_0_C12_C12_1"
#define ICC_CTLR_EL1 "S3_0_C12_C12_4"
#define ICC_IGRPEN1_EL1 "S3_0_C12_C12_7"

#define ICC_IAR_INTID_MASK 0xFFFFFFu

typedef struct gicv3_t
{
    unsigned int irq_nr;
} gicv3_t;

extern struct gicv3_t _gicv3;

void gicv3_init(void);
void gicv3_enable_int(int vector, bool enable);
bool gicv3_is_int_enabled(int int_id);
void gicv3_set_int_trigger(uint32_t int_id, int edge);
void gicv3_set_int_target(uint32_t int_id, uint8_t target_cpu_mask);
void gicv3_write_eoir(uint32_t irqstat);
uint32_t gicv3_read_iar(void);
uint32_t gicv3_iar_irqnr(uint32_t iar);

/* GICv2 compatibility shims (called via extern from timer/exception code) */
void gic_set_ipriority(uint32_t int_id, uint32_t priority);
void gic_write_dir(uint32_t irqstat);

#endif // __GICV3_H__