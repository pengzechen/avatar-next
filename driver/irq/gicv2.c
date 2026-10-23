/*
   SPDX-License-Identifier: MIT

   Copyright (c) 2026 Avatar OS project
*/

#include "mmio.h"
#include "gicv2.h"
#include "driver/driver.h"
#include "driver/irq.h"
#include "driver/time.h" /* CNTP_TIMER */

/* GICv2 模块内基地址 */
uintptr_t gicv2_gicd_base = 0;
uintptr_t gicv2_gicc_base = 0;
uintptr_t gicv2_gich_base = 0;

struct gic_t _gicv2;

void gic_test_init(void)
{
    KLOG_GIC("GIC: GICD enable %s\n", read32((void *)GICD_CTLR) ? "ok" : "error");
    KLOG_GIC("GIC: GICC enable %s\n", read32((void *)GICC_CTLR) ? "ok" : "error");
    KLOG_GIC("GIC: IRQ numbers: %d\n", _gicv2.irq_nr);
    KLOG_GIC("GIC: CPU count: %d\n", cpu_num());
}

/*
   ===========================================
   下面是gic的初始化函数, 包括el1kernel的初始化和hypervisor的初始化
   ===========================================
*/

/* el1 kernel gicc的初始化。smp启动副核执行 */
void gicc_init()
{
    logger_gic_debug("GIC: Initializing GICC for EL1 kernel\n");

    /* 设置优先级 为 0xf8 */
    write32(0xff - 7, (void *)GICC_PMR);
    logger_gic_debug("GIC: Set PMR to 0x%x\n", 0xff - 7);

    /* EOImodeNS, bit [9] Controls the behavior of Non-secure accesses to GICC_EOIR GICC_AEOIR, and GICC_DIR */
    write32(GICC_CTRL_ENABLE_GROUP0 | (1 << 9), (void *)GICC_CTLR);
    logger_gic_debug("GIC: GICC enabled for Group0\n");
}

/* el2 hypervisor gicc的初始化。smp启动副核执行 */
void gicc_el2_init()
{
    logger_gic_debug("GIC: Initializing GICC for EL2 hypervisor\n");

    /* 设置优先级 为 0xf8 */
    write32(0xff - 7, (void *)GICC_PMR);
    logger_gic_debug("GIC: Set PMR to 0x%x\n", 0xff - 7);

    /*
       EOImodeNS, bit [9] Controls the behavior of Non-secure accesses to GICC_EOIR GICC_AEOIR, and GICC_DIR
       写 EOI 只清除 pending, 需要写 DIR 手动清除 active
    */
    write32(GICC_CTRL_ENABLE_GROUP0 | (1 << 9), (void *)GICC_CTLR);
    logger_gic_debug("GIC: GICC enabled for Group0 with EOI mode\n");

    /* bit [2] 当虚拟中断列表寄存器中没有条目时, 会产生中断。 */
    write32((1 << 0), (void *)GICH_HCR);
    write32((1 << 0) | (1 << 1), (void *)GICH_VMCR);
    logger_gic_debug("GIC: GICH initialized for virtualization\n");
}

/* gicd g0, g1  gicc enable。smp启动首核执行 */
void gic_init(void)
{
    gicv2_gicd_base = platform_get_mmio("irq", "gicd");
    gicv2_gicc_base = platform_get_mmio("irq", "gicc");
    gicv2_gich_base = platform_get_mmio("irq", "gich");

    KLOG_GIC("GIC: Initializing GIC distributor and CPU interface\n");

    _gicv2.irq_nr = GICD_TYPER_IRQS(read32((void *)GICD_TYPER));
    if (_gicv2.irq_nr > 1020) {
        logger_warn("GIC: IRQ number %d exceeds maximum, capping to 1020\n", _gicv2.irq_nr);
        _gicv2.irq_nr = 1020;
    }
    logger_gic_debug("GIC: Detected %d IRQ lines\n", _gicv2.irq_nr);

    /* GICD 启用组0中断, 组1中断转发, 服从优先级规则 */
    write32(GICD_CTRL_ENABLE_GROUP0 | GICD_CTRL_ENABLE_GROUP1, (void *)GICD_CTLR);
    logger_gic_debug("GIC: GICD enabled for Group0 and Group1\n");

    gicc_init();

    gic_test_init();
    logger_info("GIC: Initialization completed successfully\n");
}

/* 次级核只需要初始化本地 CPU interface(不重复初始化 distributor) */
void gic_init_secondary(void)
{
    gicc_el2_init();
}

/* gicd g0, g1  gicc,  gich enable。 smp启动首核执行 */
void gic_virtual_init(void)
{
    KLOG_GIC("GIC: Initializing GIC for virtualization\n");

    /* 关键：必须先从平台拿 MMIO base, 否则后续所有寄存器宏都解析成
       (0 + offset), 写入物理地址 0(QEMU virt 那里是 RAM/device,
       静默丢弃), 中断永远不会送达。
       gic_init() 已经做了这件事；EL2/VHE 路径走 gic_virtual_init,
       之前漏写导致 timer IRQ 从未投递。
    */
    gicv2_gicd_base = platform_get_mmio("irq", "gicd");
    gicv2_gicc_base = platform_get_mmio("irq", "gicc");
    gicv2_gich_base = platform_get_mmio("irq", "gich");
    KLOG_GIC("GIC: base addrs: gicd=0x%llx gicc=0x%llx gich=0x%llx\n", (unsigned long long)gicv2_gicd_base,
             (unsigned long long)gicv2_gicc_base, (unsigned long long)gicv2_gich_base);

    /* 获得 gicd irq numbers */
    _gicv2.irq_nr = GICD_TYPER_IRQS(read32((void *)GICD_TYPER));
    if (_gicv2.irq_nr > 1020) {
        logger_warn("GIC: IRQ number %d exceeds maximum, capping to 1020\n", _gicv2.irq_nr);
        _gicv2.irq_nr = 1020;
    }
    logger_gic_debug("GIC: Detected %d IRQ lines for virtualization\n", _gicv2.irq_nr);

    /* GICD 启用组0中断, 组1中断转发, 服从优先级规则 */
    write32(GICD_CTRL_ENABLE_GROUP0 | GICD_CTRL_ENABLE_GROUP1, (void *)GICD_CTLR);
    logger_gic_debug("GIC: GICD enabled for Group0 and Group1 (virtual)\n");

    gicc_el2_init();

    gic_test_init();

    KLOG_GIC("GIC: GICH enable %s\n", read32((void *)GICH_HCR) ? "ok" : "error");
    logger_info("GIC: Virtualization initialization completed\n");
}

/*
   ===========================================
   下面是一些get, set函数。 读取寄存器值
   ===========================================
*/

uint32_t gic_get_typer(void)
{
    return read32((void *)GICD_TYPER);
}

uint32_t gic_get_iidr(void)
{
    return read32((void *)GICD_IIDR);
}

uint32_t gic_read_iar(void)
{
    uint32_t iar = read32((void *)GICC_IAR);
    uint32_t irq_id = iar & GICC_IAR_INT_ID_MASK;

    /* 只在非虚假中断时记录调试信息 */
    if (irq_id != GICC_INT_SPURIOUS) {
        /* 这个太多了 */
    }

    return iar;
}

uint32_t gic_iar_irqnr(uint32_t iar)
{
    return iar & GICC_IAR_INT_ID_MASK;
}

void gic_write_eoir(uint32_t irqstat)
{
    uint32_t irq_id = irqstat & GICC_IAR_INT_ID_MASK;
    write32(irqstat, (void *)GICC_EOIR);

    (void)irq_id; /* Suppress unused variable warning */
    /* 这个太多了 */
}

void gic_write_dir(uint32_t irqstat)
{
    uint32_t irq_id = irqstat & GICC_IAR_INT_ID_MASK;
    write32(irqstat, (void *)GICC_DIR);

    (void)irq_id; /* Suppress unused variable warning */
    /* 这个太多了 */
}

/* 发送给特定的核(某个核) */
void gic_ipi_send_single(int32_t irq, int32_t cpu)
{
    if (cpu >= 8) {
        logger_error("GIC: Invalid CPU ID %d for IPI (max 7)\n", cpu);
        return;
    }
    if (irq >= 16) {
        logger_error("GIC: Invalid SGI IRQ %d for IPI (max 15)\n", irq);
        return;
    }

    write32(1 << (cpu + 16) | irq, (void *)GICD_SGIR);
    logger_gic_debug("GIC: Sent IPI %d to CPU %d\n", irq, cpu);
}

/* The number of implemented CPU interfaces. */
uint32_t cpu_num(void)
{
    return GICD_TYPER_CPU_NUM(read32((void *)GICD_TYPER));
}

/*
   Enables the given interrupt.
   W1S	Write-1-to-Set	写入 1 会置位(set)对应位, 写 0 不影响
   W1C	Write-1-to-Clear	写入 1 会清零(clear)对应位, 写 0 不影响
*/
void gic_enable_int(int32_t vector, int32_t enabled)
{
    int32_t reg = vector >> 5;                     /* vec / 32 */
    int32_t mask = 1 << (vector & ((1 << 5) - 1)); /* vec % 32 */
    if (enabled)
        write32(mask, (void *)GICD_ISENABLER(reg));
    else
        write32(mask, (void *)GICD_ICENABLER(reg));
    /* logger("set enable: reg: %d, mask: 0x%llx\n", reg, mask); */
    logger_gic_debug("GIC: %s int: %d\n", enabled ? "enable" : "disable", vector);
}

/* Check the given interrupt. */
int32_t gic_get_enable(int32_t vector)
{
    int32_t reg = vector >> 5;                     /* vec / 32 */
    int32_t mask = 1 << (vector & ((1 << 5) - 1)); /* vec % 32 */
    uint32_t val = read32((void *)GICD_ISENABLER(reg));

    /* logger("get enable: reg: %llx, mask: %llx, value: %llx\n", reg, mask, val); */
    logger_gic_debug("GIC: int %d is %s\n", vector, val & mask ? "enabled" : "disabled");
    return (val & mask) != 0;
}

/* An auxiliary function for determining whether it is SGI, returning 1 indicates it is SGI and 0 indicates it is not */
static int32_t gic_is_sgi(int32_t int_id)
{
    return int_id >= 0 && int_id <= 15; /* SGI 一般是0-15号中断 */
}

/* Set the Active status of the interrupt. act=1 activates, act=0 clears the activation */
void gic_set_active(int32_t int_id, int32_t act)
{
    if (int_id < 0 || int_id >= _gicv2.irq_nr) {
        logger_error("GIC: Invalid interrupt ID %d for set_active\n", int_id);
        return;
    }

    int32_t reg = int_id / 32;
    int32_t mask = 1 << (int_id % 32);

    if (act) {
        write32(mask, (void *)GICD_ISACTIVER(reg));
    } else {
        write32(mask, (void *)GICD_ICACTIVER(reg));
    }
    logger_gic_debug("GIC: Set IRQ %d active state to %d\n", int_id, act);
}

/* Set the Pending status of the interrupt. pend=1 sets Pending, pend=0 clears Pending */
void gic_set_pending(int32_t int_id, int32_t pend, int32_t target_cpu)
{
    if (int_id < 0 || int_id >= _gicv2.irq_nr) {
        logger_error("GIC: Invalid interrupt ID %d for set_pending\n", int_id);
        return;
    }

    if (gic_is_sgi(int_id)) {
        int32_t reg = int_id / 4;       /* 每个 GICD_SPENDSGIR 管 4 个 SGI */
        int32_t off = (int_id % 4) * 8; /* 每个 SGI 占 8 bit(一个字节, 每个 bit 表示一个 CPU) */

        if (target_cpu < 0 || target_cpu >= 8) {
            logger_error("GIC: Invalid CPU ID %d for SGI pending\n", target_cpu);
            return;
        }

        if (pend) {
            /* 设置指定 CPU 的 SGI pending 位 */
            write32(1 << (off + target_cpu), (void *)GICD_SPENDSGIR(reg));
        } else {
            /* 清除该 SGI 在所有 CPU 上的 pending(你也可以只清除指定 CPU 的位) */
            write32(1 << (off + target_cpu), (void *)GICD_CPENDSGIR(reg));
        }

        logger_gic_debug("GIC: Set SGI %d pending=%d for CPU %d\n", int_id, pend, target_cpu);
    } else {
        int32_t reg = int_id / 32;
        int32_t mask = 1 << (int_id % 32);

        if (pend) {
            write32(mask, (void *)GICD_ISPENDER(reg));
        } else {
            write32(mask, (void *)GICD_ICPENDER(reg));
        }

        logger_gic_debug("GIC: Set IRQ %d pending=%d\n", int_id, pend);
    }
}

/* Set the interrupt priority */
void gic_set_ipriority(uint32_t vector, uint32_t pri)
{
    uint32_t n = vector >> 2; /* 哪个寄存器 */
    uint32_t m = vector & 3;  /* 寄存器内的哪个字节 */
    uint64_t addr = GICD_IPRIORITYR(n);
    uint32_t val = read32((void *)(uint64_t)addr);

    /* 设置第 m 字节的优先级 */
    uint8_t priority = (pri << 3) | (1 << 7); /* GICv2: [7]=1 表示 Group1 (非 secure) */
    val &= ~(0xFF << (8 * m));
    val |= (priority << (8 * m));

    write32(val, (void *)(uint64_t)addr);
    logger_gic_debug("GIC: set int: %d(n: %u, m: %u) priority: 0x%x\n", vector, n, m, pri);
}

/* Get the interrupt priority */
int32_t gic_get_ipriority(int32_t vector)
{
    int32_t n = vector >> 2;                               /* 每个 IPRIORITYR 寄存器存储 4 个中断(每个占 8 bit) */
    int32_t m = vector & 0x3;                              /* 当前中断在该寄存器中的偏移(0~3) */
    uint32_t reg_val = read32((void *)GICD_IPRIORITYR(n)); /* 读取整个 32-bit 寄存器 */
    int32_t priority = (reg_val >> (m * 8)) & 0xFF;        /* 提取目标中断的 8-bit 优先级 */
    return priority;
}

/* Get the target CPU for a specific interrupt */
int32_t gic_get_target(int32_t int_id)
{
    int32_t idx = (int_id * 8) / 32;
    int32_t off = (int_id * 8) % 32;
    uint32_t val = read32((void *)(GICD_ITARGETSR(idx)));
    return (val >> off) & 0xFF;
}

/* Set the target CPU for a specific interrupt */
void gic_set_target(int32_t int_id, uint8_t target)
{
    if (int_id < GIC_FIRST_SPI || int_id >= _gicv2.irq_nr) {
        logger_error("GIC: Invalid SPI interrupt ID %d for set_target\n", int_id);
        return;
    }

    if (target == 0) {
        logger_warn("GIC: Setting target to 0 for IRQ %d (no CPU selected)\n", int_id);
    }

    int32_t idx = (int_id * 8) / 32;
    int32_t off = (int_id * 8) % 32;
    uint32_t mask = 0xFF << off;

    uint32_t old_val = read32((void *)(GICD_ITARGETSR(idx)));
    uint32_t new_val = (old_val & ~mask) | ((target << off) & mask);
    logger_gic_debug("GIC: Set IRQ %d target to 0x%x\n", int_id, target);
    write32(new_val, (void *)(GICD_ITARGETSR(idx)));
}

/* Set the interrupt configuration (edge/level) */
void gic_set_icfgr(int32_t int_id, uint8_t cfg)
{
    if (int_id < 16) {
        logger_warn("GIC: Cannot configure SGI %d (SGIs are always edge-triggered)\n", int_id);
        return;
    }

    if (int_id >= _gicv2.irq_nr) {
        logger_error("GIC: Invalid interrupt ID %d for configuration\n", int_id);
        return;
    }

    uint32_t reg_index = (int_id * 2) / 32;
    uint32_t bit_offset = (int_id * 2) % 32;
    uint32_t mask = 0b11 << bit_offset;

    volatile uint32_t *reg = (volatile uint32_t *)GICD_ICFGR(reg_index);
    uint32_t val = *reg;

    val = (val & ~mask) | (((uint32_t)(cfg & 0x3) << bit_offset) & mask);
    *reg = val;

    logger_gic_debug("GIC: Set IRQ %d configuration to %s\n", int_id,
                     (cfg & 0x2) ? "edge-triggered" : "level-sensitive");
}

uint32_t gic_make_virtual_hardware_interrupt(uint32_t vector, uint32_t pintvec, int32_t pri, bool grp1)
{
    uint32_t mask = 0x90000000; /* grp0 hw pending */
    mask |= ((uint32_t)(pri & 0xf8) << 20) | (vector & (0x1ff)) | ((pintvec & 0x1ff) << 10) | ((uint32_t)grp1 << 30);
    return mask;
}

uint32_t gic_make_virtual_software_interrupt(uint32_t vector, int32_t pri, bool grp1)
{
    uint32_t mask = 0x10000000; /* grp0  pending */
    mask |= ((uint32_t)(pri & 0xf8) << 20) | (vector & (0x1ff)) | ((uint32_t)grp1 << 30);
    return mask;
}

uint32_t gic_make_virtual_software_sgi(uint32_t vector, int32_t cpu_id, int32_t pri, bool grp1)
{
    uint32_t mask = 0x10000000; /* grp0  pending */
    mask |= ((uint32_t)(pri & 0xf8) << 20) | (vector & (0x1ff)) | ((uint32_t)grp1 << 30) | ((uint32_t)cpu_id << 10);
    return mask;
}

/*
   ===================================
   下面是关于 GICH 的函数
   ===================================
*/

uint32_t gic_read_lr(int32_t n)
{
    return read32((void *)GICH_LR(n));
}

int32_t gic_lr_read_pri(uint32_t lr_value)
{
    return (lr_value & (0xf8 << 20)) >> 20;
}

uint32_t gic_lr_read_vid(uint32_t lr_value)
{
    return lr_value & 0x1ff;
}

uint32_t gic_apr()
{
    return read32((void *)GICH_APR);
}

uint32_t gic_elsr0()
{
    return read32((void *)GICH_ELSR0);
}

uint32_t gic_elsr1()
{
    return read32((void *)GICH_ELSR1);
}

void gic_write_lr(int32_t n, uint32_t mask)
{
    if (n < 0 || n >= GICH_LR_NUM) {
        logger_error("GIC: Invalid LR index %d (max %d)\n", n, GICH_LR_NUM - 1);
        return;
    }

    write32(mask, (void *)GICH_LR(n));
    uint32_t vid = gic_lr_read_vid(mask);

    (void)vid; /* Suppress unused variable warning */
    /* 这个太多了 */
}

/* 启用非优先级中断, 这种中断允许在虚拟化环境下处理一些低优先级的中断。 */
void gic_set_np_int(void)
{
    write32(read32((void *)GICH_HCR) | (1 << 3), (void *)GICH_HCR);
    logger_gic_debug("GIC: Enabled non-priority interrupts in hypervisor\n");
}

/* 禁用非优先级中断, 确保虚拟机只处理优先级较高的中断。 */
void gic_clear_np_int(void)
{
    write32(read32((void *)GICH_HCR) & ~(1 << 3), (void *)GICH_HCR);
    logger_gic_debug("GIC: Disabled non-priority interrupts in hypervisor\n");
}

/* ── 公共 IRQ 接口实现 ───────────────────────────────────────── */

#define GICV2_RESCHED_SGI 15u

static void gicv2_dispatch(void *frame)
{
    uint32_t iar = gic_read_iar();
    uint32_t irq = gic_iar_irqnr(iar);

    if (irq >= 1020u)
        return;

    irq_dispatch(irq, frame);

    /* GICC_EOIR 期望 IAR 原值(含 CPUID)；GICC_DIR 在 EOImode=1 时做 deactivate */
    gic_write_eoir(iar);
    gic_write_dir(iar);
}

static int gicv2_enable_api(unsigned irq, bool on)
{
    if (irq >= 1020u)
        return -2;
    gic_enable_int((int32_t)irq, on ? 1 : 0);
    return 0;
}

static int gicv2_set_priority_api(unsigned irq, unsigned prio)
{
    if (irq >= 1020u)
        return -2;
    gic_set_ipriority(irq, prio & 0xFFu);
    return 0;
}

static int gicv2_secondary_init_api(void)
{
    gic_init_secondary();
    return 0;
}

static int gicv2_send_ipi_api(uint32_t hw_id)
{
    uint32_t aff0 = hw_id & 0xFFu;
    if (aff0 >= 8u) /* GICD_SGIR 的 TargetList 只有 8 位 */
        return -2;
    gic_ipi_send_single(GICV2_RESCHED_SGI, (int32_t)aff0);
    return 0;
}

static int gicv2_register_ipi_api(irq_handler_fn fn, void *ctx)
{
    int rc = irq_register(GICV2_RESCHED_SGI, fn, ctx);
    if (rc != 0)
        return rc;

    gic_set_ipriority(GICV2_RESCHED_SGI, 0);
    gic_enable_int(GICV2_RESCHED_SGI, 1);
    return 0;
}

static int gicv2_register_timer_api(irq_handler_fn fn, void *ctx)
{
    if (CNTP_TIMER == 0 || gicv2_gicd_base == 0 || gicv2_gicc_base == 0)
        return -1;

    int rc = irq_register(CNTP_TIMER, fn, ctx);
    if (rc != 0)
        return rc;

    gic_set_ipriority(CNTP_TIMER, 0);
    gic_enable_int((int32_t)CNTP_TIMER, 1);
    return 0;
}

static int gicv2_timer_enable_api(void)
{
    if (CNTP_TIMER == 0)
        return -1;
    gic_enable_int((int32_t)CNTP_TIMER, 1);
    return 0;
}

static uint64_t gicv2_cpu_hw_id_api(void)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr & 0xFFFFFFULL;
}

static void gicv2_debug_dump_api(void)
{
    KLOG_SMP("[smp-test] GIC base: gicd=0x%llx gicc=0x%llx\n", (unsigned long long)gicv2_gicd_base,
             (unsigned long long)gicv2_gicc_base);
    uint32_t gicd_ctlr = read32((void *)(gicv2_gicd_base + 0x000));
    uint32_t gicd_isen0 = read32((void *)(gicv2_gicd_base + 0x100));
    uint32_t gicd_ispend = read32((void *)(gicv2_gicd_base + 0x200));
    uint32_t gicd_pri26 = read32((void *)(gicv2_gicd_base + 0x400 + (26 / 4) * 4));
    uint32_t gicc_ctlr = read32((void *)(gicv2_gicc_base + 0x000));
    uint32_t gicc_pmr = read32((void *)(gicv2_gicc_base + 0x004));
    uint32_t gicc_hppir = read32((void *)(gicv2_gicc_base + 0x018));
    uint32_t gicc_rpr = read32((void *)(gicv2_gicc_base + 0x014));
    KLOG_SMP("[smp-test] GICD CTLR=0x%x ISENABLER0=0x%x ISPENDR0=0x%x IPRIORITY[26..]=0x%x\n", gicd_ctlr, gicd_isen0,
             gicd_ispend, gicd_pri26);
    KLOG_SMP("[smp-test] GICC CTLR=0x%x PMR=0x%x HPPIR=0x%x RPR=0x%x\n", gicc_ctlr, gicc_pmr, gicc_hppir, gicc_rpr);
}

static const irq_ops_t gicv2_irq_ops = {
    .dispatch = gicv2_dispatch,
    .enable = gicv2_enable_api,
    .set_priority = gicv2_set_priority_api,
    .secondary_init = gicv2_secondary_init_api,
    .send_ipi = gicv2_send_ipi_api,
    .register_ipi = gicv2_register_ipi_api,
    .register_timer = gicv2_register_timer_api,
    .timer_enable = gicv2_timer_enable_api,
    .cpu_hw_id = gicv2_cpu_hw_id_api,
    .debug_dump = gicv2_debug_dump_api,
    .virt_supported = false, /* vGICv2 接口未接入 VMM，GICv3 才有 vGIC 模型 */
};

static int gicv2_probe(const device_t *dev)
{
    (void)dev; /* 阶段 1：gic_virtual_init() 仍直接读平台配置 */
    irq_bind(&gicv2_irq_ops);
    gic_virtual_init();
    return 0;
}

DRIVER_ENTRY(driver_gicv2) = {
    .name = "gicv2",
    .cls = DRV_IRQ,
    .phase = DRV_PHASE_IRQ,
    .start_phase = DRV_PHASE_COUNT,
    .probe = gicv2_probe,
};