/*
 * driver/usb/dwc2.c — DWC2 主机控制器初始化和根端口操作
 *
 * 翻译自 sg200x-bsp/src/usb/host/dwc2/controller.rs
 * 目标平台：SG2002 (cv182x-host feature)，使用 CV182x 片内 USB2 PHY。
 *
 * 通道分配约定：
 *   - 通道 0：EP0 控制传输
 *   - 通道 1：Bulk / Isoch
 */

#include "usb/dwc2_regs.h"
#include "usb/usb.h"
#include "platform_cfg.h"
#include "mmio.h"
#include "klog.h"
#include "mm_vm.h"
#include "spinlock.h"
#if ARCH_RISCV64
#include "irq/plic.h"
#endif

/* ==========================================================================
 * 1. 模块内全局变量
 * ========================================================================== */

static uintptr_t g_dwc2_base      = 0;
static uintptr_t g_dwc2_phy_base  = 0;
static bool      g_dwc2_inited    = false;
static uint32_t  g_dwc2_irq       = 0;
static volatile uint64_t g_dwc2_irq_count = 0;
static volatile uint32_t g_dwc2_last_gintsts = 0;
static volatile uint32_t g_dwc2_last_haint = 0;
static volatile uint32_t g_dwc2_hcint_shadow[DWC2_MAX_HOST_CHANNELS];
static spinlock_noirq_t g_dwc2_hcint_shadow_lock = SPINLOCK_NOIRQ_INIT;

/* 主机通道数量（从 GHWCFG2 读取） */
static uint32_t  g_num_host_channels = 0;
/* 是否使用内部 DMA 架构 (GHWCFG2.ARCH == 2) */
static bool      g_internal_dma     = false;

/* ==========================================================================
 * 2. 寄存器访问辅助 (内联读写，直接使用 mmio.h 的 read32/write32)
 * ========================================================================== */

static inline uint32_t dwc2_read32(uintptr_t off)
{
    return read32((void *)(g_dwc2_base + off));
}

static inline void dwc2_write32(uintptr_t off, uint32_t val)
{
    write32(val, (void *)(g_dwc2_base + off));
}

static inline uint32_t dwc2_phy_read32(uintptr_t off)
{
    return read32((void *)(g_dwc2_phy_base + off));
}

static inline void dwc2_phy_write32(uintptr_t off, uint32_t val)
{
    write32(val, (void *)(g_dwc2_phy_base + off));
}

/* 主机通道寄存器读写 */
static inline uint32_t hc_read32(uint32_t ch, uintptr_t reg_off)
{
    return read32((void *)(g_dwc2_base + DWC2_OFF_HC_BASE + ch * DWC2_OFF_HC_STRIDE + reg_off));
}

static inline void hc_write32(uint32_t ch, uintptr_t reg_off, uint32_t val)
{
    write32(val, (void *)(g_dwc2_base + DWC2_OFF_HC_BASE + ch * DWC2_OFF_HC_STRIDE + reg_off));
}

static bool dwc2_irq_log_sample(uint64_t n)
{
    return n <= 8 || (n <= 4096 && (n & (n - 1)) == 0);
}

static void dwc2_usb_irq_handler(uint32_t irq, void *ctx)
{
    (void)ctx;
    uint32_t gintmsk = dwc2_read32(DWC2_OFF_GINTMSK);
    uint32_t gintsts = dwc2_read32(DWC2_OFF_GINTSTS) & gintmsk;
    uint32_t haint = 0;

    if (gintsts & GINTSTS_HCHINT) {
        haint = dwc2_read32(DWC2_OFF_HAINT);
        /* Only service CH0 (control) in IRQ — CH1 (isoch) is polled. */
        if (haint & (1U << 0)) {
            uint32_t hcint = hc_read32(0, HC_OFF_INT);
            g_dwc2_hcint_shadow[0] |= hcint;
            if (hcint)
                hc_write32(0, HC_OFF_INT, hcint);
        }
    }

    uint32_t clear = gintsts & ~GINTSTS_RXFLVL;
    if (clear)
        dwc2_write32(DWC2_OFF_GINTSTS, clear);

    uint64_t n = ++g_dwc2_irq_count;
    g_dwc2_last_gintsts = gintsts;
    g_dwc2_last_haint = haint;
    if (dwc2_irq_log_sample(n)) {
        KLOG_WARN("[DWC2 IRQ] #%llu plic=%u GINTSTS=0x%08x HAINT=0x%08x GINTMSK=0x%08x HC0=0x%08x HC1=0x%08x\n",
                  n, irq, gintsts, haint, gintmsk,
                  g_dwc2_hcint_shadow[0], g_dwc2_hcint_shadow[1]);
    }
}

/* ==========================================================================
 * 3. 公开 API：基址设置
 * ========================================================================== */

void dwc2_usb_set_base_virt(uintptr_t base)
{
    g_dwc2_base = base;
}

void dwc2_usb_set_phy_base_virt(uintptr_t phy_base)
{
    g_dwc2_phy_base = phy_base;
}

uintptr_t dwc2_get_base_virt(void)
{
    return g_dwc2_base;
}

uint64_t dwc2_usb_irq_count(void)
{
    return g_dwc2_irq_count;
}

uint32_t dwc2_usb_last_irq_gintsts(void)
{
    return g_dwc2_last_gintsts;
}

uint32_t dwc2_usb_last_irq_haint(void)
{
    return g_dwc2_last_haint;
}

uint32_t dwc2_usb_take_hcint(uint32_t ch)
{
    if (ch >= DWC2_MAX_HOST_CHANNELS)
        return 0;

    spin_lock_irqsave(&g_dwc2_hcint_shadow_lock);
    uint32_t hcint = g_dwc2_hcint_shadow[ch];
    g_dwc2_hcint_shadow[ch] = 0;
    spin_unlock_irqrestore(&g_dwc2_hcint_shadow_lock);
    return hcint;
}

/* ==========================================================================
 * 4. 自旋延迟 (约 N 次空循环)
 * ========================================================================== */

static void spin_delay(uint32_t iterations)
{
    timer_spin(iterations);
}

/* ==========================================================================
 * 5. AHB 空闲等待
 * ========================================================================== */

static int wait_ahb_idle(void)
{
    for (uint32_t i = 0; i < 3000000; i++) {
        if (dwc2_read32(DWC2_OFF_GRSTCTL) & GRSTCTL_AHBIDLE) {
            return 0;
        }
        spin_delay(32);
    }
    KLOG_ERROR("[DWC2] wait_ahb_idle timeout: GRSTCTL=0x%08x\n",
               dwc2_read32(DWC2_OFF_GRSTCTL));
    return -1;
}

/* ==========================================================================
 * 6. 软复位
 * ========================================================================== */

static int core_soft_reset(void)
{
    if (wait_ahb_idle() != 0)
        return -1;

    uint32_t snpsid = dwc2_read32(DWC2_OFF_GSNPSID);
    uint32_t core_rev = snpsid & DWC2_CORE_REV_MASK;
    bool new_rst_seq = core_rev >= (DWC2_CORE_REV_4_20A & DWC2_CORE_REV_MASK);

    /* 置位 CSFTRST */
    dwc2_write32(DWC2_OFF_GRSTCTL, GRSTCTL_CSFTRST);

    if (!new_rst_seq) {
        /* 旧版 IP：等 CSFTRST 自清 */
        for (uint32_t i = 0; i < 3000000; i++) {
            if (!(dwc2_read32(DWC2_OFF_GRSTCTL) & GRSTCTL_CSFTRST)) {
                spin_delay(4096);
                KLOG_DEBUG("[DWC2]   soft_reset: legacy path, %lu spins\n", (unsigned long)i);
                return 0;
            }
            spin_delay(32);
        }
        KLOG_ERROR("[DWC2] core_soft_reset CSFTRST timeout (legacy)\n");
        return -1;
    }

    /* Core >= 4.20a：等 CSFTRST_DONE，然后清 CSFTRST + 置 CSFTRST_DONE */
    for (uint32_t i = 0; i < 3000000; i++) {
        uint32_t grst = dwc2_read32(DWC2_OFF_GRSTCTL);
        if (grst & GRSTCTL_CSFTRST_DONE) {
            grst &= ~GRSTCTL_CSFTRST;
            grst |= GRSTCTL_CSFTRST_DONE;
            dwc2_write32(DWC2_OFF_GRSTCTL, grst);
            spin_delay(4096);
            KLOG_DEBUG("[DWC2]   soft_reset: 4.20a+ path, %lu spins, "
                       "CSFTRST_DONE cleared\n", (unsigned long)i);
            return 0;
        }
        spin_delay(32);
    }
    KLOG_ERROR("[DWC2] core_soft_reset CSFTRST_DONE timeout "
               "GRSTCTL=0x%08x\n", dwc2_read32(DWC2_OFF_GRSTCTL));
    return -1;
}

/* ==========================================================================
 * 7. Force Host 模式
 * ========================================================================== */

static int force_host_mode(void)
{
    uint32_t gusb = dwc2_read32(DWC2_OFF_GUSBCFG);
    gusb |= GUSBCFG_FORCEHOSTMODE;
    dwc2_write32(DWC2_OFF_GUSBCFG, gusb);
    spin_delay(100000);

    for (uint32_t i = 0; i < 500000; i++) {
        if (dwc2_read32(DWC2_OFF_GINTSTS) & GINTSTS_CURMODE_HOST) {
            return 0;
        }
        spin_delay(32);
    }
    KLOG_ERROR("[DWC2] CURMODE_HOST not set after FORCEHOSTMODE\n");
    return -1;
}

/* ==========================================================================
 * 8. CV182x / SG2002 主机路径
 * ========================================================================== */

/* CLKGEN 寄存器 */
#define CLKGEN_BASE           0x03002000
#define REG_CLK_EN_1          0x004   /* CLK_EN_1: USB clock gates */
#define REG_CLK_EN_2          0x008   /* CLK_EN_2: USB 33MHz gate */
#define REG_CLK_BYP_0         0x030   /* CLK_BYP_0: bypass control */

/* CLK_EN_1 bits 28-31: clk_axi4_usb / clk_apb_usb / clk_125m_usb / clk_33k_usb */
#define CLKEN1_USB_MASK       (0xFu << 28)
/* CLK_EN_2 bit 0: clk_12m_usb */
#define CLKEN2_USB_12M        (1u << 0)
/* CLK_BYP_0 bit17/18: must be 0 for fpll path */
#define CLK_BYP0_USB_MASK     ((1u << 17) | (1u << 18))

/* TOP 系统控制 */
#define TOP_BASE              0x03000000
#define TOP_USB_PHY_CTRL      0x48    /* USB PHY device/host mode */
#define TOP_DDR_ADDR_MODE     0xB4    /* eco register */
#define TOP_USB_CTRSTS        0x3000  /* USB controller reset/status */

/* FMUX / IOBLK / GPIO for VBUS */
#define FMUX_BASE             0x03001000
#define FMUX_USB_VBUS_DET     0xFC    /* pinmux for USB_VBUS_DET */
#define IOBLK_BASE            0x03001800
#define IOBLK_USB_VBUS_DET    0x020   /* IO block pad control */
#define GPIO1_BASE            0x03021000
#define GPIO1_DR              0x000   /* data register */
#define GPIO1_DDR             0x004   /* data direction register */
#define GPIO1_VBUS_PIN        6       /* GPIOB[6] drives VBUS */

/* RSTC 寄存器 */
#define RSTC_BASE             0x03003000
#define RSTC_SOFT_RSTN_0      0x000
#define RSTC_USB_RESET_BIT    (1u << 11)   /* REG_SOFT_RESET_X_USB, rstc.rs:72 */

/* CLKGEN/RSTC MMIO 访问（使用恒等映射 + KERNEL_VMA） */
static inline uint32_t clkgen_read32(uintptr_t off)
{
    uintptr_t pa = CLKGEN_BASE + off;
#if DEVICE_MMIO_NEEDS_VMA
    pa += KERNEL_VMA;
#endif
    return read32((void *)pa);
}

static inline void clkgen_write32(uintptr_t off, uint32_t val)
{
    uintptr_t pa = CLKGEN_BASE + off;
#if DEVICE_MMIO_NEEDS_VMA
    pa += KERNEL_VMA;
#endif
    write32(val, (void *)pa);
}

static inline uint32_t rstc_read32(uintptr_t off)
{
    uintptr_t pa = RSTC_BASE + off;
#if DEVICE_MMIO_NEEDS_VMA
    pa += KERNEL_VMA;
#endif
    return read32((void *)pa);
}

static inline void rstc_write32(uintptr_t off, uint32_t val)
{
    uintptr_t pa = RSTC_BASE + off;
#if DEVICE_MMIO_NEEDS_VMA
    pa += KERNEL_VMA;
#endif
    write32(val, (void *)pa);
}

static inline uint32_t top_read32(uintptr_t off)
{
    uintptr_t pa = TOP_BASE + off;
#if DEVICE_MMIO_NEEDS_VMA
    pa += KERNEL_VMA;
#endif
    return read32((void *)pa);
}

static inline void top_write32(uintptr_t off, uint32_t val)
{
    uintptr_t pa = TOP_BASE + off;
#if DEVICE_MMIO_NEEDS_VMA
    pa += KERNEL_VMA;
#endif
    write32(val, (void *)pa);
}

static inline void fmux_write32(uintptr_t off, uint32_t val)
{
    uintptr_t pa = FMUX_BASE + off;
#if DEVICE_MMIO_NEEDS_VMA
    pa += KERNEL_VMA;
#endif
    write32(val, (void *)pa);
}

static inline uint32_t ioblk_read32(uintptr_t off)
{
    uintptr_t pa = IOBLK_BASE + off;
#if DEVICE_MMIO_NEEDS_VMA
    pa += KERNEL_VMA;
#endif
    return read32((void *)pa);
}

static inline void ioblk_write32(uintptr_t off, uint32_t val)
{
    uintptr_t pa = IOBLK_BASE + off;
#if DEVICE_MMIO_NEEDS_VMA
    pa += KERNEL_VMA;
#endif
    write32(val, (void *)pa);
}

static inline uint32_t gpio1_read32(uintptr_t off)
{
    uintptr_t pa = GPIO1_BASE + off;
#if DEVICE_MMIO_NEEDS_VMA
    pa += KERNEL_VMA;
#endif
    return read32((void *)pa);
}

static inline void gpio1_write32(uintptr_t off, uint32_t val)
{
    uintptr_t pa = GPIO1_BASE + off;
#if DEVICE_MMIO_NEEDS_VMA
    pa += KERNEL_VMA;
#endif
    write32(val, (void *)pa);
}

/* 使能 USB 时钟并解除复位 */
static void cv182x_usb_clock_reset_init(void)
{
    uint32_t v;

    /* 1. CLK_EN_1(+0x004): 使能 USB 四路时钟 bit[31:28] */
    v = clkgen_read32(REG_CLK_EN_1);
    KLOG_INFO("[DWC2]   CLKGEN: CLK_EN_1=0x%08x (before)\n", v);
    clkgen_write32(REG_CLK_EN_1, v | CLKEN1_USB_MASK);

    /* 2. CLK_EN_2(+0x008): 使能 clk_12m_usb bit0 */
    v = clkgen_read32(REG_CLK_EN_2);
    KLOG_INFO("[DWC2]   CLKGEN: CLK_EN_2=0x%08x (before)\n", v);
    clkgen_write32(REG_CLK_EN_2, v | CLKEN2_USB_12M);

    /* 3. CLK_BYP_0(+0x030): 清除 USB bypass bit17/18 → 走 fpll */
    v = clkgen_read32(REG_CLK_BYP_0);
    if (v & CLK_BYP0_USB_MASK) {
        clkgen_write32(REG_CLK_BYP_0, v & ~CLK_BYP0_USB_MASK);
        KLOG_INFO("[DWC2]   CLK_BYP_0: cleared bits 17/18 for USB fpll path\n");
    }

    spin_delay(50000);

    /* 4. 解除 USB IP 软复位（SOFT_RSTN_0 bit 11，低有效，写 1 = 解除） */
    v = rstc_read32(RSTC_SOFT_RSTN_0);
    if (!(v & RSTC_USB_RESET_BIT)) {
        rstc_write32(RSTC_SOFT_RSTN_0, v | RSTC_USB_RESET_BIT);
        KLOG_INFO("[DWC2]   RSTC: deasserted USB reset (SOFT_RSTN_0 bit11)\n");
    } else {
        KLOG_INFO("[DWC2]   RSTC: USB reset already deasserted\n");
    }

    spin_delay(50000);
    KLOG_INFO("[DWC2]   USB clock & reset init done\n");
}

/* TOP PHY host bringup: reset toggle + device→host mode switch */
static void cv182x_usb_top_host_bringup(void)
{
    uint32_t v;

    /* Step 1: USB controller reset toggle via TOP+0x3000 bit11 */
    v = top_read32(TOP_USB_CTRSTS);
    top_write32(TOP_USB_CTRSTS, v & ~(1u << 11));
    spin_delay(50000);  /* ~50us */
    v = top_read32(TOP_USB_CTRSTS);
    top_write32(TOP_USB_CTRSTS, v | (1u << 11));
    spin_delay(50000);

    /* Step 2: USB PHY mode: device→host via TOP+0x48 */
    v = top_read32(TOP_USB_PHY_CTRL);
    /* First: set bits[7:6]=0b11 and bit0=1 (intermediate state) */
    v = (v & ~0xC0u) | 0xC0u | 0x01u;
    top_write32(TOP_USB_PHY_CTRL, v);
    spin_delay(100000);  /* ~1ms */
    /* Then: set bits[7:6]=0b01 and bit0=1 (host mode) */
    v = (v & ~0xC0u) | 0x40u | 0x01u;
    top_write32(TOP_USB_PHY_CTRL, v);
    spin_delay(100000);

    /* Step 3: ECO bit in TOP+0xB4 */
    v = top_read32(TOP_DDR_ADDR_MODE);
    top_write32(TOP_DDR_ADDR_MODE, v | 0x80u);

    KLOG_INFO("[DWC2]   TOP PHY host bringup done  USB_PHY_CTRL=0x%08x\n",
              top_read32(TOP_USB_PHY_CTRL));
}

/* Enable VBUS power: pinmux → IO block → GPIO1 pin6 output high */
static void cv182x_usb_vbus_enable(void)
{
    uint32_t v;

    /* FMUX: set USB_VBUS_DET pin to GPIO mode (value 3 = XGPIOB[6]) */
    fmux_write32(FMUX_USB_VBUS_DET, 3);

    /* IOBLK: set drive strength bits[7:5] = 0b111 */
    v = ioblk_read32(IOBLK_USB_VBUS_DET);
    ioblk_write32(IOBLK_USB_VBUS_DET, v | (7u << 5));

    /* GPIO1: pin6 direction = output */
    v = gpio1_read32(GPIO1_DDR);
    gpio1_write32(GPIO1_DDR, v | (1u << GPIO1_VBUS_PIN));

    /* GPIO1: pin6 level = high (VBUS on) */
    v = gpio1_read32(GPIO1_DR);
    gpio1_write32(GPIO1_DR, v | (1u << GPIO1_VBUS_PIN));

    /* Wait ~2s for VBUS to stabilize and device to power up */
    timer_delay_ms(2000);
    KLOG_INFO("[DWC2]   VBUS GPIO enabled (GPIO1 pin%d high), waited 2s\n",
              GPIO1_VBUS_PIN);
}

/* 8a. OTG Host Session Overrides (dr_mode=otg 时必须)
 *
 * 使用读-改-写（modify），保留硬件预设位。
 * GOTGCTL 包含只读状态位，不可直接写 0 覆盖。 */
static void cv182x_init_gotgctl(void)
{
    uint32_t val = dwc2_read32(DWC2_OFF_GOTGCTL);
    val |= GOTGCTL_DBNCE_FLTR_BYPASS
        |  GOTGCTL_AVALOEN
        |  GOTGCTL_AVALOVAL
        |  GOTGCTL_VBVALOEN
        |  GOTGCTL_VBVALOVAL;
    dwc2_write32(DWC2_OFF_GOTGCTL, val);
    spin_delay(200000);
}

/* 8b. GUSBCFG: UTMI 16-bit、HS 超时校准
 *
 * PHYIF16 设错是 chirp 失败的关键根因。
 * cv182x PHY 实测为 8-bit UTMI，需确保 PHYIF16=0。
 */
static void cv182x_init_gusbcfg(void)
{
    uint32_t ghwcfg4 = dwc2_read32(DWC2_OFF_GHWCFG4);
    uint32_t utmi_w = (ghwcfg4 >> GHWCFG4_UTMI_PHY_DATA_WIDTH_SHIFT)
                    & GHWCFG4_UTMI_PHY_DATA_WIDTH_MASK;
    /* 仅在 16-bit only 时 PHYIF16=1 */
    bool want_16bit = (utmi_w == 1);

    uint32_t gusb = dwc2_read32(DWC2_OFF_GUSBCFG);
    /* ULPI_UTMI_SEL 已由 POR 清零 (UTMI+)；补上 FORCEHOSTMODE + TOUTCAL */
    gusb |= GUSBCFG_FORCEHOSTMODE;
    gusb &= ~(GUSBCFG_TOUTCAL_MASK << GUSBCFG_TOUTCAL_SHIFT);
    gusb |= (7 << GUSBCFG_TOUTCAL_SHIFT);  /* TOUTCAL=7 */
    if (want_16bit) {
        gusb |= GUSBCFG_PHYIF16;
    } else {
        gusb &= ~GUSBCFG_PHYIF16;
    }
    dwc2_write32(DWC2_OFF_GUSBCFG, gusb);

    KLOG_DEBUG("[DWC2] GHWCFG4.UTMI_PHY_DATA_WIDTH=%lu => PHYIF16=%d\n",
               (unsigned long)utmi_w, want_16bit ? 1 : 0);
}

/* 8c. GAHBCFG: DMA 使能
 *
 * 内部 DMA 架构 (GHWCFG2.ARCH=2) 时必须开启 DMA_EN，
 * 否则 EP0 无法使用 HCDMA。
 */
static void cv182x_init_gahbcfg(void)
{
    uint32_t ghwcfg2 = dwc2_read32(DWC2_OFF_GHWCFG2);
    uint32_t arch = (ghwcfg2 >> GHWCFG2_ARCH_SHIFT) & GHWCFG2_ARCH_MASK;
    g_internal_dma = (arch == 2);

    uint32_t gahb = dwc2_read32(DWC2_OFF_GAHBCFG);
    gahb &= ~(GAHBCFG_HBSTLEN_MASK << GAHBCFG_HBSTLEN_SHIFT);
    gahb |= GAHBCFG_GLBL_INTR_EN
          | (GAHBCFG_HBSTLEN_INCR16 << GAHBCFG_HBSTLEN_SHIFT);
    if (g_internal_dma) {
        gahb |= GAHBCFG_DMA_EN;
    }
    dwc2_write32(DWC2_OFF_GAHBCFG, gahb);

    KLOG_DEBUG("[DWC2] GHWCFG2.ARCH=%lu => internal_dma=%d\n",
               (unsigned long)arch, g_internal_dma);
}

/* 8d. HCFG: HS 模式下不设 FSLSSUPP */
static void cv182x_init_hcfg(void)
{
    /* HS only: 清零 FSLSSUPP 和 FSLSPCLKSEL */
    dwc2_write32(DWC2_OFF_HCFG, 0);
}

/* 8e. FIFO 初始化
 *
 * 动态 FIFO：从 GHWCFG3 读取总深度，计算 RX/NPTX/PTX 分区。
 * 优先采用设备树常用值；超出总深度时收缩。
 */
static int cv182x_init_host_fifos(void)
{
    uint32_t ghwcfg2 = dwc2_read32(DWC2_OFF_GHWCFG2);
    uint32_t ghwcfg3 = dwc2_read32(DWC2_OFF_GHWCFG3);
    uint32_t ghwcfg4 = dwc2_read32(DWC2_OFF_GHWCFG4);

    uint32_t total_depth = (ghwcfg3 >> GHWCFG3_DFIFO_DEPTH_SHIFT)
                         & GHWCFG3_DFIFO_DEPTH_MASK;
    uint32_t hc = 1 + ((ghwcfg2 >> GHWCFG2_NUM_HOST_CHAN_SHIFT)
                     & GHWCFG2_NUM_HOST_CHAN_MASK);
    g_num_host_channels = hc;

    uint32_t rx   = 536;
    uint32_t nptx = 32;
    uint32_t ptx  = 768;

    if (rx + nptx + ptx > total_depth) {
        rx   = 516 + hc;
        nptx = 256;
        ptx  = 768;
    }
    if (rx + nptx + ptx > total_depth) {
        ptx = total_depth - rx - nptx;
    }

    uint32_t nptx_start = rx;
    uint32_t ptx_start  = rx + nptx;

    dwc2_write32(DWC2_OFF_GRXFSIZ,   rx);
    dwc2_write32(DWC2_OFF_GNPTXFSIZ, (nptx << 16) | nptx_start);
    dwc2_write32(DWC2_OFF_HPTXFSIZ,  (ptx << 16) | ptx_start);

    /* 硬件支持专用 FIFO + core >= 2.91a 时，写 GDFIFOCFG */
    uint32_t snpsid = dwc2_read32(DWC2_OFF_GSNPSID);
    bool ded_fifo = (ghwcfg4 & GHWCFG4_DED_FIFO_EN) != 0;
    if (ded_fifo && snpsid >= DWC2_CORE_REV_2_91A) {
        uint32_t epbase = rx + nptx + ptx;
        uint32_t gdfifo = dwc2_read32(DWC2_OFF_GDFIFOCFG);
        gdfifo &= ~(GDFIFOCFG_EPINFOBASE_MASK << GDFIFOCFG_EPINFOBASE_SHIFT);
        gdfifo |= (epbase & GDFIFOCFG_EPINFOBASE_MASK) << GDFIFOCFG_EPINFOBASE_SHIFT;
        dwc2_write32(DWC2_OFF_GDFIFOCFG, gdfifo);
    }

    KLOG_DEBUG("[DWC2] FIFO: total=%lu hc=%lu rx=%lu nptx=%lu ptx=%lu\n",
               (unsigned long)total_depth, (unsigned long)hc,
               (unsigned long)rx, (unsigned long)nptx, (unsigned long)ptx);
    return 0;
}

/* 8f. Flush 收发 FIFO */
static int cv182x_flush_tx_fifo_all(void)
{
    if (wait_ahb_idle() != 0)
        return -1;

    dwc2_write32(DWC2_OFF_GRSTCTL,
                 GRSTCTL_TXFFLSH | (GRSTCTL_TXFNUM_ALL << GRSTCTL_TXFNUM_SHIFT));

    /* 等 TXFFLSH 自清 */
    for (uint32_t i = 0; i < 3000000; i++) {
        if (!(dwc2_read32(DWC2_OFF_GRSTCTL) & GRSTCTL_TXFFLSH)) {
            spin_delay(2000);
            return 0;
        }
        spin_delay(8);
    }
    KLOG_ERROR("[DWC2] flush_tx_fifo timeout\n");
    return -1;
}

static int cv182x_flush_rx_fifo(void)
{
    if (wait_ahb_idle() != 0)
        return -1;

    dwc2_write32(DWC2_OFF_GRSTCTL, GRSTCTL_RXFFLSH);

    for (uint32_t i = 0; i < 3000000; i++) {
        if (!(dwc2_read32(DWC2_OFF_GRSTCTL) & GRSTCTL_RXFFLSH)) {
            spin_delay(2000);
            return 0;
        }
        spin_delay(8);
    }
    KLOG_ERROR("[DWC2] flush_rx_fifo timeout\n");
    return -1;
}

/* 8g. CV182x USB2 PHY: 清除 UTMI override，交由 DWC2 管理
 *
 * Host 模式下 DWC2 自行驱动 dp_pulldown / dm_pulldown 信号；
 * UTMI_OVERRIDE=1 会使 PHY 忽略 DWC2 UTMI 信号，干扰连接检测。
 */
static void cv182x_phy_host_clear_utmi_override(void)
{
    if (g_dwc2_phy_base == 0) {
        KLOG_WARN("[DWC2] PHY base not set, skipping UTMI override clear\n");
        return;
    }

    uint32_t old014 = dwc2_phy_read32(CV182X_PHY_REG014);
    uint32_t old000 = dwc2_phy_read32(CV182X_PHY_REG000);
    uint32_t old004 = dwc2_phy_read32(CV182X_PHY_REG004);

    /* 写 0 清除 UTMI_OVERRIDE，DWC2 自行管理 PHY 信号 */
    dwc2_phy_write32(CV182X_PHY_REG014, 0);
    spin_delay(200000);
    uint32_t now014 = dwc2_phy_read32(CV182X_PHY_REG014);

    KLOG_INFO("[DWC2]   PHY REG000=0x%08x REG004=0x%08x REG014: 0x%04x -> 0x%04x "
              "(UTMI_OVERRIDE=%d -> cleared)\n",
              old000, old004, old014 & 0xffff, now014 & 0xffff,
              (old014 & PHY_REG014_UTMI_OVERRIDE) ? 1 : 0);
}

/* ==========================================================================
 * 9. 端口电源控制
 * ========================================================================== */

/* HPRT0 安全读：mask 掉 W1C 位 */
static uint32_t hprt0_read_safe(void)
{
    return dwc2_read32(DWC2_OFF_HPRT0) & ~HPRT0_W1C_MASK;
}

static void port_power_on(void)
{
    uint32_t hprt = hprt0_read_safe();
    hprt |= HPRT0_PWR;
    dwc2_write32(DWC2_OFF_HPRT0, hprt);
}

/* ==========================================================================
 * 10. 主初始化流程
 * ========================================================================== */

int dwc2_usb_init(void)
{
    if (g_dwc2_base == 0) {
        KLOG_ERROR("[DWC2] base not set (call dwc2_usb_set_base_virt)\n");
        return -1;
    }

    if (g_dwc2_inited) {
        KLOG_WARN("[DWC2] already initialized\n");
        return 0;
    }

    KLOG_INFO("[DWC2] === USB Host Controller Init ===\n");
    KLOG_INFO("[DWC2] MMIO base: virt=0x%lx\n", (unsigned long)g_dwc2_base);
    g_dwc2_irq = platform_get_uint("usb", "irq");

    /* ── 0. USB 上电序列（时钟 → PHY → VBUS）必须在 probe 之前 ── */
    KLOG_INFO("[DWC2] [step 0] USB power-up: clocks + PHY + VBUS...\n");
    cv182x_usb_clock_reset_init();
    cv182x_usb_top_host_bringup();
    cv182x_usb_vbus_enable();
    KLOG_INFO("[DWC2] [step 0] USB power-up done\n");

    /* ── 1. 探测硬件 ─────────────────────────────────── */
    uint32_t ghwcfg1 = dwc2_read32(DWC2_OFF_GHWCFG1);
    uint32_t ghwcfg2 = dwc2_read32(DWC2_OFF_GHWCFG2);
    uint32_t ghwcfg3 = dwc2_read32(DWC2_OFF_GHWCFG3);
    uint32_t ghwcfg4 = dwc2_read32(DWC2_OFF_GHWCFG4);
    uint32_t snpsid  = dwc2_read32(DWC2_OFF_GSNPSID);

    if (ghwcfg2 == 0 && ghwcfg3 == 0) {
        KLOG_ERROR("[DWC2] GHWCFG2/3 zero — no controller?\n");
        return -1;
    }

    /* 解析硬件参数 */
    uint32_t core_rev   = snpsid & DWC2_CORE_REV_MASK;
    uint32_t hc         = 1 + ((ghwcfg2 >> GHWCFG2_NUM_HOST_CHAN_SHIFT)
                            & GHWCFG2_NUM_HOST_CHAN_MASK);
    uint32_t arch       = (ghwcfg2 >> GHWCFG2_ARCH_SHIFT)
                        & GHWCFG2_ARCH_MASK;
    uint32_t dfifo_depth = (ghwcfg3 >> GHWCFG3_DFIFO_DEPTH_SHIFT)
                         & GHWCFG3_DFIFO_DEPTH_MASK;
    uint32_t utmi_w     = (ghwcfg4 >> GHWCFG4_UTMI_PHY_DATA_WIDTH_SHIFT)
                        & GHWCFG4_UTMI_PHY_DATA_WIDTH_MASK;
    bool     ded_fifo   = (ghwcfg4 & GHWCFG4_DED_FIFO_EN) != 0;
    bool     internal_dma = (arch == 2);
    const char *utmi_str = (utmi_w == 0) ? "8-bit only" :
                           (utmi_w == 1) ? "16-bit only" :
                           (utmi_w == 2) ? "8/16-bit programmable" : "?";
    const char *arch_str = (arch == 0) ? "Slave-only" :
                           (arch == 1) ? "External DMA" :
                           (arch == 2) ? "Internal DMA" : "?";

    KLOG_INFO("[DWC2] [probe] Core: SNSPSID=0x%08x (rev=0x%04x, >=4.20a=%d)\n",
              snpsid, core_rev,
              (core_rev >= (DWC2_CORE_REV_4_20A & DWC2_CORE_REV_MASK)) ? 1 : 0);
    KLOG_INFO("[DWC2] [probe] Arch: %s  HostChannels: %lu  DFIFO-depth: %lu words\n",
              arch_str, (unsigned long)hc, (unsigned long)dfifo_depth);
    KLOG_INFO("[DWC2] [probe] UTMI: %s  Dedicated-FIFO: %d\n",
              utmi_str, ded_fifo ? 1 : 0);
    KLOG_INFO("[DWC2] [probe] GHWCFG1=%08x  GHWCFG2=%08x  GHWCFG3=%08x  GHWCFG4=%08x\n",
              ghwcfg1, ghwcfg2, ghwcfg3, ghwcfg4);

    /* ── 2. 关全局中断 ───────────────────────────────── */
    dwc2_write32(DWC2_OFF_GINTMSK, 0);
    dwc2_write32(DWC2_OFF_GINTSTS, 0xFFFFFFFF);
    KLOG_INFO("[DWC2] [step 1/8] Global interrupts masked\n");

    /* ── 3. 软复位 (第一轮) ───────────────────────────── */
    KLOG_INFO("[DWC2] [step 2/8] Core soft reset (1st)...\n");
    if (core_soft_reset() != 0) {
        KLOG_ERROR("[DWC2] [step 2/8] FAILED\n");
        return -1;
    }
    KLOG_INFO("[DWC2] [step 2/8] Core soft reset (1st) OK  GRSTCTL=0x%08x\n",
              dwc2_read32(DWC2_OFF_GRSTCTL));

    /* ── 4. Force Host 模式 ──────────────────────────── */
    KLOG_INFO("[DWC2] [step 3/8] Force Host mode...\n");
    if (force_host_mode() != 0) {
        KLOG_ERROR("[DWC2] [step 3/8] FAILED\n");
        return -1;
    }
    KLOG_INFO("[DWC2] [step 3/8] Force Host mode OK  GINTSTS.CURMODE_HOST=%d\n",
              (dwc2_read32(DWC2_OFF_GINTSTS) & GINTSTS_CURMODE_HOST) ? 1 : 0);

    /* ── 5. 软复位 (第二轮) ──────────────────────────── */
    KLOG_INFO("[DWC2] [step 4/8] Core soft reset (2nd)...\n");
    if (core_soft_reset() != 0) {
        KLOG_ERROR("[DWC2] [step 4/8] FAILED\n");
        return -1;
    }
    KLOG_INFO("[DWC2] [step 4/8] Core soft reset (2nd) OK\n");

    /* ── 6. CV182x/SG2002 特定配置 ─────────────────────── */
    KLOG_INFO("[DWC2] [step 6/8] CV182x platform config...\n");

    cv182x_init_gotgctl();
    KLOG_INFO("[DWC2]   GOTGCTL (session overrides) = 0x%08x\n",
              dwc2_read32(DWC2_OFF_GOTGCTL));

    cv182x_init_gusbcfg();
    KLOG_INFO("[DWC2]   GUSBCFG (UTMI cfg, PHYIF16=%d) = 0x%08x\n",
              (dwc2_read32(DWC2_OFF_GUSBCFG) & GUSBCFG_PHYIF16) ? 1 : 0,
              dwc2_read32(DWC2_OFF_GUSBCFG));

    dwc2_write32(DWC2_OFF_PCGCTL, 0);
    KLOG_INFO("[DWC2]   PCGCTL (power/clock gate) = 0x%08x\n",
              dwc2_read32(DWC2_OFF_PCGCTL));

    cv182x_init_gahbcfg();
    g_internal_dma = internal_dma;
    KLOG_INFO("[DWC2]   GAHBCFG (DMA_EN=%d) = 0x%08x\n",
              (dwc2_read32(DWC2_OFF_GAHBCFG) & GAHBCFG_DMA_EN) ? 1 : 0,
              dwc2_read32(DWC2_OFF_GAHBCFG));

    cv182x_init_hcfg();
    KLOG_INFO("[DWC2]   HCFG (FSLS only) = 0x%08x\n",
              dwc2_read32(DWC2_OFF_HCFG));

    KLOG_INFO("[DWC2] [step 6/9] CV182x platform config OK\n");

    /* ── 7. FIFO 分区 + Flush ──────────────────────────── */
    KLOG_INFO("[DWC2] [step 7/9] FIFO partition + flush...\n");
    if (cv182x_init_host_fifos() != 0) {
        KLOG_ERROR("[DWC2] [step 7/9] FIFO init FAILED\n");
        return -1;
    }

    uint32_t grxfsiz   = dwc2_read32(DWC2_OFF_GRXFSIZ);
    uint32_t gnptxfsiz = dwc2_read32(DWC2_OFF_GNPTXFSIZ);
    uint32_t hptxfsiz  = dwc2_read32(DWC2_OFF_HPTXFSIZ);
    uint32_t gdfifocfg = dwc2_read32(DWC2_OFF_GDFIFOCFG);
    KLOG_INFO("[DWC2]   GRXFSIZ=0x%04x (%lu)  GNPTXFSIZ=0x%08x  HPTXFSIZ=0x%08x\n",
              grxfsiz, (unsigned long)grxfsiz, gnptxfsiz, hptxfsiz);
    KLOG_INFO("[DWC2]   GDFIFOCFG=0x%08x\n", gdfifocfg);
    KLOG_INFO("[DWC2]   RX: start=0 size=%lu  NPTX: start=%lu size=%lu  PTX: start=%lu size=%lu\n",
              (unsigned long)(grxfsiz & 0xffff),
              (unsigned long)(gnptxfsiz & 0xffff),
              (unsigned long)((gnptxfsiz >> 16) & 0xffff),
              (unsigned long)(hptxfsiz & 0xffff),
              (unsigned long)((hptxfsiz >> 16) & 0xffff));

    if (cv182x_flush_tx_fifo_all() != 0) {
        KLOG_ERROR("[DWC2] [step 7/9] TX FIFO flush FAILED\n");
        return -1;
    }
    KLOG_INFO("[DWC2]   TX FIFO flush OK\n");

    if (cv182x_flush_rx_fifo() != 0) {
        KLOG_ERROR("[DWC2] [step 7/9] RX FIFO flush FAILED\n");
        return -1;
    }
    KLOG_INFO("[DWC2]   RX FIFO flush OK\n");
    KLOG_INFO("[DWC2] [step 7/9] FIFO partition + flush OK\n");

    /* ── 8. 中断使能 ──────────────────────────────────── */
    /*
     * HAINTMSK: only CH0 (control) generates PLIC interrupts.
     * CH1 (isoch) is polled directly — in buffer DMA mode the hardware
     * sets CHHLTD regardless of HCINTMSK, so no mask is needed for polling.
     */
    dwc2_write32(DWC2_OFF_HAINTMSK, (1u << 0));
    uint32_t hcintmsk = HCINT_XFERCOMPL | HCINT_CHHLTD | HCINT_AHBERR |
                        HCINT_STALL | HCINT_NAK | HCINT_XACTERR |
                        HCINT_BBLERR | HCINT_FRMOVRN | HCINT_DATATGLERR;
    hc_write32(0, HC_OFF_INTMSK, hcintmsk);
    hc_write32(1, HC_OFF_INTMSK, 0);
    uint32_t gintmsk = dwc2_read32(DWC2_OFF_GINTMSK);
    gintmsk |= GINTSTS_HCHINT;
    dwc2_write32(DWC2_OFF_GINTMSK, gintmsk);
    dwc2_write32(DWC2_OFF_GINTSTS, 0xFFFFFFFF);
    KLOG_INFO("[DWC2] [step 8/9] Channel ints enabled (HC0 IRQ, HC1 polling)  HAINTMSK=0x%04x  HC0MSK=0x%08x HC1MSK=0x%08x GAHBCFG=0x%08x GINTSTS=0x%08x GINTMSK=0x%08x\n",
              dwc2_read32(DWC2_OFF_HAINTMSK) & 0xffff,
              hc_read32(0, HC_OFF_INTMSK),
              hc_read32(1, HC_OFF_INTMSK),
              dwc2_read32(DWC2_OFF_GAHBCFG),
              dwc2_read32(DWC2_OFF_GINTSTS),
              dwc2_read32(DWC2_OFF_GINTMSK));

#if ARCH_RISCV64
    if (g_dwc2_irq != 0) {
        plic_init();
        plic_install(g_dwc2_irq, dwc2_usb_irq_handler, NULL);
        plic_enable_irq(g_dwc2_irq, 1);
        KLOG_INFO("[DWC2] PLIC IRQ registered irq=%u\n", g_dwc2_irq);
    } else {
        KLOG_WARN("[DWC2] usb.irq missing; controller IRQ line not enabled\n");
    }
#endif

    /* ── 9. 上电根端口 ─────────────────────────────────── */
    port_power_on();

    /* 等 CONNSTS 就绪（设备最多 100ms 发出连接信号）再打印 */
    {
        uint32_t hprt0;
        for (int t = 0; t < 10; t++) {
            timer_delay_ms(50);
            hprt0 = dwc2_read32(DWC2_OFF_HPRT0);
            if (hprt0 & HPRT0_CONNSTS)
                break;
        }
        KLOG_INFO("[DWC2] [step 9/9] Root port power ON  HPRT0=0x%08x "
                  "(PWR=%d CONNSTS=%d SPD=%d LNSTS=%d)\n",
                  hprt0,
                  (hprt0 & HPRT0_PWR) ? 1 : 0,
                  (hprt0 & HPRT0_CONNSTS) ? 1 : 0,
                  (int)((hprt0 >> HPRT0_SPD_SHIFT) & HPRT0_SPD_MASK),
                  (int)((hprt0 >> HPRT0_LNSTS_SHIFT) & HPRT0_LNSTS_MASK));
    }

    /* ── 10. PHY 配置 ─────────────────────────────────── */
    cv182x_phy_host_clear_utmi_override();
    cv182x_init_gotgctl();  /* 再写一次确保生效 */

    g_dwc2_inited = true;
    g_num_host_channels = hc;
    KLOG_INFO("[DWC2] === Init Complete: hc=%lu dma=%d dfifo=%lu words utmi=%s ===\n",
              (unsigned long)hc, g_internal_dma ? 1 : 0,
              (unsigned long)dfifo_depth, utmi_str);

    /* ── 连接检测轮询（后续展开枚举时启用） ──────────── */
#if 0
    KLOG_INFO("[DWC2] Waiting for device (CONNSTS) up to 2s...\n");
    {
        uint32_t hprt, gint;
        for (int t = 0; t < 40; t++) {
            timer_delay_ms(50);
            hprt = dwc2_read32(DWC2_OFF_HPRT0);
            if (hprt & HPRT0_CONNSTS) {
                KLOG_INFO("[DWC2] CONNSTS=1 after ~%dms "
                          "HPRT0=0x%08x SPD=%d LNSTS=%d\n",
                          t * 50, hprt,
                          (int)((hprt >> HPRT0_SPD_SHIFT) & HPRT0_SPD_MASK),
                          (int)((hprt >> HPRT0_LNSTS_SHIFT) & HPRT0_LNSTS_MASK));
                break;
            }
        }
    }
#endif
    return 0;
}

/* ==========================================================================
 * 11. 根端口操作
 * ========================================================================== */

bool dwc2_usb_device_connected(void)
{
    if (!g_dwc2_inited)
        return false;
    uint32_t hprt = dwc2_read32(DWC2_OFF_HPRT0);
    return (hprt & HPRT0_CONNSTS) != 0;
}

int dwc2_usb_reset_root_port(void)
{
    if (!g_dwc2_inited) {
        KLOG_ERROR("[DWC2] Not initialized\n");
        return -1;
    }

    uint32_t hprt = dwc2_read32(DWC2_OFF_HPRT0);

    /* 清 CONNDET (W1C) */
    if (hprt & HPRT0_CONNDET) {
        dwc2_write32(DWC2_OFF_HPRT0,
                     (hprt & ~HPRT0_W1C_MASK) | HPRT0_CONNDET);
    }

    /* ── 拉 PRTRST ~60ms ────────────────────────────── */
    uint32_t base = hprt0_read_safe() | HPRT0_PWR;
    dwc2_write32(DWC2_OFF_HPRT0, base | HPRT0_RST);
    timer_delay_ms(60);

    /* ── 解 PRTRST ───────────────────────────────────── */
    base = hprt0_read_safe() | HPRT0_PWR;
    dwc2_write32(DWC2_OFF_HPRT0, base & ~HPRT0_RST);

    /* 等 ~80ms 让 PHY chirp 完成 */
    timer_delay_ms(80);

    hprt = dwc2_read32(DWC2_OFF_HPRT0);
    uint32_t speed = (hprt >> HPRT0_SPD_SHIFT) & HPRT0_SPD_MASK;
    const char *speed_str = "unknown";
    if (speed == HPRT0_SPD_HS) speed_str = "HS";
    else if (speed == HPRT0_SPD_FS) speed_str = "FS";
    else if (speed == HPRT0_SPD_LS) speed_str = "LS";

    KLOG_INFO("[DWC2] Root port reset done: CONNSTS=%lu SPD=%s\n",
              (unsigned long)(hprt & 1), speed_str);
    return 0;
}

int dwc2_usb_get_root_speed(void)
{
    if (!g_dwc2_inited)
        return -1;
    uint32_t hprt = dwc2_read32(DWC2_OFF_HPRT0);
    return (int)((hprt >> HPRT0_SPD_SHIFT) & HPRT0_SPD_MASK);
}

uint32_t dwc2_usb_read_hprt0(void)
{
    if (!g_dwc2_inited)
        return 0;
    return dwc2_read32(DWC2_OFF_HPRT0);
}

/* ==========================================================================
 * 12. 调试：关键寄存器快照
 * ========================================================================== */

void dwc2_usb_dump_regs(void)
{
    if (g_dwc2_base == 0) {
        KLOG_INFO("[DWC2] base not set\n");
        return;
    }
    KLOG_INFO("[DWC2] --- reg dump ---\n");
    KLOG_INFO("  GOTGCTL =0x%08x  GAHBCFG =0x%08x\n",
              dwc2_read32(DWC2_OFF_GOTGCTL),
              dwc2_read32(DWC2_OFF_GAHBCFG));
    KLOG_INFO("  GUSBCFG =0x%08x  GRSTCTL =0x%08x\n",
              dwc2_read32(DWC2_OFF_GUSBCFG),
              dwc2_read32(DWC2_OFF_GRSTCTL));
    KLOG_INFO("  GINTSTS =0x%08x  GINTMSK =0x%08x\n",
              dwc2_read32(DWC2_OFF_GINTSTS),
              dwc2_read32(DWC2_OFF_GINTMSK));
    KLOG_INFO("  HPRT0   =0x%08x  HCFG    =0x%08x\n",
              dwc2_read32(DWC2_OFF_HPRT0),
              dwc2_read32(DWC2_OFF_HCFG));
    KLOG_INFO("  PCGCTL  =0x%08x  GSNPSID =0x%08x\n",
              dwc2_read32(DWC2_OFF_PCGCTL),
              dwc2_read32(DWC2_OFF_GSNPSID));
}
