/* driver/usb/dwc3.c
 *
 * DWC3 USB3 控制器探测 — RK3588
 *
 * 第一步：检测并打印 DWC3/xHCI 控制器版本信息。
 *
 * 初始化流程：
 *   1. 通过 platform_get_mmio("usb","base0/1") 获取 MMIO 虚拟地址
 *   2. 读取 xHCI 能力寄存器（HCIVERSION / HCSPARAMS1 / HCCPARAMS1）
 *   3. 读取 DWC3 全局寄存器（GSNPSID / GCTL / GHWPARAMS0）
 *   4. 打印控制器信息
 *
 * 框架集成：
 *   - 由 platform.lua 的 register_device("usb0") 在 drivers 阶段调用
 *   - 通过 Lua 绑定 dwc3.probe() 触发
 */

#include "usb/dwc3.h"
#include "mmio.h"
#include "klog.h"
#include "platform_cfg.h"
#include "mm_vm.h"      /* virt_to_phys() */
#include "string.h"     /* memset() */
#include "cache.h"

/* 运行时 MMIO 基地址（platform_get_mmio 在驱动 init 时填充） */
uintptr_t g_dwc3_base0 = 0;
uintptr_t g_dwc3_base1 = 0;

/* ── Orange Pi 5 Plus USB VBUS 5V 上电 ─────────────────────────────────────
 *
 * vcc5v0_host 调节器由 GPIO3_B7（GPIO3[15]）通过负载开关芯片控制，
 * ACTIVE_HIGH。必须在 xHCI 启动前拉高，否则 USB 端口无法向设备供电。
 *
 * DTS 来源：
 *   vcc5v0-host { gpio = <&gpio3 RK_PB7 GPIO_ACTIVE_HIGH>; }
 *   pinctrl: vcc5v0-host-en { rockchip,pins = <3 15 0 ...>; }
 *
 * 寄存器（RK3588 新式 GPIO：高 16 位 = 写使能掩码，低 16 位 = 数据值）：
 *   GPIO3 base:            0xFEC40000
 *   SYS_IOC base:          0xFD5F0000
 *   GPIO3B_IOMUX_H offset: 0x600C  （pins 12-15，每 pin 4 bit）
 *   SWPORT_DR_L  offset:   0x00    （data [15:0]）
 *   SWPORT_DDR_L offset:   0x08    （direction [15:0]）
 *
 * GPIO3[15] = bit 15；RK3588 mask-write 值 = (1<<31)|(1<<15) = 0x80008000
 * ─────────────────────────────────────────────────────────────────────────*/

#define RK3588_GPIO3_BASE        0xFEC40000UL
#define RK3588_SYS_IOC_BASE      0xFD5F0000UL
#define RK_GPIO_DR_L             0x00U    /* Data register [15:0] */
#define RK_GPIO_DDR_L            0x08U    /* Direction register [15:0] */
#define RK3588_IOC_GPIO3B_IMUX_H 0x600CU  /* GPIO3B_IOMUX_H (pins 12-15) */

/* GPIO3_B7 = GPIO3[15]，mask-write：bit15 enable + bit15 data = 0x80008000 */
#define GPIO3_B7_MASK_SET        ((1U << 31) | (1U << 15))

static void rk3588_vbus_enable(void)
{
    uintptr_t gpio3  = (uintptr_t)RK3588_GPIO3_BASE   + KERNEL_VMA;
    uintptr_t sysioc = (uintptr_t)RK3588_SYS_IOC_BASE + KERNEL_VMA;

    /* 1. IOMUX：GPIO3_B7 = function 0 (GPIO)
     *    GPIO3B_IOMUX_H bits[15:12]，写使能掩码在 bits[31:28] */
    write32((0xFU << 28) | (0x0U << 12),
            (void *)(sysioc + RK3588_IOC_GPIO3B_IMUX_H));

    /* 2. 方向：output */
    write32(GPIO3_B7_MASK_SET, (void *)(gpio3 + RK_GPIO_DDR_L));

    /* 3. 数据：HIGH（使能 VBUS 5V） */
    write32(GPIO3_B7_MASK_SET, (void *)(gpio3 + RK_GPIO_DR_L));

    /* 回读 DR_L 验证 bit15 已置高 */
    uint32_t dr_val = read32((void *)(gpio3 + RK_GPIO_DR_L));
    KLOG_INFO("dwc3: GPIO3_B7 HIGH — USB Host VBUS 5V enabled"
              "  (DR_L=0x%08x bit15=%u)\n",
              dr_val, (dr_val >> 15) & 1U);
}

/* ── 内联寄存器访问 ──────────────────────────────────────────────────── */

static inline uint32_t dwc3_read32(uintptr_t base, uint32_t off)
{
    return read32((void *)(base + (uintptr_t)off));
}

/* ── 单控制器探测 ────────────────────────────────────────────────────── */

static void dwc3_probe_one(uintptr_t base, int id)
{
    /* xHCI 能力寄存器 */
    uint32_t cap_ver    = dwc3_read32(base, XHCI_CAP_VER);
    uint8_t  caplength  = (uint8_t)XHCI_CAPLENGTH(cap_ver);
    uint16_t hciversion = (uint16_t)XHCI_HCIVERSION(cap_ver);

    uint32_t hcsparams1 = dwc3_read32(base, XHCI_HCSPARAMS1);
    uint8_t  maxslots   = (uint8_t)XHCI_MAXSLOTS(hcsparams1);
    uint8_t  maxports   = (uint8_t)XHCI_MAXPORTS(hcsparams1);
    uint16_t maxintrs   = (uint16_t)XHCI_MAXINTRS(hcsparams1);

    uint32_t hccparams1 = dwc3_read32(base, XHCI_HCCPARAMS1);
    uint8_t  ac64       = (uint8_t)XHCI_AC64(hccparams1);
    uint8_t  csz        = (uint8_t)XHCI_CSZ(hccparams1);

    /* DWC3 全局寄存器 */
    uint32_t gsnpsid  = dwc3_read32(base, DWC3_GSNPSID);
    uint32_t gctl     = dwc3_read32(base, DWC3_GCTL);
    uint32_t ghwp0    = dwc3_read32(base, DWC3_GHWPARAMS0);
    uint32_t ghwp1    = dwc3_read32(base, DWC3_GHWPARAMS1);

    KLOG_INFO("dwc3: ─── USB3 OTG%d @ 0x%08lx ───\n",
              id, (unsigned long)base);

    /* xHCI 信息 */
    KLOG_INFO("dwc3:   xHCI %x.%02x  caplength=%u  MaxSlots=%u  MaxPorts=%u  MaxIntrs=%u\n",
              (hciversion >> 8) & 0xFF, hciversion & 0xFF,
              caplength, maxslots, maxports, maxintrs);
    KLOG_INFO("dwc3:   HCCPARAMS1=0x%08x  AC64=%u  CSZ=%u\n",
              hccparams1, ac64, csz);

    /* DWC3 版本 */
    if ((gsnpsid & DWC3_GSNPSID_MASK) == DWC3_GSNPSID_MAGIC) {
        uint32_t sub = DWC3_GSNPSID_SUB(gsnpsid);
        char sub_c = (sub >= 0xaU) ? (char)('a' + (int)(sub - 0xaU)) :
                                     (char)('0' + (int)sub);
        KLOG_INFO("dwc3:   GSNPSID=0x%08x  => DWC3 v%u.%02x%c\n",
                  gsnpsid,
                  DWC3_GSNPSID_MAJOR(gsnpsid),
                  DWC3_GSNPSID_MINOR(gsnpsid),
                  sub_c);
    } else {
        KLOG_INFO("dwc3:   GSNPSID=0x%08x  => unknown IP\n", gsnpsid);
    }

    /* 当前工作模式 */
    uint8_t prtcap = (uint8_t)DWC3_GCTL_PRTCAPDIR(gctl);
    const char *mode_str =
        (prtcap == DWC3_GCTL_PRTCAP_HOST)   ? "Host"   :
        (prtcap == DWC3_GCTL_PRTCAP_DEVICE) ? "Device" :
        (prtcap == DWC3_GCTL_PRTCAP_OTG)    ? "OTG"    : "Unknown";

    KLOG_INFO("dwc3:   GCTL=0x%08x  mode=%s\n", gctl, mode_str);
    KLOG_INFO("dwc3:   GHWPARAMS0=0x%08x  GHWPARAMS1=0x%08x\n", ghwp0, ghwp1);
}

/* ── 主探测入口 ─────────────────────────────────────────────────────── */

void dwc3_probe(void)
{
    KLOG_INFO("dwc3: probing RK3588 DWC3/xHCI USB controllers\n");

    /* 从平台配置读取 MMIO 地址（mmio_vma=true，自动加 KERNEL_VMA 偏移）*/
    g_dwc3_base0 = platform_get_mmio("usb", "base0");
    g_dwc3_base1 = platform_get_mmio("usb", "base1");

    if (g_dwc3_base0 != 0) {
        dwc3_probe_one(g_dwc3_base0, 0);
    } else {
        KLOG_WARN("dwc3: usb.base0 not configured\n");
    }

    if (g_dwc3_base1 != 0) {
        dwc3_probe_one(g_dwc3_base1, 1);
    } else {
        KLOG_WARN("dwc3: usb.base1 not configured\n");
    }

    KLOG_INFO("dwc3: probe complete\n");
}

/* ── 简单忙等（不依赖 OS timer）────────────────────────────────────────── */

static void dwc3_poll_delay(volatile uint32_t n)
{
    while (n--) {
        __asm__ volatile("" ::: "memory");
    }
}

/* ── xHCI 读写（相对于 operational base = dwc3_base + CAPLENGTH）──────── */

static inline uint32_t xhci_op_read(uintptr_t op_base, uint32_t off)
{
    return read32((void *)(op_base + off));
}

static inline void xhci_op_write(uintptr_t op_base, uint32_t off, uint32_t val)
{
    write32(val, (void *)(op_base + off));
}

/* ── 单控制器 Host 模式初始化 ────────────────────────────────────────── */

static int dwc3_host_init_one(uintptr_t base, int id)
{
    /* 1. 取 caplength，计算 operational base */
    uint32_t cap_ver   = dwc3_read32(base, XHCI_CAP_VER);
    uint8_t  caplength = (uint8_t)XHCI_CAPLENGTH(cap_ver);
    uintptr_t op_base  = base + caplength;

    KLOG_INFO("dwc3: [OTG%d] host init  caplength=%u  op_base=0x%08lx\n",
              id, (unsigned)caplength, (unsigned long)op_base);

    /* 2. 关掉 PHY suspend，设置 USBTRDTIM=9（8-bit UTMI HS 必需）*/
    uint32_t phycfg = dwc3_read32(base, DWC3_GUSB2PHYCFG0);
    phycfg &= ~DWC3_GUSB2PHYCFG_SUSPHY;
    phycfg &= ~DWC3_GUSB2PHYCFG_USBTRDTIM_MASK;
    phycfg |=  DWC3_GUSB2PHYCFG_USBTRDTIM(9U);  /* 8-bit UTMI → 9 */
    write32(phycfg, (void *)(base + DWC3_GUSB2PHYCFG0));
    KLOG_INFO("dwc3: [OTG%d] GUSB2PHYCFG0=0x%08x\n", id,
              (unsigned)dwc3_read32(base, DWC3_GUSB2PHYCFG0));

    uint32_t pipectl = dwc3_read32(base, DWC3_GUSB3PIPECTL0);
    pipectl &= ~DWC3_GUSB3PIPECTL_SUSPEN;
    write32(pipectl, (void *)(base + DWC3_GUSB3PIPECTL0));

    /* 3. DWC3 核心软复位
     * 注意：CoreSoftReset 不自清零，必须手动清除。
     * RK3588 完整 PHY 复位需要 GRF/CRU 配合；
     * 这里仅触发并等待固定时间后清除。*/
    uint32_t gctl = dwc3_read32(base, DWC3_GCTL);
    gctl |= DWC3_GCTL_CORESOFTRESET;
    write32(gctl, (void *)(base + DWC3_GCTL));

    /* 等待 ~100 µs（databook: 至少 100 ns）*/
    dwc3_poll_delay(500000U);

    /* 读 GSTS.CSRTIMEOUT：若置位说明 PHY 未响应（可接受，继续） */
    uint32_t gsts = dwc3_read32(base, DWC3_GSTS);
    if (gsts & DWC3_GSTS_CSRTIMEOUT)
        KLOG_WARN("dwc3: [OTG%d] PHY did not ack reset (CSRTIMEOUT);"
                  " continuing without full PHY reset\n", id);

    /* 手动清除 CoreSoftReset */
    gctl = dwc3_read32(base, DWC3_GCTL);
    gctl &= ~DWC3_GCTL_CORESOFTRESET;
    write32(gctl, (void *)(base + DWC3_GCTL));
    dwc3_poll_delay(200000U);

    KLOG_INFO("dwc3: [OTG%d] core soft reset done  GSTS=0x%08x\n", id, gsts);

    /* 4. 切换到 Host 模式（PRTCAPDIR = 01） */
    gctl = dwc3_read32(base, DWC3_GCTL);
    gctl &= ~DWC3_GCTL_PRTCAP_MASK;
    gctl |= (DWC3_GCTL_PRTCAP_HOST << DWC3_GCTL_PRTCAP_SHIFT);
    write32(gctl, (void *)(base + DWC3_GCTL));

    dwc3_poll_delay(200000U);

    gctl = dwc3_read32(base, DWC3_GCTL);
    KLOG_INFO("dwc3: [OTG%d] GCTL after host mode set: 0x%08x  mode=%s\n",
              id, gctl,
              (DWC3_GCTL_PRTCAPDIR(gctl) == DWC3_GCTL_PRTCAP_HOST) ?
              "Host" : "?");

    /* 4b. DWC3 v3.00a host 模式必要配置 GUCTL */
    uint32_t guctl = dwc3_read32(base, DWC3_GUCTL);
    guctl |= DWC3_GUCTL_USBHSTINAUTORETRYEN;  /* Host IN 自动重试 */
    write32(guctl, (void *)(base + DWC3_GUCTL));
    KLOG_INFO("dwc3: [OTG%d] GUCTL=0x%08x\n", id,
              (unsigned)dwc3_read32(base, DWC3_GUCTL));

    /* 5. xHCI Host Controller Reset (USBCMD.HCRST = 1) */
    uint32_t usbcmd = xhci_op_read(op_base, XHCI_OP_USBCMD);
    usbcmd |= XHCI_USBCMD_HCRST;
    xhci_op_write(op_base, XHCI_OP_USBCMD, usbcmd);

    /* 等待 HCRST 清零 + CNR 清零（最多 ~500 ms） */
    uint32_t timeout = 5000000U;
    while (timeout--) {
        usbcmd = xhci_op_read(op_base, XHCI_OP_USBCMD);
        uint32_t usbsts_r = xhci_op_read(op_base, XHCI_OP_USBSTS);
        if (!(usbcmd & XHCI_USBCMD_HCRST) && !(usbsts_r & XHCI_USBSTS_CNR))
            break;
        dwc3_poll_delay(10);
    }

    uint32_t usbsts = xhci_op_read(op_base, XHCI_OP_USBSTS);
    if ((xhci_op_read(op_base, XHCI_OP_USBCMD) & XHCI_USBCMD_HCRST) ||
        (usbsts & XHCI_USBSTS_CNR)) {
        KLOG_ERROR("dwc3: [OTG%d] xHCI reset timed out! USBSTS=0x%08x\n",
                   id, usbsts);
        return -1;
    }

    KLOG_INFO("dwc3: [OTG%d] xHCI reset done  USBSTS=0x%08x  %s\n",
              id, usbsts,
              (usbsts & XHCI_USBSTS_HCH) ? "halted" : "running");

    /* 6. 读端口状态（MaxPorts 来自 HCSPARAMS1） */
    uint32_t hcsparams1 = dwc3_read32(base, XHCI_HCSPARAMS1);
    uint8_t  maxports   = (uint8_t)XHCI_MAXPORTS(hcsparams1);

    for (uint8_t p = 0; p < maxports; p++) {
        uint32_t portsc = xhci_op_read(op_base, XHCI_OP_PORTSC(p));
        uint8_t  speed  = (uint8_t)XHCI_PORTSC_SPEED(portsc);
        const char *speed_str =
            (speed == 1) ? "FullSpeed"  :
            (speed == 2) ? "LowSpeed"   :
            (speed == 3) ? "HighSpeed"  :
            (speed == 4) ? "SuperSpeed" : "---";

        KLOG_INFO("dwc3: [OTG%d] port%u  PORTSC=0x%08x  %s  PP=%u  CCS=%u\n",
                  id, (unsigned)p + 1U, portsc,
                  speed_str,
                  (portsc & XHCI_PORTSC_PP)  ? 1U : 0U,
                  (portsc & XHCI_PORTSC_CCS) ? 1U : 0U);
    }

    return 0;
}

/* ── dwc3_host_init ──────────────────────────────────────────────────── */

void dwc3_host_init(void)
{
    KLOG_INFO("dwc3: === host mode init ===\n");

    if (g_dwc3_base0 == 0 && g_dwc3_base1 == 0) {
        KLOG_WARN("dwc3: host_init called before probe\n");
        return;
    }

    if (g_dwc3_base0 != 0)
        dwc3_host_init_one(g_dwc3_base0, 0);
    if (g_dwc3_base1 != 0)
        dwc3_host_init_one(g_dwc3_base1, 1);

    KLOG_INFO("dwc3: === host init complete ===\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * xHCI 启动 + 设备扫描（数据结构类型定义见 dwc3.h）
 * ═══════════════════════════════════════════════════════════════════════════ */

xhci_hw_t g_xhci_hw[2];   /* [0]=OTG0  [1]=OTG1 */

/* Scratchpad buffers: HCSPARAMS2.MaxScratchpad=32 requires these before R/S=1 */
uint64_t g_scratch_array[2][XHCI_MAX_SCRATCH] __attribute__((aligned(64)));
uint8_t  g_scratch_pages[2][XHCI_MAX_SCRATCH][4096] __attribute__((aligned(4096)));

/* 获取 32 位物理地址（AC64=0，确保在低 4 GB 内） */
static inline uint32_t xhci_phys32(const void *va)
{
    return (uint32_t)(virt_to_phys(va) & 0xFFFFFFFFUL);
}

/* Runtime 寄存器读写（相对于 runtime_base） */
static inline uint32_t xhci_rt_read(uintptr_t rt_base, uint32_t off)
{
    return read32((void *)(rt_base + off));
}
static inline void xhci_rt_write(uintptr_t rt_base, uint32_t off, uint32_t val)
{
    write32(val, (void *)(rt_base + off));
}

/* ── 打印端口状态 ──────────────────────────────────────────────────────── */

static void xhci_print_ports(uintptr_t base, uintptr_t op_base, int id)
{
    uint32_t hcsparams1 = dwc3_read32(base, XHCI_HCSPARAMS1);
    uint8_t  maxports   = (uint8_t)XHCI_MAXPORTS(hcsparams1);

    for (uint8_t p = 0; p < maxports; p++) {
        uint32_t portsc = xhci_op_read(op_base, XHCI_OP_PORTSC(p));
        uint8_t  speed  = (uint8_t)XHCI_PORTSC_SPEED(portsc);
        uint8_t  pls    = (uint8_t)XHCI_PORTSC_PLS(portsc);
        uint8_t  ccs    = (portsc & XHCI_PORTSC_CCS) ? 1U : 0U;

        static const char *speed_str[] = {
            "---", "FullSpeed", "LowSpeed", "HighSpeed",
            "SuperSpeed", "SS+", "?6", "?7"
        };
        static const char *pls_str[] = {
            "U0", "U1", "U2", "U3",
            "Disabled", "RxDetect", "Inactive", "Polling",
            "Recovery", "HotReset", "CompMode", "TestMode",
            "?C", "?D", "?E", "Resume"
        };

        if (ccs) {
            KLOG_INFO("dwc3: [OTG%d] port%u  *** DEVICE CONNECTED ***"
                      "  speed=%s  PLS=%s  PORTSC=0x%08x\n",
                      id, (unsigned)p + 1U,
                      (speed < 8U) ? speed_str[speed] : "?",
                      (pls  < 16U) ? pls_str[pls]    : "?",
                      portsc);
        } else {
            KLOG_INFO("dwc3: [OTG%d] port%u  no device"
                      "  PLS=%s  PORTSC=0x%08x\n",
                      id, (unsigned)p + 1U,
                      (pls < 16U) ? pls_str[pls] : "?",
                      portsc);
        }
    }
}

/* ── 单控制器 xHCI 启动 ───────────────────────────────────────────────── */

static void xhci_start_one(uintptr_t base, xhci_hw_t *hw, int id)
{
    /* 计算各寄存器区基地址 */
    uint32_t cap_ver  = dwc3_read32(base, XHCI_CAP_VER);
    uint8_t  caplen   = (uint8_t)XHCI_CAPLENGTH(cap_ver);
    uintptr_t op_base = base + caplen;

    uint32_t rtsoff   = read32((void *)(base + XHCI_RTSOFF)) & ~0x1FU;
    uintptr_t rt_base = base + (uintptr_t)rtsoff;
    uintptr_t ir0     = rt_base + XHCI_IR0_OFF;  /* Interrupter 0 */

    KLOG_INFO("dwc3: [OTG%d] xhci_start  rt_base=0x%08lx\n",
              id, (unsigned long)rt_base);

    /* ── 1. 清零所有数据结构 ─── */
    memset(hw, 0, sizeof(*hw));

    /* ── 2. DCBAA ─── */
    uint32_t dcbaa_phys = xhci_phys32(hw->dcbaa);
    xhci_op_write(op_base, XHCI_OP_DCBAAP_LO, dcbaa_phys);
    xhci_op_write(op_base, XHCI_OP_DCBAAP_HI, 0U);

    /* ── 3. Command Ring（末尾 Link TRB 回绕到起点）─── */
    uint32_t cmd_phys = xhci_phys32(hw->cmd_ring);
    /* Link TRB: 最后一个 TRB，type=Link, TC=1 */
    hw->cmd_ring[XHCI_CMD_RING_TRBS - 1U].param_lo = cmd_phys;
    hw->cmd_ring[XHCI_CMD_RING_TRBS - 1U].param_hi = 0U;
    hw->cmd_ring[XHCI_CMD_RING_TRBS - 1U].control  =
        XHCI_TRB_TYPE_LINK | XHCI_TRB_LINK_TC;

    /* CRCR: 物理地址 | RCS=1 */
    xhci_op_write(op_base, XHCI_OP_CRCR_LO, cmd_phys | XHCI_CRCR_RCS);
    xhci_op_write(op_base, XHCI_OP_CRCR_HI, 0U);

    /* ── 4. Event Ring Segment Table ─── */
    uint32_t evt_phys  = xhci_phys32(hw->evt_ring);
    uint32_t erst_phys = xhci_phys32(hw->erst);

    hw->erst[0].addr_lo  = evt_phys;
    hw->erst[0].addr_hi  = 0U;
    hw->erst[0].seg_size = XHCI_EVT_RING_TRBS;
    hw->erst[0].rsvd     = 0U;

    /* Interrupter 0: ERSTSZ=1, ERSTBA, ERDP 指向段起始 */
    xhci_rt_write(ir0, XHCI_IR_ERSTSZ,    1U);
    xhci_rt_write(ir0, XHCI_IR_ERSTBA_LO, erst_phys);
    xhci_rt_write(ir0, XHCI_IR_ERSTBA_HI, 0U);
    xhci_rt_write(ir0, XHCI_IR_ERDP_LO,   evt_phys);  /* 初始 dequeue = 段起始 */
    xhci_rt_write(ir0, XHCI_IR_ERDP_HI,   0U);

    /* 刷新 DMA 可访问结构到 DRAM */
    clean_dcache_range(hw->cmd_ring, sizeof(xhci_trb_t) * XHCI_CMD_RING_TRBS);
    clean_dcache_range(hw->evt_ring, sizeof(xhci_trb_t) * XHCI_EVT_RING_TRBS);
    clean_dcache_range(hw->erst,     sizeof(hw->erst[0]));
    clean_dcache_range(hw->dcbaa,    sizeof(hw->dcbaa));

    /* 使能 Interrupter 0：清除 IP，设置 IE=1（DWC3 需要才会写事件到环）*/
    xhci_rt_write(ir0, XHCI_IR_IMAN, 0x3U);  /* IP[1]=1(clear), IE[0]=1 */

    /* ── 5. MaxSlotsEn（打印 HCSPARAMS2，保持 MaxSlotsEn=1）─── */
    uint32_t hcsparams1 = read32((void *)(base + XHCI_HCSPARAMS1));
    uint32_t hcsparams2 = read32((void *)(base + XHCI_HCSPARAMS2));
    uint32_t max_slots  = XHCI_MAXSLOTS(hcsparams1);
    /* HCSPARAMS2 Max_Scratchpad_Bufs: bits[31:27]=Hi, bits[25:21]=Lo */
    uint32_t sp_hi = (hcsparams2 >> 27) & 0x1FU;
    uint32_t sp_lo = (hcsparams2 >> 21) & 0x1FU;
    uint32_t max_sp = (sp_hi << 5) | sp_lo;
    KLOG_INFO("dwc3: [OTG%d] HCSPARAMS1=0x%08x MaxSlots=%u  HCSPARAMS2=0x%08x MaxScratchpad=%u\n",
              id, (unsigned)hcsparams1, (unsigned)max_slots,
              (unsigned)hcsparams2, (unsigned)max_sp);
    /* MaxSlotsEn=1 (只枚举一个设备) */
    xhci_op_write(op_base, XHCI_OP_CONFIG, 1U);

    /* ── 5b. Scratchpad Buffers（xHCI spec §4.20，必须在 R/S=1 之前完成）─── */
    if (max_sp > 0U) {
        uint32_t n = (max_sp > XHCI_MAX_SCRATCH) ? XHCI_MAX_SCRATCH : max_sp;
        uint64_t *sa = g_scratch_array[id];
        for (uint32_t i = 0; i < n; i++) {
            sa[i] = (uint64_t)xhci_phys32(&g_scratch_pages[id][i][0]);
        }
        /* 刷新 scratchpad 页和指针数组 */
        clean_dcache_range(sa, sizeof(uint64_t) * n);
        clean_dcache_range(g_scratch_pages[id], sizeof(g_scratch_pages[id]));
        /* DCBAA[0] = 指针数组物理地址 */
        hw->dcbaa[0] = (uint64_t)xhci_phys32(sa);
        clean_dcache_range(hw->dcbaa, sizeof(hw->dcbaa));
        KLOG_INFO("dwc3: [OTG%d] scratchpad: %u bufs  array_phys=0x%08x\n",
                  id, n, xhci_phys32(sa));
    }

    /* ── 6. 启动：USBCMD.RS = 1 ─── */
    uint32_t usbcmd = xhci_op_read(op_base, XHCI_OP_USBCMD);
    usbcmd |= XHCI_USBCMD_RS;
    xhci_op_write(op_base, XHCI_OP_USBCMD, usbcmd);

    /* 等待 HCHalted 清零（最多 ~500 ms）*/
    uint32_t timeout = 5000000U;
    while (timeout--) {
        if (!(xhci_op_read(op_base, XHCI_OP_USBSTS) & XHCI_USBSTS_HCH))
            break;
        dwc3_poll_delay(10);
    }

    uint32_t usbsts = xhci_op_read(op_base, XHCI_OP_USBSTS);
    if (usbsts & XHCI_USBSTS_HCH) {
        KLOG_ERROR("dwc3: [OTG%d] xHCI failed to start! USBSTS=0x%08x\n",
                   id, usbsts);
        return;
    }

    KLOG_INFO("dwc3: [OTG%d] xHCI running  USBSTS=0x%08x\n", id, usbsts);

    /* ── 7. 等待端口稳定后读连接状态 ─── */
    dwc3_poll_delay(30000000U);   /* ~90ms：等待 HS 握手 + reset 完成 */
    xhci_print_ports(base, op_base, id);
}

/* ── dwc3_xhci_start ─────────────────────────────────────────────────── */

void dwc3_xhci_start(void)
{
    KLOG_INFO("dwc3: === xHCI start (device scan) ===\n");

    if (g_dwc3_base0 == 0 && g_dwc3_base1 == 0) {
        KLOG_WARN("dwc3: xhci_start called before probe\n");
        return;
    }

    /* 上电 USB Host VBUS 5V（GPIO3_B7，Orange Pi 5 Plus）。
     * 必须先于 xHCI 启动；USB 规范要求 VBUS 稳定 ≥100ms 后设备才能枚举。*/
    rk3588_vbus_enable();
    dwc3_poll_delay(100000000U);   /* ~300ms：等待 VBUS 稳定 + 设备上电 */

    if (g_dwc3_base0 != 0)
        xhci_start_one(g_dwc3_base0, &g_xhci_hw[0], 0);
    if (g_dwc3_base1 != 0)
        xhci_start_one(g_dwc3_base1, &g_xhci_hw[1], 1);

    KLOG_INFO("dwc3: === xHCI start complete ===\n");
}
