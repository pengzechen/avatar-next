/*
 * driver/usb/dwc2_regs.h — Synopsys DesignWare USB 2.0 OTG (DWC2) 寄存器定义
 *
 * 翻译自 sg200x-bsp/src/usb/host/dwc2/regs.rs，对齐 Linux drivers/usb/dwc2/hw.h。
 * 本文件为纯头文件，所有宏为常量表达式，可在编译期求值。
 *
 * 寄存器布局概述：
 *   - 全局寄存器：0x000–0x05c
 *   - 主机寄存器：0x100–0x444
 *   - 主机通道：  0x500–0x6ff (16 通道 × 0x20)
 *   - 设备寄存器：0x800–0xbff
 *   - PCGCTL：    0xe00
 */
#ifndef __DWC2_REGS_H__
#define __DWC2_REGS_H__

#include "types.h"

/* ==========================================================================
 * 1. 寄存器偏移量
 * ========================================================================== */

/* ── 全局寄存器 ───────────────────────────────────────────────────────── */

#define DWC2_OFF_GOTGCTL  0x000
#define DWC2_OFF_GOTGINT  0x004
#define DWC2_OFF_GAHBCFG  0x008
#define DWC2_OFF_GUSBCFG  0x00c
#define DWC2_OFF_GRSTCTL  0x010
#define DWC2_OFF_GINTSTS  0x014
#define DWC2_OFF_GINTMSK  0x018
#define DWC2_OFF_GRXSTSR  0x01c
#define DWC2_OFF_GRXSTSP  0x020
#define DWC2_OFF_GRXFSIZ  0x024
#define DWC2_OFF_GNPTXFSIZ 0x028
#define DWC2_OFF_GSNPSID  0x040
#define DWC2_OFF_GHWCFG1  0x044
#define DWC2_OFF_GHWCFG2  0x048
#define DWC2_OFF_GHWCFG3  0x04c
#define DWC2_OFF_GHWCFG4  0x050
#define DWC2_OFF_GDFIFOCFG 0x05c

/* ── 主机寄存器 ───────────────────────────────────────────────────────── */

#define DWC2_OFF_HPTXFSIZ 0x100
#define DWC2_OFF_HCFG     0x400
#define DWC2_OFF_HFIR     0x404
#define DWC2_OFF_HFNUM    0x408
#define DWC2_OFF_HAINT    0x414
#define DWC2_OFF_HAINTMSK 0x418
#define DWC2_OFF_HPRT0    0x440

/* 主机通道基址 (每个通道 0x20 字节) */
#define DWC2_OFF_HC_BASE  0x500
#define DWC2_OFF_HC_STRIDE 0x20
#define DWC2_MAX_HOST_CHANNELS 16

/* 单通道寄存器偏移 (相对于通道基址) */
#define HC_OFF_CHAR   0x00
#define HC_OFF_SPLT   0x04
#define HC_OFF_INT    0x08
#define HC_OFF_INTMSK 0x0c
#define HC_OFF_TSIZ   0x10
#define HC_OFF_DMA    0x14
#define HC_OFF_DMAB   0x1c

/* ── 设备寄存器 ───────────────────────────────────────────────────────── */

#define DWC2_OFF_DCFG     0x800
#define DWC2_OFF_DCTL     0x804
#define DWC2_OFF_DSTS     0x808
#define DWC2_OFF_DIEPMSK  0x810
#define DWC2_OFF_DOEPMSK  0x814
#define DWC2_OFF_DAINT    0x818
#define DWC2_OFF_DAINTMSK 0x81c
#define DWC2_OFF_DIEP_BASE 0x900
#define DWC2_OFF_DOEP_BASE 0xb00
#define DWC2_OFF_DIEP_STRIDE 0x20
#define DWC2_OFF_DOEP_STRIDE 0x20

/* ── 电源与时钟 ───────────────────────────────────────────────────────── */

#define DWC2_OFF_PCGCTL   0xe00

/* ==========================================================================
 * 2. 位域定义 — 全局寄存器
 * ========================================================================== */

/* ── GAHBCFG (0x008) ──────────────────────────────────────────────────── */

#define GAHBCFG_GLBL_INTR_EN   (1u << 0)
#define GAHBCFG_HBSTLEN_SHIFT   1
#define GAHBCFG_HBSTLEN_MASK    0xfu
#define GAHBCFG_HBSTLEN_SINGLE  0
#define GAHBCFG_HBSTLEN_INCR    1
#define GAHBCFG_HBSTLEN_INCR4   3
#define GAHBCFG_HBSTLEN_INCR8   5
#define GAHBCFG_HBSTLEN_INCR16  7
#define GAHBCFG_DMA_EN          (1u << 5)

/* ── GUSBCFG (0x00c) ──────────────────────────────────────────────────── */

#define GUSBCFG_TOUTCAL_SHIFT       0
#define GUSBCFG_TOUTCAL_MASK        0x7u
#define GUSBCFG_PHYIF16             (1u << 3)
#define GUSBCFG_ULPI_UTMI_SEL       (1u << 4)
#define GUSBCFG_FSINTF              (1u << 5)
#define GUSBCFG_PHYSEL              (1u << 6)
#define GUSBCFG_SRPCAP              (1u << 8)
#define GUSBCFG_HNPCAP              (1u << 9)
#define GUSBCFG_USBTRDTIM_SHIFT     10
#define GUSBCFG_USBTRDTIM_MASK      0xfu
#define GUSBCFG_TERMSELDLPULSE      (1u << 22)
#define GUSBCFG_FORCEHOSTMODE       (1u << 29)
#define GUSBCFG_FORCEDEVMODE        (1u << 30)

/* ── GRSTCTL (0x010) ──────────────────────────────────────────────────── */

#define GRSTCTL_CSFTRST       (1u << 0)
#define GRSTCTL_RXFFLSH       (1u << 4)
#define GRSTCTL_TXFFLSH       (1u << 5)
#define GRSTCTL_TXFNUM_SHIFT  6
#define GRSTCTL_TXFNUM_MASK   0x1fu
#define GRSTCTL_TXFNUM_ALL    0x10
#define GRSTCTL_CSFTRST_DONE  (1u << 29)
#define GRSTCTL_AHBIDLE       (1u << 31)

/* ── GINTSTS / GINTMSK (0x014/0x018) ──────────────────────────────────── */

#define GINTSTS_CURMODE_HOST  (1u << 0)
#define GINTSTS_MODEMIS       (1u << 1)
#define GINTSTS_OTGINT        (1u << 2)
#define GINTSTS_SOF           (1u << 3)
#define GINTSTS_RXFLVL        (1u << 4)
#define GINTSTS_NPTXFEMP      (1u << 5)
#define GINTSTS_GINNAKEFF     (1u << 6)
#define GINTSTS_GOUTNAKEFF    (1u << 7)
#define GINTSTS_ERLYSUSP      (1u << 10)
#define GINTSTS_USBSUSP       (1u << 11)
#define GINTSTS_USBRST        (1u << 12)
#define GINTSTS_ENUMDONE      (1u << 13)
#define GINTSTS_ISOOUTDROP    (1u << 14)
#define GINTSTS_EOPF          (1u << 15)
#define GINTSTS_EPMIS         (1u << 17)
#define GINTSTS_IEPINT        (1u << 18)
#define GINTSTS_OEPINT        (1u << 19)
#define GINTSTS_INCOMPLPOUT   (1u << 20)
#define GINTSTS_INCOMPLPIN    (1u << 21)
#define GINTSTS_FETSUSP       (1u << 22)
#define GINTSTS_RSTDET        (1u << 23)
#define GINTSTS_PRTINT        (1u << 24)
#define GINTSTS_HCHINT        (1u << 25)
#define GINTSTS_CONIDSTSCHNG  (1u << 28)
#define GINTSTS_DISCONNINT    (1u << 29)
#define GINTSTS_SESSREQINT    (1u << 30)
#define GINTSTS_WKUPINT       (1u << 31)

/* ── GOTGCTL (0x000) ──────────────────────────────────────────────────── */

#define GOTGCTL_VBVALOEN          (1u << 2)
#define GOTGCTL_VBVALOVAL         (1u << 3)
#define GOTGCTL_AVALOEN           (1u << 4)
#define GOTGCTL_AVALOVAL          (1u << 5)
#define GOTGCTL_DBNCE_FLTR_BYPASS (1u << 15)

/* ── GHWCFG2 (0x048) ──────────────────────────────────────────────────── */

#define GHWCFG2_ARCH_SHIFT         3
#define GHWCFG2_ARCH_MASK          0x3u
#define GHWCFG2_NUM_HOST_CHAN_SHIFT 14
#define GHWCFG2_NUM_HOST_CHAN_MASK  0xfu

/* ── GHWCFG3 (0x04c) ──────────────────────────────────────────────────── */

#define GHWCFG3_DFIFO_DEPTH_SHIFT 16
#define GHWCFG3_DFIFO_DEPTH_MASK  0xffffu

/* ── GHWCFG4 (0x050) ──────────────────────────────────────────────────── */

#define GHWCFG4_DED_FIFO_EN        (1u << 25)
#define GHWCFG4_UTMI_PHY_DATA_WIDTH_SHIFT 14
#define GHWCFG4_UTMI_PHY_DATA_WIDTH_MASK  0x3u

/* ── GDFIFOCFG (0x05c) ────────────────────────────────────────────────── */

#define GDFIFOCFG_GDFIFOCFG_SHIFT  0
#define GDFIFOCFG_GDFIFOCFG_MASK   0xffffu
#define GDFIFOCFG_EPINFOBASE_SHIFT 16
#define GDFIFOCFG_EPINFOBASE_MASK  0xffffu

/* ==========================================================================
 * 3. 位域定义 — 主机寄存器
 * ========================================================================== */

/* ── HCFG (0x400) ─────────────────────────────────────────────────────── */

#define HCFG_FSLSPCLKSEL_SHIFT 0
#define HCFG_FSLSPCLKSEL_MASK  0x3u
#define HCFG_FSLSPCLKSEL_48MHZ 1
#define HCFG_FSLSSUPP          (1u << 2)

/* ── HPRT0 (0x440) ────────────────────────────────────────────────────── */

#define HPRT0_CONNSTS   (1u << 0)
#define HPRT0_CONNDET   (1u << 1)
#define HPRT0_ENA       (1u << 2)
#define HPRT0_ENACHG    (1u << 3)
#define HPRT0_OVRCURACT (1u << 4)
#define HPRT0_OVRCURCHG (1u << 5)
#define HPRT0_RST       (1u << 8)
#define HPRT0_LNSTS_SHIFT 10
#define HPRT0_LNSTS_MASK  0x3u
#define HPRT0_PWR       (1u << 12)
#define HPRT0_SPD_SHIFT 17
#define HPRT0_SPD_MASK  0x3u
#define HPRT0_SPD_HS    0
#define HPRT0_SPD_FS    1
#define HPRT0_SPD_LS    2

/* HPRT0 的 W1C 位掩码：read-modify-write 时必须 mask 掉 */
#define HPRT0_W1C_MASK (HPRT0_CONNDET | HPRT0_ENA | HPRT0_ENACHG | HPRT0_OVRCURCHG)

/* ── HAINTMSK (0x418) ─────────────────────────────────────────────────── */

#define HAINTMSK_CHINT_MASK 0xffffu

/* ==========================================================================
 * 4. 位域定义 — 主机通道寄存器
 * ========================================================================== */

/* ── HCCHAR (0x00) ────────────────────────────────────────────────────── */

#define HCCHAR_MPS_SHIFT      0
#define HCCHAR_MPS_MASK       0x7ffu
#define HCCHAR_EPNUM_SHIFT    11
#define HCCHAR_EPNUM_MASK     0xfu
#define HCCHAR_EPDIR          (1u << 15)  /* 1=IN, 0=OUT */
#define HCCHAR_LSPDDEV        (1u << 17)
#define HCCHAR_EPTYPE_SHIFT   18
#define HCCHAR_EPTYPE_MASK    0x3u
#define HCCHAR_EPTYPE_CONTROL   0
#define HCCHAR_EPTYPE_ISO       1
#define HCCHAR_EPTYPE_BULK      2
#define HCCHAR_EPTYPE_INTERRUPT 3
#define HCCHAR_MC_SHIFT       20
#define HCCHAR_MC_MASK        0x3u
#define HCCHAR_DEVADDR_SHIFT  22
#define HCCHAR_DEVADDR_MASK   0x7fu
#define HCCHAR_ODDFRM         (1u << 29)
#define HCCHAR_CHDIS          (1u << 30)
#define HCCHAR_CHENA          (1u << 31)

/* ── HCINT / HCINTMSK (0x08/0x0c) ─────────────────────────────────────── */

#define HCINT_XFERCOMPL  (1u << 0)
#define HCINT_CHHLTD     (1u << 1)
#define HCINT_AHBERR     (1u << 2)
#define HCINT_STALL      (1u << 3)
#define HCINT_NAK        (1u << 4)
#define HCINT_ACK        (1u << 5)
#define HCINT_NYET       (1u << 6)
#define HCINT_XACTERR    (1u << 7)
#define HCINT_BBLERR     (1u << 8)
#define HCINT_FRMOVRN    (1u << 9)
#define HCINT_DATATGLERR (1u << 10)

/* ── HCTSIZ (0x10) ────────────────────────────────────────────────────── */

#define HCTSIZ_XFERSIZE_SHIFT 0
#define HCTSIZ_XFERSIZE_MASK  0x7ffffu
#define HCTSIZ_PKTCNT_SHIFT   19
#define HCTSIZ_PKTCNT_MASK    0x3ffu
#define HCTSIZ_PID_SHIFT      29
#define HCTSIZ_PID_MASK       0x3u
#define HCTSIZ_PID_DATA0      0
#define HCTSIZ_PID_DATA2      1
#define HCTSIZ_PID_DATA1      2
#define HCTSIZ_PID_SETUP      3

/* ==========================================================================
 * 5. CV182x 片内 USB2 PHY 寄存器
 * ========================================================================== */

#define CV182X_PHY_OFFSET  0x000  /* PHY register offset inside CV182x USB2 PHY block (sg2002: 0x03006000) */

/* PHY 寄存器偏移 */
#define CV182X_PHY_REG000  0x000
#define CV182X_PHY_REG004  0x004
#define CV182X_PHY_REG008  0x008
#define CV182X_PHY_REG00C  0x00c
#define CV182X_PHY_REG010  0x010
#define CV182X_PHY_REG014  0x014
#define CV182X_PHY_REG018  0x018
#define CV182X_PHY_REG01C  0x01c
#define CV182X_PHY_REG020  0x020
#define CV182X_PHY_REG024  0x024
#define CV182X_PHY_REG028  0x028
#define CV182X_PHY_REG02C  0x02c
#define CV182X_PHY_REG030  0x030
#define CV182X_PHY_REG034  0x034
#define CV182X_PHY_REG038  0x038
#define CV182X_PHY_REG03C  0x03c
#define CV182X_PHY_REG040  0x040
#define CV182X_PHY_REG044  0x044
#define CV182X_PHY_REG048  0x048
#define CV182X_PHY_REG04C  0x04c
#define CV182X_PHY_REG050  0x050
#define CV182X_PHY_REG054  0x054

/* REG014 位域 (UTMI override / host 模式控制) */
#define PHY_REG014_UTMI_OVERRIDE (1u << 0)
#define PHY_REG014_OPMODE_SHIFT  1
#define PHY_REG014_OPMODE_MASK   0x3u
#define PHY_REG014_OPMODE_NORMAL 0
#define PHY_REG014_XCVRSEL_SHIFT 3
#define PHY_REG014_XCVRSEL_MASK  0x3u
#define PHY_REG014_XCVRSEL_HS    0
#define PHY_REG014_XCVRSEL_FS    1
#define PHY_REG014_XCVRSEL_LS    2
#define PHY_REG014_TERMSEL       (1u << 5)
#define PHY_REG014_DPPULLDOWN    (1u << 6)
#define PHY_REG014_DMPULLDOWN    (1u << 7)
#define PHY_REG014_UTMI_RESET    (1u << 8)

/* ==========================================================================
 * 6. USB 协议常量
 * ========================================================================== */

/* 标准请求 */
#define USB_REQ_DIR_DEVICE_TO_HOST 0x80
#define USB_REQ_DIR_HOST_TO_DEVICE 0x00
#define USB_REQ_TYPE_STANDARD      0x00
#define USB_REQ_TYPE_CLASS         0x20
#define USB_REQ_TYPE_VENDOR        0x40
#define USB_REQ_RECIP_DEVICE       0x00
#define USB_REQ_RECIP_INTERFACE    0x01
#define USB_REQ_RECIP_ENDPOINT     0x02
#define USB_REQ_RECIP_OTHER        0x03

#define USB_REQ_GET_STATUS        0
#define USB_REQ_CLEAR_FEATURE     1
#define USB_REQ_SET_FEATURE       3
#define USB_REQ_SET_ADDRESS       5
#define USB_REQ_GET_DESCRIPTOR    6
#define USB_REQ_SET_DESCRIPTOR    7
#define USB_REQ_GET_CONFIGURATION 8
#define USB_REQ_SET_CONFIGURATION 9
#define USB_REQ_GET_INTERFACE     10
#define USB_REQ_SET_INTERFACE     11

/* 描述符类型 */
#define USB_DESC_DEVICE          1
#define USB_DESC_CONFIGURATION   2
#define USB_DESC_STRING          3
#define USB_DESC_INTERFACE       4
#define USB_DESC_ENDPOINT        5
#define USB_DESC_HUB             0x29

/* Hub 类请求 */
#define HUB_REQ_GET_STATUS       0
#define HUB_REQ_CLEAR_FEATURE    1
#define HUB_REQ_SET_FEATURE      3
#define HUB_REQ_GET_DESCRIPTOR   6

/* Hub 特性选择器 */
#define HUB_FEATURE_PORT_CONNECTION   0
#define HUB_FEATURE_PORT_ENABLE       1
#define HUB_FEATURE_PORT_SUSPEND      2
#define HUB_FEATURE_PORT_OVER_CURRENT 3
#define HUB_FEATURE_PORT_RESET        4
#define HUB_FEATURE_PORT_POWER        8
#define HUB_FEATURE_PORT_LOWSPEED     9
#define HUB_FEATURE_C_PORT_CONNECTION 16
#define HUB_FEATURE_C_PORT_ENABLE     17
#define HUB_FEATURE_C_PORT_SUSPEND    18
#define HUB_FEATURE_C_PORT_OVER_CURRENT 19
#define HUB_FEATURE_C_PORT_RESET      20
#define HUB_FEATURE_PORT_INDICATOR    22

/* 设备速度 (枚举后) */
#define USB_SPEED_HIGH 0
#define USB_SPEED_FULL 1
#define USB_SPEED_LOW  2

/* DWC2 核心版本常量 */
#define DWC2_CORE_REV_2_91A  0x4f54291a
#define DWC2_CORE_REV_4_20A  0x4f54420a
#define DWC2_CORE_REV_MASK   0xffff

/* ==========================================================================
 * 7. 便利宏：计算寄存器地址
 * ========================================================================== */

/* 主机通道寄存器地址 */
#define DWC2_HC_REG(core_base, ch, reg_off) \
    ((core_base) + DWC2_OFF_HC_BASE + (ch) * DWC2_OFF_HC_STRIDE + (reg_off))

/* 全局寄存器地址 */
#define DWC2_REG(core_base, reg_off) ((core_base) + (reg_off))

/* PHY 寄存器地址 */
#define DWC2_PHY_REG(phy_base, reg_off) ((phy_base) + (reg_off))

#endif /* __DWC2_REGS_H__ */
