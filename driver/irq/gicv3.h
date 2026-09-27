#ifndef __GICV3_H__
#define __GICV3_H__

#include "types.h"
#include "platform_cfg.h"

/* ============================================================
 * GICv3 宿主驱动（EL2 / VHE）
 *
 * GICv3 与 GICv2 的根本差异：
 *   - CPU interface 不再是 MMIO，而是系统寄存器 ICC_*_EL1。
 *     VHE（E2H=1）下 EL2 访问 ICC_*_EL1 就落在物理 CPU interface 上，
 *     所以宿主用的是和普通 EL1 内核一样的寄存器名。
 *   - SGI/PPI 是 per-CPU 的，住在 GICR（Redistributor）里；
 *     GICD 只管 SPI。GICv2 那套「GICD 管一切 + ITARGETSR」不存在了。
 *   - vGIC 用 ICH_*_EL2 系统寄存器（ICH_LR<n>_EL2），
 *     不再是 GICH/GICV 的 MMIO。
 * ============================================================ */

/* ── 模块内基地址（由 gicv3_init() 从 platform_get_mmio 填充）────── */
extern uintptr_t gicv3_gicd_base;
extern uintptr_t gicv3_gicr_base;

/* 每个 Redistributor 占 2 个 64KB 帧：RD_base（0x00000）+ SGI_base（0x10000） */
#define GICR_STRIDE        0x20000UL
#define GICR_SGI_OFFSET    0x10000UL

/* ── Distributor（GICD）：ARE 开启后只负责 SPI ────────────────── */
#define GICD_CTLR         (gicv3_gicd_base + 0x0000)
#define GICD_TYPER        (gicv3_gicd_base + 0x0004)
#define GICD_IIDR         (gicv3_gicd_base + 0x0008)
#define GICD_IGROUPR(n)   (gicv3_gicd_base + 0x0080 + 4 * (n))
#define GICD_ISENABLER(n) (gicv3_gicd_base + 0x0100 + 4 * (n))
#define GICD_ICENABLER(n) (gicv3_gicd_base + 0x0180 + 4 * (n))
#define GICD_ISPENDR(n)   (gicv3_gicd_base + 0x0200 + 4 * (n))
#define GICD_ICPENDR(n)   (gicv3_gicd_base + 0x0280 + 4 * (n))
#define GICD_ISACTIVER(n) (gicv3_gicd_base + 0x0300 + 4 * (n))
#define GICD_ICACTIVER(n) (gicv3_gicd_base + 0x0380 + 4 * (n))
#define GICD_IPRIORITYR   (gicv3_gicd_base + 0x0400)
#define GICD_ITARGETSR    (gicv3_gicd_base + 0x0800) /* GICv3+ARE: RES0 */
#define GICD_ICFGR(n)     (gicv3_gicd_base + 0x0c00 + 4 * (n))
#define GICD_IROUTER(n)   (gicv3_gicd_base + 0x6000 + 8 * (n))
#define GICD_PIDR2        (gicv3_gicd_base + 0xffe8)

/* GICD_CTLR bits（单安全态视图） */
#define GICD_CTLR_ENABLE_G1NS_BIT (1u << 0)
#define GICD_CTLR_ENABLE_G1A_BIT  (1u << 1)
#define GICD_CTLR_ARE_S_BIT       (1u << 4)
#define GICD_CTLR_ARE_NS_BIT      (1u << 5)
#define GICD_CTLR_RWP_BIT         (1u << 31)

/* GICD_TYPER 字段 */
#define GICD_TYPER_IRQS(typer)   ((((typer) & 0x1fu) + 1u) * 32u)
#define GICD_TYPER_CPU_NUM(typer) ((((typer) >> 5) & 0x7u) + 1u)

/* ── Redistributor（GICR）RD_base 帧 ─────────────────────────── */
#define GICR_CTLR         (gicv3_gicr_base + 0x0000)
#define GICR_IIDR         (gicv3_gicr_base + 0x0004)
#define GICR_TYPER        (gicv3_gicr_base + 0x0008)
#define GICR_STATUSR      (gicv3_gicr_base + 0x0010)
#define GICR_WAKER        (gicv3_gicr_base + 0x0014)
#define GICR_PROPBASER    (gicv3_gicr_base + 0x0070)
#define GICR_PENDBASER    (gicv3_gicr_base + 0x0078)
#define GICR_PIDR2        (gicv3_gicr_base + 0xffe8)

/* GICR_WAKER bits */
#define GICR_WAKER_PROCESSOR_SLEEP (1u << 1)
#define GICR_WAKER_CHILDREN_ASLEEP (1u << 2)

/* ── Redistributor SGI_base 帧（per-CPU 的 SGI/PPI）──────────── */
#define GICR_SGI_BASE(cpu)   (gicv3_gicr_base + GICR_STRIDE * (cpu) + \
                              GICR_SGI_OFFSET)
#define GICR_IGROUPR0(cpu)   (GICR_SGI_BASE(cpu) + 0x0080)
#define GICR_ISENABLER0(cpu) (GICR_SGI_BASE(cpu) + 0x0100)
#define GICR_ICENABLER0(cpu) (GICR_SGI_BASE(cpu) + 0x0180)
#define GICR_ISPENDR0(cpu)   (GICR_SGI_BASE(cpu) + 0x0200)
#define GICR_ICPENDR0(cpu)   (GICR_SGI_BASE(cpu) + 0x0280)
#define GICR_ISACTIVER0(cpu) (GICR_SGI_BASE(cpu) + 0x0300)
#define GICR_ICACTIVER0(cpu) (GICR_SGI_BASE(cpu) + 0x0380)
#define GICR_IPRIORITYR(n)   (gicv3_gicr_base + 0x0400 + 4 * (n))
#define GICR_ICFGR0(cpu)     (GICR_SGI_BASE(cpu) + 0x0c00)
#define GICR_ICFGR1(cpu)     (GICR_SGI_BASE(cpu) + 0x0c04)

/* ── CPU interface 系统寄存器编码 ────────────────────────────── */
/*
 * 注意：**没有 ICC_SRE_EL2 的编码宏**。
 * VHE（HCR_EL2.E2H=1）下 ICC_SRE_EL2 不可单独访问 —— 用它的 EL2 编码
 * `S3_4_C12_C12_5` 会 UNDEFINED（实测 EC=0x0）。此时 EL2 的 GIC 寄存器
 * 统一走 ICC_*_EL1 名字：ICC_SRE_EL1 即 ICC_SRE_EL2 的别名，
 * Enable 位在 bit[3]（见 ICC_SRE_EL2_ENABLE）。
 */
#define ICC_SRE_EL1       "S3_0_C12_C12_5"
#define ICC_PMR_EL1       "S3_0_C4_C6_0"
#define ICC_IAR1_EL1      "S3_0_C12_C12_0"
#define ICC_EOIR1_EL1     "S3_0_C12_C12_1"
#define ICC_HPPIR1_EL1    "S3_0_C12_C12_2"
#define ICC_CTLR_EL1      "S3_0_C12_C12_4"
#define ICC_IGRPEN1_EL1   "S3_0_C12_C12_7"
#define ICC_DIR_EL1       "S3_0_C12_C11_1"
#define ICC_RPR_EL1       "S3_0_C12_C11_3"
#define ICC_SGI1R_EL1     "S3_0_C12_C11_5"

/* ICC_SRE_EL2 bits */
#define ICC_SRE_EL2_SRE     (1u << 0)
#define ICC_SRE_EL2_DFB     (1u << 1)
#define ICC_SRE_EL2_DIB     (1u << 2)
#define ICC_SRE_EL2_ENABLE  (1u << 3)

/* ICC_CTLR_EL1 bits */
#define ICC_CTLR_EL1_EOI_MODE_DROP (1u << 1)

#define ICC_IAR_INTID_MASK 0xFFFFFFu
#define ICC_INTID_SPURIOUS 1023u

/* ── Hypervisor（vGIC）系统寄存器编码，仅 EL2 可访问 ─────────── */
#define ICH_AP0R0_EL2     "S3_4_C12_C8_0"
#define ICH_AP1R0_EL2     "S3_4_C12_C9_0"
#define ICH_AP1R1_EL2     "S3_4_C12_C9_1"
#define ICH_AP1R2_EL2     "S3_4_C12_C9_2"
#define ICH_AP1R3_EL2     "S3_4_C12_C9_3"
#define ICH_HCR_EL2       "S3_4_C12_C11_0"
#define ICH_VTR_EL2       "S3_4_C12_C11_1"
#define ICH_MISR_EL2      "S3_4_C12_C11_2"
#define ICH_EISR_EL2      "S3_4_C12_C11_3"
#define ICH_ELRSR_EL2     "S3_4_C12_C11_5"
#define ICH_VMCR_EL2      "S3_4_C12_C11_7"

/* ICH_HCR_EL2 bits */
#define ICH_HCR_EN            (1u << 0)
#define ICH_HCR_UIEN          (1u << 1)
#define ICH_HCR_EOI_COUNT_EN  (1u << 2)
#define ICH_HCR_VGRP1_DIE     (1u << 8)  /* guest EOI 一个 Group1 中断时产生维护中断 */

/* ICH_MISR_EL2 bits */
#define ICH_MISR_EOI  (1u << 0)

/* ICH_LR<n>_EL2 位域（GICv3，64 位 LR）
 *
 * 注意 ICH_LR_ST_* 是**状态字段的取值**（写 LR 时左移 ICH_LR_STATE_SHIFT），
 * 不是可直接 AND 的位掩码；回读判定写成
 *   ((lr >> ICH_LR_STATE_SHIFT) & 3u) & ICH_LR_ST_ACTIVE
 */
#define ICH_LR_STATE_SHIFT  62
#define ICH_LR_STATE_MASK   (3ULL << ICH_LR_STATE_SHIFT)
#define ICH_LR_ST_INVALID        0u
#define ICH_LR_ST_PENDING        1u   /* 已排入，guest 未应答   */
#define ICH_LR_ST_ACTIVE         2u   /* guest 已应答，未 EOI  */
#define ICH_LR_ST_ACTIVE_PENDING 3u
#define ICH_LR_HW           (1ULL << 61)
#define ICH_LR_GROUP1       (1ULL << 60)
#define ICH_LR_PRIO_SHIFT   48
#define ICH_LR_PINTID_SHIFT 32
#define ICH_LR_VINTID_MASK  0xffffffffULL

#define GICV3_MAX_LRS 16

typedef struct gicv3_t
{
    unsigned int irq_nr;   /* GICD_TYPER 报告的 SPI+32 总数 */
    unsigned int nr_lrs;   /* ICH_VTR_EL2.ListRegs + 1，上限 GICV3_MAX_LRS */
} gicv3_t;

extern struct gicv3_t _gicv3;

/* ── 宿主初始化 ──────────────────────────────────────────────── */
void gicv3_init(void);
void gicv3_init_secondary(void);

/* ── 中断使能 / 查询 ─────────────────────────────────────────── */
void gicv3_enable_int(int int_id, bool enable);
bool gicv3_is_int_enabled(int int_id);
void gicv3_set_int_trigger(uint32_t int_id, int edge);
void gicv3_set_int_target(uint32_t int_id, uint8_t target_cpu_mask);

/* ── CPU interface 应答 / 结束 ───────────────────────────────── */
uint32_t gicv3_read_iar(void);
uint32_t gicv3_iar_irqnr(uint32_t iar);
void gicv3_write_eoir(uint32_t irqstat);

/* ── 兼容 GICv2 命名（timer / exception 以 extern 直接调用）──── */
void gic_set_ipriority(uint32_t int_id, uint32_t priority);
void gic_write_dir(uint32_t irqstat);

/* ── vGIC：ICH_LR<n>_EL2 访问 ────────────────────────────────── */
void     gicv3_write_lr(int32_t n, uint64_t value);
uint64_t gicv3_read_lr(int32_t n);
uint64_t gicv3_read_elrsr(void);
uint64_t gicv3_read_eisr(void);
uint64_t gicv3_read_misr(void);
uint64_t gicv3_read_hcr(void);
void     gicv3_write_hcr(uint64_t value);
uint32_t gicv3_vtr_nr_lrs(void);

/* ── 杂项 ────────────────────────────────────────────────────── */
uint32_t gicv3_get_typer(void);
uint32_t gicv3_get_iidr(void);
uint32_t cpu_num(void);
void gicv3_ipi_send_single(int32_t irq, int32_t cpu);

#endif // __GICV3_H__
