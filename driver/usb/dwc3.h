/* driver/usb/dwc3.h
 *
 * DWC3 (DesignWare USB3) 控制器寄存器定义 — RK3588
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * 硬件概述
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * DWC3 是一个包含 xHCI 主机控制器的 USB3 DRD（双角色设备）控制器。
 *
 * 寄存器分布：
 *   0x0000 - 0x7FFF : 标准 xHCI 寄存器（能力 + 操作 + 运行时 + 门铃）
 *   0xC100 - 0xCFFF : DWC3 全局寄存器（厂商扩展）
 *
 * RK3588 地址：
 *   USB3 OTG0: 0xFC000000  (USB 3.1 Gen1, 5 Gbps)
 *   USB3 OTG1: 0xFC400000  (USB 3.1 Gen1, 5 Gbps)
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * 参考资料
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * - xHCI 规范 v1.2
 * - DWC_usb3 databook
 * - Linux drivers/usb/dwc3/core.h
 * - ref/CrabUSB/usb-host/src/backend/kmod/dwc/
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * 初始化流程
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * 详见 docs/USB_DWC3_INIT.md
 *
 * 1. dwc3_probe()        - 探测控制器，打印硬件信息
 * 2. dwc3_host_init()    - 切换到 Host 模式，配置 xHCI
 * 3. dwc3_xhci_start()   - 启动 xHCI，扫描已连接设备
 * 4. dwc3_hid_enumerate()- 枚举 HID 设备（见 dwc3_enum.c）
 */

#ifndef DWC3_H
#define DWC3_H

#include "types.h"

/* ── xHCI 能力寄存器（位于 DWC3 基址 + 0x000）─────────────────────────── */

/* 0x00: [7:0]=CAPLENGTH, [15:8]=reserved, [31:16]=HCIVERSION */
#define XHCI_CAP_VER            0x00U
#define XHCI_CAPLENGTH(r)       ((r) & 0xFFU)
#define XHCI_HCIVERSION(r)      (((r) >> 16) & 0xFFFFU)

/* 0x04: HCSPARAMS1 — 结构参数1 */
#define XHCI_HCSPARAMS1         0x04U
#define XHCI_MAXSLOTS(r)        ((r) & 0xFFU)
#define XHCI_MAXINTRS(r)        (((r) >> 8) & 0x7FFU)
#define XHCI_MAXPORTS(r)        (((r) >> 24) & 0xFFU)

/* 0x08: HCSPARAMS2 */
#define XHCI_HCSPARAMS2         0x08U

/* 0x10: HCCPARAMS1 — 能力参数1 */
#define XHCI_HCCPARAMS1         0x10U
#define XHCI_AC64(r)            ((r) & 0x1U)   /* 64-bit 寻址能力 */
#define XHCI_CSZ(r)             (((r) >> 2) & 0x1U)  /* 上下文大小 */

/* 0x14: DBOFF — Doorbell Array 偏移 */
#define XHCI_DBOFF              0x14U

/* 0x18: RTSOFF — Runtime Register Space 偏移 */
#define XHCI_RTSOFF             0x18U

/* ── DWC3 全局寄存器（位于 DWC3 基址 + 0xC100）───────────────────────── */

#define DWC3_GLOBALS_REGS_START 0xC100U

/* GCTL: 0xC110 — 全局控制寄存器 */
#define DWC3_GCTL               (DWC3_GLOBALS_REGS_START + 0x10U)
#define DWC3_GCTL_PRTCAPDIR(r)  (((r) >> 12) & 0x3U)
#define DWC3_GCTL_PRTCAP_SHIFT  12U
#define DWC3_GCTL_PRTCAP_MASK   (0x3U << DWC3_GCTL_PRTCAP_SHIFT)
/* PRTCAPDIR 编码 */
#define DWC3_GCTL_PRTCAP_HOST   1U
#define DWC3_GCTL_PRTCAP_DEVICE 2U
#define DWC3_GCTL_PRTCAP_OTG    3U
/* GCTL 控制位 */
#define DWC3_GCTL_CORESOFTRESET (1U << 11)  /* 软复位（自清零）*/
#define DWC3_GCTL_SCALEDOWN_MASK (0x3U << 4)

/* GSNPSID: 0xC120 — 版本 ID
 * 格式：[31:16]=0x5533（magic），[15:12]=major，[11:4]=minor（hex），[3:0]=sub（0xa→'a'）
 * 例：0x5533300a → major=3, minor=0x00, sub=0xa → "v3.00a"
 */
#define DWC3_GSNPSID            (DWC3_GLOBALS_REGS_START + 0x20U)
#define DWC3_GSNPSID_MAGIC      0x55330000UL   /* bits[31:16] 标识 */
#define DWC3_GSNPSID_MASK       0xFFFF0000UL
#define DWC3_GSNPSID_MAJOR(r)   (((r) >> 12) & 0xFU)
#define DWC3_GSNPSID_MINOR(r)   (((r) >> 4)  & 0xFFU)  /* 2 hex digits */
#define DWC3_GSNPSID_SUB(r)     ((r) & 0xFU)           /* 0xa→'a', 0xb→'b'… */

/* GHWPARAMS0: 0xC140 — 硬件参数0 */
#define DWC3_GHWPARAMS0         (DWC3_GLOBALS_REGS_START + 0x40U)
/* GHWPARAMS0 字段 */
#define DWC3_GHWPARAMS0_USB3MDL(r)  ((r) & 0x7U)  /* USB3 模式 */

/* GHWPARAMS1: 0xC144 — 硬件参数1 */
#define DWC3_GHWPARAMS1         (DWC3_GLOBALS_REGS_START + 0x44U)
#define DWC3_GHWPARAMS1_NUM_INT(r)  (((r) >> 15) & 0x3FU)  /* 中断线数 */

/* GSTS: 0xC118 — 全局状态寄存器 */
#define DWC3_GSTS               (DWC3_GLOBALS_REGS_START + 0x18U)
#define DWC3_GSTS_DEVICE_IP     (1U << 2)
#define DWC3_GSTS_HOST_IP       (1U << 3)
#define DWC3_GSTS_CSRTIMEOUT    (1U << 5)

/* GUCTL1: 0xC11C — Global User Control Register 1
 * 注意：GUCTL1 在地址上早于 GUCTL（0xC12C），命名顺序与地址顺序相反。
 * Linux kernel drivers/usb/dwc3/core.h: #define DWC3_GUCTL1 0xc11c
 */
#define DWC3_GUCTL1             (DWC3_GLOBALS_REGS_START + 0x1CU)
#define DWC3_GUCTL1_TX_IPGAP_LINECHECK_DIS (1U << 28) /* RK3588: 禁用 TX IP Gap 行检查（HS枚举必须）*/
#define DWC3_GUCTL1_PARKMODE_DISABLE_SS    (1U << 17) /* SuperSpeed Park Mode 禁用 */
#define DWC3_GUCTL1_PARKMODE_DISABLE_HS    (1U << 16) /* HighSpeed Park Mode 禁用（OTG1 必须）*/

/* DCTL: 0xC704 — Device Control Register
 * 在切换到 Host mode 前必须停止 Device 控制器，否则 Device 状态机持续运行
 * 会干扰 Host mode 的 USB 总线事务（BSR=0 SET_ADDRESS 永远收不到 STATUS ZLP）。
 */
#define DWC3_DCTL               (DWC3_GLOBALS_REGS_START + 0x604U)  /* 0xC704 */
#define DWC3_DCTL_RUN_STOP      (1U << 31)   /* 1=Device running, 0=Device stopped */
#define DWC3_DCTL_CSFTRST       (1U << 30)   /* Device soft reset */
#define DWC3_DCTL_LGSF          (1U << 14)   /* LPM capable (noop for us) */

/* GUCTL: 0xC12C — Global User Control Register */
#define DWC3_GUCTL              (DWC3_GLOBALS_REGS_START + 0x2CU)
#define DWC3_GUCTL_USBHSTINAUTORETRYEN  (1U << 14) /* 主机模式 IN 自动重试 */
/* NOEXTRDL (bit9): 禁止 xHC 预读 TRB（v3.00a pre-fetch cache quirk）
 * 不设置时 xHC 在 Address Device BSR=1 完成后缓存空 ctrl_ring 状态，
 * BSR=0 发 SET_ADDRESS 时永远等待 doorbell，后续命令全部超时。 */
#define DWC3_GUCTL_NOEXTRDL             (1U << 9)

/* GUCTL2: 0xC19C — Global User Control Register 2 */
#define DWC3_GUCTL2             (DWC3_GLOBALS_REGS_START + 0x9CU)
#define DWC3_GUCTL2_DIS_DEL_PHY_POWER_CHG (1U << 12) /* RK3588: 禁用延迟PHY功耗变化（防SET_ADDRESS挂死）*/

/* GFLADJ: 0xC630 — Global Frame Length Adjustment Register
 * RK3588 quirk: snps,gfladj-refclk-240mhz-quirk
 * 参考时钟 24 MHz → DECR = (240/24) - 1 = 9
 * 若不设置，microframe 定时器不运行，xHC 无法调度任何 EP0/EP1 传输。*/
#define DWC3_GFLADJ             (DWC3_GLOBALS_REGS_START + 0x530U)  /* 0xC630 */
#define DWC3_GFLADJ_REFCLK_240MHZDECR_SHIFT  24U
#define DWC3_GFLADJ_REFCLK_240MHZDECR_MASK   (0x3FU << 24U)
#define DWC3_GFLADJ_REFCLK_240MHZDECR_PLS1   (1U << 23U)  /* 余数补偿（24MHz 整除不需要）*/
#define DWC3_GFLADJ_30MHZ_SDBND_SEL          (1U << 7U)   /* SOF sync boundary 由硬件决定 */
#define DWC3_GFLADJ_30MHZ_MASK               0x3FU

/* ── RK3588 USB2 PHY GRF 寄存器（phy-rockchip-inno-usb2.c quirk）────────
 *
 * Rockchip GRF 写格式：32 位写，高 16 位是写掩码，低 16 位是数据。
 *   写 value：(mask << 16) | data
 *   设置 bit N：(1<<N)<<16 | (1<<N)
 *   清除 bit N：(1<<N)<<16 | 0
 *
 * OTG0 (FC000000): u2phy0, PHY GRF @ 0xFD5D0000, type=OTG, reg=0x0000
 * OTG1 (FC400000): u2phy2, PHY GRF @ 0xFD5D8000, type=HOST, reg=0x8000
 */
#define RK3588_USB2PHY0_GRF_PHYS   0xFD5D0000UL  /* OTG0 USB2 PHY */
#define RK3588_USB2PHY2_GRF_PHYS   0xFD5D8000UL  /* OTG1 USB2 PHY */

/* GRF 内部偏移（相对于各自的 PHY GRF 基址）*/
#define USB2PHY_GRF_CON1           0x0004U  /* DC 电压调节 bits[11:8] */
#define USB2PHY_GRF_CON2           0x0008U  /* SIDDQ(bit13), pre-emph(bits[4:3]), phy_sus(bit2) */
#define USB2PHY_GRF_CON3           0x000CU  /* suspend config bits[4:0] */

/* HOST 类型 (reg=0x8000)：phy_sus = {0x0008, 2, 2, 0, 1} */
/* OTG  类型 (reg=0x0000)：phy_sus = {0x000c, 11, 11, 0, 1} */

/* CRU 基址（Clock/Reset Unit）*/
#define RK3588_CRU_BASE_PHYS       0xFD7C0000UL
#define RK3588_CRU_SOFTRST_OFF     0x0400U    /* SOFTRST_CON00 offset */

/* SRST_OTGPHY_U2_0 = 623 → SOFTRST38 (offset 0x0498) bit 15
 * 用于复位 OTG1 的 USB2 PHY（u2phy2） */
#define RK3588_SOFTRST38_OFF       0x0498U    /* CRU_SOFTRST_CON38 = CRU+0x0498 */
#define RK3588_SRST_OTGPHY_U2_0_BIT 15U       /* bit 15 = SRST_OTGPHY_U2_0 */

/* GUSB2PHYCFG0: 0xC200 — USB2 PHY 配置（端口0）*/
#define DWC3_GUSB2PHYCFG0      (DWC3_GLOBALS_REGS_START + 0x100U)
#define DWC3_GUSB2PHYCFG_SUSPHY (1U << 6)   /* Suspend USB2 PHY */
#define DWC3_GUSB2PHYCFG_PHYSOFTRST (1U << 31) /* PHY 软复位 */
#define DWC3_GUSB2PHYCFG_U2_FREECLK_EXISTS (1U << 30) /* RK3588: 清零此位（PHY无自由运行时钟）*/
/* USBTRDTIM: bits[13:10] — USB 2.0 Turnaround Time（8-bit UTMI=9，16-bit=5）*/
#define DWC3_GUSB2PHYCFG_USBTRDTIM_MASK  (0xFU << 10)
#define DWC3_GUSB2PHYCFG_USBTRDTIM(n)    (((n) & 0xFU) << 10)

/* GUSB3PIPECTL0: 0xC2C0 — USB3 PIPE 控制（端口0）*/
#define DWC3_GUSB3PIPECTL0     (DWC3_GLOBALS_REGS_START + 0x1C0U)
#define DWC3_GUSB3PIPECTL_SUSPEN  (1U << 17)  /* Suspend USB3 PHY */
#define DWC3_GUSB3PIPECTL_PHYSOFTRST (1U << 31) /* PHY 软复位 */

/* ── xHCI 能力寄存器补充（位于 DWC3 基址 + 0x000）──────────────────── */
#define XHCI_DBOFF              0x14U       /* Doorbell Array 偏移 */
#define XHCI_RTSOFF             0x18U       /* Runtime Register Space 偏移 */

/* ── xHCI 操作寄存器（base + CAPLENGTH，CAPLENGTH 通常 = 0x20）────────── */
/* 下面偏移量相对于 xHCI operational base（= DWC3_base + caplength）*/
#define XHCI_OP_USBCMD         0x00U
#define XHCI_OP_USBSTS         0x04U
#define XHCI_OP_PAGESIZE       0x08U
#define XHCI_OP_DNCTRL         0x14U  /* Notification Control */
#define XHCI_OP_CRCR_LO       0x18U  /* Command Ring Control (低 32 位) */
#define XHCI_OP_CRCR_HI       0x1CU  /* Command Ring Control (高 32 位) */
#define XHCI_OP_DCBAAP_LO     0x30U  /* Device Context Base Array (低 32) */
#define XHCI_OP_DCBAAP_HI     0x34U  /* Device Context Base Array (高 32) */
#define XHCI_OP_CONFIG         0x38U

/* PORTSC: operational_base + 0x400 + port_index * 0x10 */
#define XHCI_OP_PORTSC(n)      (0x400U + (uint32_t)(n) * 0x10U)

/* USBCMD 位 */
#define XHCI_USBCMD_RS         (1U << 0)   /* Run/Stop */
#define XHCI_USBCMD_HCRST      (1U << 1)   /* Host Controller Reset */
#define XHCI_USBCMD_INTE       (1U << 2)   /* 中断使能 */

/* USBSTS 位 */
#define XHCI_USBSTS_HCH        (1U << 0)   /* HCHalted */
#define XHCI_USBSTS_HSE        (1U << 2)   /* Host System Error */
#define XHCI_USBSTS_CNR        (1U << 11)  /* Controller Not Ready */

/* CRCR 控制位（写 CRCR 低 32 位时） */
#define XHCI_CRCR_RCS          (1U << 0)   /* Ring Cycle State */
#define XHCI_CRCR_CS           (1U << 1)   /* Command Stop */
#define XHCI_CRCR_CA           (1U << 2)   /* Command Abort */
#define XHCI_CRCR_CRR          (1U << 3)   /* Command Ring Running (只读) */

/* PORTSC 位/字段 */
#define XHCI_PORTSC_CCS        (1U << 0)   /* Current Connect Status */
#define XHCI_PORTSC_PED        (1U << 1)   /* Port Enabled */
#define XHCI_PORTSC_PR         (1U << 4)   /* Port Reset */
#define XHCI_PORTSC_PLS(r)     (((r) >> 5) & 0xFU)   /* Port Link State */
#define XHCI_PORTSC_PP         (1U << 9)   /* Port Power */
#define XHCI_PORTSC_SPEED(r)   (((r) >> 10) & 0xFU)  /* 端口速度 1=FS 2=LS 3=HS 4=SS */
#define XHCI_PORTSC_CSC        (1U << 17)  /* Connect Status Change（写 1 清） */
#define XHCI_PORTSC_PLC        (1U << 22)  /* Port Link State Change（写 1 清）*/

/* ── xHCI 运行时寄存器（base + RTSOFF）─────────────────────────────────── */
/* 偏移量相对于 runtime_base = DWC3_base + RTSOFF */
#define XHCI_RT_MFINDEX        0x00U
/* Interrupter 0（从 runtime_base + 0x20 起）*/
#define XHCI_IR0_OFF           0x20U
#define XHCI_IR_IMAN           0x00U       /* Interrupt Management */
#define XHCI_IR_IMOD           0x04U       /* Interrupt Moderation */
#define XHCI_IR_ERSTSZ         0x08U       /* Event Ring Segment Table Size */
#define XHCI_IR_ERSTBA_LO      0x10U       /* ERST Base Address (低 32) */
#define XHCI_IR_ERSTBA_HI      0x14U       /* ERST Base Address (高 32) */
#define XHCI_IR_ERDP_LO        0x18U       /* Event Ring Dequeue Pointer (低 32) */
#define XHCI_IR_ERDP_HI        0x1CU       /* Event Ring Dequeue Pointer (高 32) */
/* ERDP 位 */
#define XHCI_ERDP_EHB          (1U << 3)   /* Event Handler Busy（写 1 清）*/

/* ── xHCI TRB 类型（control[15:10]）────────────────────────────────── */
#define XHCI_TRB_TYPE_SHIFT    10U
#define XHCI_TRB_TYPE_LINK     (6U  << XHCI_TRB_TYPE_SHIFT)  /* Link TRB */
/* Link TRB: Toggle Cycle */
#define XHCI_TRB_LINK_TC       (1U << 1)

/* ──────────────────────────────────────────────────────────────────────────────
 * RK3588 硬编码基址（与 platform.lua 的 usb.base0/1 一致）
 * ──────────────────────────────────────────────────────────────────────────────*/
#define RK3588_USB3_OTG0_BASE   0xFC000000UL
#define RK3588_USB3_OTG1_BASE   0xFC400000UL

/* ──────────────────────────────────────────────────────────────────────────────
 * 驱动 API
 * ──────────────────────────────────────────────────────────────────────────────
 *
 * 这些函数由 platform.lua 在不同阶段调用：
 *
 * 1. dwc3_probe()        - 在 drivers 阶段早期调用
 * 2. dwc3_host_init()    - 在 dwc3_probe() 之后调用
 * 3. dwc3_xhci_start()   - 在 dwc3_host_init() 之后调用
 * 4. dwc3_hid_enumerate()- 在 dwc3_xhci_start() 之后调用
 * ──────────────────────────────────────────────────────────────────────────────*/

/**
 * dwc3_probe - 探测并打印 RK3588 DWC3/xHCI 控制器信息
 *
 * 读取 xHCI 能力寄存器和 DWC3 全局寄存器，打印控制器版本和参数。
 */
void dwc3_probe(void);

/**
 * dwc3_host_init - 将 DWC3 切换到 Host 模式，完成 xHCI 复位
 *
 * 操作：
 * - 配置 PHY（GUSB2PHYCFG0）
 * - 切换到 Host 模式（GCTL.PRTCAPDIR = Host）
 * - 配置 RK3588 特定 quirks（GUCTL/GUCTL2）
 * - 执行 xHCI 控制器复位
 * - 启动控制器（USBCMD.RS=1）
 * - 打印端口状态
 */
void dwc3_host_init(void);

/**
 * dwc3_xhci_start - 配置 xHCI 数据结构并启动控制器
 *
 * 在 dwc3_host_init() 完成后调用。
 *
 * 操作：
 * - 上电 USB Host VBUS 5V（GPIO3_B7）
 * - 初始化 DCBAA、Command Ring、Event Ring、Scratchpad
 * - 配置 xHCI 寄存器（DCBAAP、CRCR、ERST、MaxSlotsEn）
 * - 启动控制器（USBCMD.RS=1）
 * - 扫描所有端口，打印已连接设备的速度信息
 */
void dwc3_xhci_start(void);

/* ──────────────────────────────────────────────────────────────────────────────
 * xHCI 硬件数据结构（dwc3.c 和 dwc3_enum.c 共享）
 * ──────────────────────────────────────────────────────────────────────────────*/
#define XHCI_CMD_RING_TRBS   16U
#define XHCI_EVT_RING_TRBS   16U
#define XHCI_MAX_SCRATCH     32U   /* HCSPARAMS2 MaxScratchpad 上限 */

typedef struct {
    uint32_t param_lo, param_hi, status, control;
} xhci_trb_t;

typedef struct {
    uint32_t addr_lo, addr_hi, seg_size, rsvd;
} xhci_erst_t;

typedef struct {
    uint64_t    dcbaa[65];  /* scratchpad + slot contexts 1..64 */
    xhci_trb_t  cmd_ring[XHCI_CMD_RING_TRBS] __attribute__((aligned(64)));
    xhci_trb_t  evt_ring[XHCI_EVT_RING_TRBS] __attribute__((aligned(64)));
    xhci_erst_t erst[1] __attribute__((aligned(64)));
} __attribute__((aligned(64))) xhci_hw_t;

/* 共享全局状态（定义在 dwc3.c）*/
extern uintptr_t g_dwc3_base0;
extern uintptr_t g_dwc3_base1;
extern xhci_hw_t g_xhci_hw[2];
extern uint64_t  g_scratch_array[2][XHCI_MAX_SCRATCH];
extern uint8_t   g_scratch_pages[2][XHCI_MAX_SCRATCH][4096];

/**
 * dwc3_hid_enumerate - 枚举已连接的 USB HID 设备并读取 HID boot 报告
 *
 * 在 dwc3_xhci_start() 之后调用。
 *
 * 枚举流程：
 * 1. 端口复位
 * 2. Enable Slot Command
 * 3. Address Device Command (BSR=1)
 * 4. GET_DESCRIPTOR(Device)
 * 5. GET_DESCRIPTOR(Configuration)
 * 6. SET_CONFIGURATION
 * 7. SET_PROTOCOL(boot)
 * 8. Configure Endpoint Command
 * 9. 轮询 Interrupt IN 传输，读取报告数据
 */
void dwc3_hid_enumerate(void);

#endif /* DWC3_H */
