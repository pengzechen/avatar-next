/* driver/usb/dwc3.c
 *
 * DWC3 USB3 控制器驱动 — RK3588
 *
 * DWC3 是 DesignWare USB3 DRD（双角色设备）控制器，内部集成了 xHCI 主机控制器。
 * RK3588 有两个 DWC3 控制器：USB3 OTG0 (0xFC000000) 和 USB3 OTG1 (0xFC400000)。
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * 初始化流程总览
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * 1. dwc3_probe()        - 探测控制器，打印硬件信息
 * 2. dwc3_host_init()    - 切换到 Host 模式，配置 xHCI
 * 3. dwc3_xhci_start()   - 启动 xHCI，扫描已连接设备
 * 4. dwc3_hid_enumerate()- 枚举 HID 设备（见 dwc3_enum.c）
 *
 * ──────────────────────────────────────────────────────────────────────────────
 * 阶段 1: dwc3_probe() - 探测
 * ──────────────────────────────────────────────────────────────────────────────
 *
 *   读取并打印：
 *   - xHCI 版本、端口数、插槽数
 *   - DWC3 IP 版本（GSNPSID）
 *   - 当前工作模式（GCTL.PRTCAPDIR）
 *
 * ──────────────────────────────────────────────────────────────────────────────
 * 阶段 2: dwc3_host_init() - 主机模式初始化
 * ──────────────────────────────────────────────────────────────────────────────
 *
 *   2.1 PHY 配置（GUSB2PHYCFG0）
 *       - 清除 SUSPHY（枚举期间禁止 PHY suspend）
 *       - 设置 USBTRDTIM=9（8-bit UTMI）
 *
 *   2.2 Core 软复位（可选，RK3588 通常跳过）
 *       - GCTL.CORESOFTRESET 自清零
 *
 *   2.3 切换到 Host 模式
 *       - GCTL.PRTCAPDIR = 01b (Host)
 *
 *   2.4 RK3588 特定配置（GUCTL/GUCTL2）
 *       - USBHSTINAUTORETRYEN: Host IN 自动重试
 *       - TX_IPGAP_LINECHECK_DIS: 禁用 TX IP Gap 检查（HS 枚举必须）
 *       - DIS_DEL_PHY_POWER_CHG: 禁用延迟 PHY 功耗变化（防 SET_ADDRESS 挂死）
 *
 *   2.5 恢复 SUSPHY（模式切换后重新启用）
 *
 *   2.6 xHCI 控制器复位
 *       - USBCMD.HCRST=1，等待 HCRST 清零 + USBSTS.CNR 清零
 *
 *   2.7 启动控制器
 *       - USBCMD.RS=1，等待 HCHalted 清零
 *
 * ──────────────────────────────────────────────────────────────────────────────
 * 阶段 3: dwc3_xhci_start() - 启动并扫描
 * ──────────────────────────────────────────────────────────────────────────────
 *
 *   3.1 VBUS 上电（GPIO3_B7）
 *       - 使能 USB Host VBUS 5V
 *
 *   3.2 停止控制器（如果正在运行）
 *       - USBCMD.RS=0，等待 HCHalted=1
 *
 *   3.3 初始化数据结构
 *       - DCBAA: Device Context Base Address Array
 *       - Command Ring: 16 TRBs + Link TRB
 *       - Event Ring: 16 TRBs
 *       - Scratchpad Buffers: 32 × 4KB 页
 *
 *   3.4 配置寄存器
 *       - DCBAAP_LO/HI: DCBAA 物理地址
 *       - CRCR_LO/HI: Command Ring 指针（RCS=1）
 *       - ERSTSZ/ERSTBA/ERDP: Event Ring 配置
 *       - MaxSlotsEn: 最少 2 个 slot
 *
 *   3.5 启动控制器
 *       - USBCMD.RS=1
 *
 *   3.6 扫描端口
 *       - 读取 PORTSC，检查 CCS（Current Connect Status）
 *       - 打印已连接设备的速度
 *
 * ──────────────────────────────────────────────────────────────────────────────
 * 阶段 4: dwc3_hid_enumerate() - 设备枚举（见 dwc3_enum.c）
 * ──────────────────────────────────────────────────────────────────────────────
 *
 *   端口复位 → Enable Slot → Address Device → GET_DESCRIPTOR → SET_CONFIGURATION
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * 框架集成
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   - 由 platform.lua 的 register_device("usb0") 在 drivers 阶段调用
 *   - 通过 Lua 绑定触发各个初始化函数
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

/* ──────────────────────────────────────────────────────────────────────────────
 * Orange Pi 5 Plus USB VBUS 5V 上电
 * ──────────────────────────────────────────────────────────────────────────────
 *
 * vcc5v0_host 调节器由 GPIO3_B7（GPIO3[15]）通过负载开关芯片控制。
 * ACTIVE_HIGH：高电平使能 VBUS 5V 输出。
 *
 * 必须在 xHCI 启动前拉高，否则 USB 端口无法向设备供电。
 * USB 规范要求 VBUS 稳定 ≥100ms 后设备才能响应枚举。
 *
 * 硬件连接：
 *   GPIO3_B7 → 负载开关芯片使能引脚 → USB VBUS 5V
 *
 * DTS 来源（Linux kernel）：
 *   vcc5v0-host {
 *       gpio = <&gpio3 RK_PB7 GPIO_ACTIVE_HIGH>;
 *   }
 *   pinctrl: vcc5v0-host-en {
 *       rockchip,pins = <3 15 0 ...>;
 *   }
 *
 * RK3588 GPIO 寄存器（新式 GPIO：高 16 位 = 写使能掩码，低 16 位 = 数据值）：
 *   GPIO3 base:            0xFEC40000
 *   SYS_IOC base:          0xFD5F0000
 *   GPIO3B_IOMUX_H offset: 0x600C  （pins 12-15，每 pin 4 bit）
 *   SWPORT_DR_L  offset:   0x00    （data [15:0]）
 *   SWPORT_DDR_L offset:   0x08    （direction [15:0]）
 *
 * GPIO3[15] = bit 15；mask-write 值 = (1<<31)|(1<<15) = 0x80008000
 * ──────────────────────────────────────────────────────────────────────────────*/

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

/* ──────────────────────────────────────────────────────────────────────────────
 * 单控制器探测
 * ──────────────────────────────────────────────────────────────────────────────
 *
 * 读取并打印控制器的硬件信息：
 *   1. xHCI 能力寄存器（位于基地址 + 0x00）
 *      - HCIVERSION: xHCI 规范版本
 *      - CAPLENGTH: 操作寄存器偏移
 *      - HCSPARAMS1: 最大插槽/端口/中断数
 *      - HCCPARAMS1: 64-bit 寻址能力（AC64）、上下文大小（CSZ）
 *   2. DWC3 全局寄存器（位于基地址 + 0xC100）
 *      - GSNPSID: IP 版本 ID（0x5533300a = v3.00a）
 *      - GCTL: 全局控制，包括当前工作模式
 *      - GHWPARAMS0/1: 硬件参数
 *
 * 输出示例：
 *   dwc3: ─── USB3 OTG0 @ 0xffff0000fc000000 ───
 *   dwc3:   xHCI 1.10  caplength=32  MaxSlots=64  MaxPorts=2  MaxIntrs=1
 *   dwc3:   HCCPARAMS1=0x0220fe64  AC64=0  CSZ=1
 *   dwc3:   GSNPSID=0x5533300a  => DWC3 v3.00a
 *   dwc3:   GCTL=0x30c12004  mode=Device
 * ──────────────────────────────────────────────────────────────────────────────*/

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

/* ──────────────────────────────────────────────────────────────────────────────
 * 单控制器 Host 模式初始化
 * ──────────────────────────────────────────────────────────────────────────────
 *
 * 将 DWC3 从 Device/OTG 模式切换到 Host 模式，并完成 xHCI 基础配置。
 *
 * 步骤：
 *   1. PHY 配置（GUSB2PHYCFG0）
 *      - 清除 SUSPHY：枚举期间禁止 PHY suspend
 *      - 设置 USBTRDTIM=9：8-bit UTMI 接口必须
 *   2. Core 软复位（可选，RK3588 跳过）
 *      - GCTL.CORESOFTRESET 自清零
 *      - 跳过原因：BootROM/U-Boot 已初始化 PHY，软复位会扰动时序
 *   3. 切换到 Host 模式
 *      - GCTL.PRTCAPDIR = 01b (Host)
 *   4. RK3588 特定配置（GUCTL/GUCTL2）
 *      - USBHSTINAUTORETRYEN: Host IN 自动重试
 *      - TX_IPGAP_LINECHECK_DIS: 禁用 TX IP Gap 检查（HS 枚举必须）
 *      - DIS_DEL_PHY_POWER_CHG: 禁用延迟 PHY 功耗变化（防 SET_ADDRESS 挂死）
 *   5. 恢复 SUSPHY（模式切换后重新启用）
 *   6. xHCI 控制器复位（USBCMD.HCRST=1）
 *   7. 启动控制器（USBCMD.RS=1）
 *   8. 读取端口状态
 *
 * 输出示例：
 *   dwc3: [OTG0] host init  caplength=32  op_base=0xffff0000fc000020
 *   dwc3: [OTG0] GUSB2PHYCFG0=0x40102400
 *   dwc3: [OTG0] core soft reset skipped  GCTL=0x30c12004  GSTS=0x7e800000
 *   dwc3: [OTG0] GCTL after host mode set: 0x30c11004  mode=Host
 *   dwc3: [OTG0] GUCTL=0x02024210
 *   dwc3: [OTG0] GUCTL2=0x0000140d
 *   dwc3: [OTG0] SUSPHY re-enabled  GUSB2PHYCFG0=0x40102440
 *   dwc3: [OTG0] xHCI reset done  USBSTS=0x00000001  halted
 *   dwc3: [OTG0] starting controller...
 *   dwc3: [OTG0] controller started  USBSTS=0x00000000
 *   dwc3: [OTG0] port1  PORTSC=0x000002a0  ---  PP=1  CCS=0
 *   dwc3: [OTG0] port2  PORTSC=0x000002a0  ---  PP=1  CCS=0
 * ──────────────────────────────────────────────────────────────────────────────*/

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

    /* 3. RK3588 的 USB2 PHY 通常已由 BootROM/U-Boot 初始化。
     * CoreSoftReset 会扰动 DWC3↔PHY 时序；端口 reset 仍能完成，但
     * BSR=0 Address Device 可能卡在 SET_ADDRESS 总线事务中不返回事件。
     * 因此这里只清掉测试缩放位并保留 PHY/PLL 状态。*/
    uint32_t gctl = dwc3_read32(base, DWC3_GCTL);
    gctl &= ~DWC3_GCTL_CORESOFTRESET;
    gctl &= ~DWC3_GCTL_SCALEDOWN_MASK;
    write32(gctl, (void *)(base + DWC3_GCTL));
    uint32_t gsts = dwc3_read32(base, DWC3_GSTS);
    dwc3_poll_delay(1000000U);

    KLOG_INFO("dwc3: [OTG%d] core soft reset skipped  GCTL=0x%08x  GSTS=0x%08x\n",
              id, (unsigned)dwc3_read32(base, DWC3_GCTL), (unsigned)gsts);

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
    guctl |= DWC3_GUCTL_TX_IPGAP_LINECHECK_DIS;  /* RK3588: 禁用 TX IP Gap 行检查 */
    guctl |= DWC3_GUCTL_PARKMODE_DISABLE_SS;  /* RK3588 OTG1 DTS quirk */
    write32(guctl, (void *)(base + DWC3_GUCTL));
    KLOG_INFO("dwc3: [OTG%d] GUCTL=0x%08x\n", id,
              (unsigned)dwc3_read32(base, DWC3_GUCTL));

    /* 4c. GUCTL2: RK3588 禁用延迟 PHY 功耗变化，防止 SET_ADDRESS 挂死 */
    uint32_t guctl2 = dwc3_read32(base, DWC3_GUCTL2);
    guctl2 |= DWC3_GUCTL2_DIS_DEL_PHY_POWER_CHG;
    write32(guctl2, (void *)(base + DWC3_GUCTL2));
    KLOG_INFO("dwc3: [OTG%d] GUCTL2=0x%08x\n", id,
              (unsigned)dwc3_read32(base, DWC3_GUCTL2));

    /* 4d. 重新启用 SUSPHY（根据 Linux dwc3_enable_susphy，模式切换后应该启用）
     * DWC3 databook: SUSPHY 应该在 PHY 初始化完成后设置以节省功耗 */
    phycfg = dwc3_read32(base, DWC3_GUSB2PHYCFG0);
    phycfg |= DWC3_GUSB2PHYCFG_SUSPHY;
    write32(phycfg, (void *)(base + DWC3_GUSB2PHYCFG0));

    pipectl = dwc3_read32(base, DWC3_GUSB3PIPECTL0);
    pipectl |= DWC3_GUSB3PIPECTL_SUSPEN;
    write32(pipectl, (void *)(base + DWC3_GUSB3PIPECTL0));

    KLOG_INFO("dwc3: [OTG%d] SUSPHY re-enabled  GUSB2PHYCFG0=0x%08x\n",
              id, (unsigned)dwc3_read32(base, DWC3_GUSB2PHYCFG0));

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

    /* 检查 Host System Error 标志 */
    if (usbsts & XHCI_USBSTS_HSE) {
        KLOG_ERROR("dwc3: [OTG%d] Host System Error after reset! USBSTS=0x%08x\n",
                   id, usbsts);
        /* 尝试清除 HSE 标志（RW1C）*/
        xhci_op_write(op_base, XHCI_OP_USBSTS, XHCI_USBSTS_HSE);
        dwc3_poll_delay(100000U);
        usbsts = xhci_op_read(op_base, XHCI_OP_USBSTS);
        if (usbsts & XHCI_USBSTS_HSE) {
            KLOG_ERROR("dwc3: [OTG%d] HSE flag persistent! USBSTS=0x%08x\n",
                       id, usbsts);
            return -1;
        }
    }

    KLOG_INFO("dwc3: [OTG%d] xHCI reset done  USBSTS=0x%08x  %s\n",
              id, usbsts,
              (usbsts & XHCI_USBSTS_HCH) ? "halted" : "running");

    /* 5b. 启动控制器（复位后处于停止状态，需要设置 RS=1）*/
    if (usbsts & XHCI_USBSTS_HCH) {
        KLOG_INFO("dwc3: [OTG%d] starting controller...\n", id);
        usbcmd = xhci_op_read(op_base, XHCI_OP_USBCMD);
        usbcmd |= XHCI_USBCMD_RS;  /* Run/Stop = 1 */
        xhci_op_write(op_base, XHCI_OP_USBCMD, usbcmd);

        /* 等待控制器启动 */
        dwc3_poll_delay(100000U);
        usbsts = xhci_op_read(op_base, XHCI_OP_USBSTS);
        KLOG_INFO("dwc3: [OTG%d] controller started  USBSTS=0x%08x\n",
                  id, usbsts);

        if (usbsts & XHCI_USBSTS_HCH) {
            KLOG_ERROR("dwc3: [OTG%d] controller failed to start!\n", id);
            return -1;
        }
    }

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
 * xHCI 启动 + 设备扫描
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * 完成以下工作：
 *   1. 上电 USB Host VBUS 5V（GPIO3_B7）
 *   2. 初始化 xHCI 数据结构（DCBAA、命令环、事件环、Scratchpad）
 *   3. 配置 xHCI 寄存器
 *   4. 启动控制器（USBCMD.RS=1）
 *   5. 扫描端口，检测已连接设备
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 强制 64 字节对齐（xHCI 规范要求）*/
xhci_hw_t g_xhci_hw[2] __attribute__((aligned(64)));   /* [0]=OTG0  [1]=OTG1 */

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

/* ──────────────────────────────────────────────────────────────────────────────
 * 单控制器 xHCI 启动
 * ──────────────────────────────────────────────────────────────────────────────
 *
 * 初始化 xHCI 数据结构并启动控制器。
 *
 * 步骤：
 *   1. 停止控制器（如果正在运行）
 *      - USBCMD.RS=0，等待 HCHalted=1
 *      - 检测错误标志，必要时进行完整复位
 *
 *   2. 清零所有数据结构
 *
 *   3. 初始化 DCBAA（Device Context Base Address Array）
 *      - DCBAA[0]: Scratchpad 指针数组
 *      - DCBAA[1..64]: Slot 设备上下文指针
 *
 *   4. 初始化 Command Ring
 *      - 16 个 TRB + 1 个 Link TRB（回绕）
 *      - 初始 PCS=1（Producer Cycle State）
 *      - CRCR.RCS=1（硬件 Consumer Cycle State）
 *
 *   5. 初始化 Event Ring
 *      - 16 个 TRB，初始 cycle=0（空槽）
 *      - ERST: Event Ring Segment Table
 *      - ERDP: Event Ring Dequeue Pointer
 *
 *   6. 配置 MaxSlotsEn
 *      - 至少 2 个 slot（某些控制器要求）
 *
 *   7. 初始化 Scratchpad Buffers
 *      - 32 × 4KB 页
 *      - DCBAA[0] = 指针数组物理地址
 *
 *   8. 启动控制器
 *      - USBCMD.RS=1
 *      - 等待 HCHalted 清零
 *
 *   9. 扫描端口
 *      - 读取 PORTSC，检查 CCS
 *      - 打印设备速度（FS/LS/HS/SS）
 *
 * 输出示例：
 *   dwc3: [OTG0] xhci_start  rt_base=0xffff0000fc000440
 *   dwc3: [OTG0] CRCR after init: RCS=0  crcr_lo=0x00000000
 *   dwc3: [OTG0] Command Ring initialized: PCS=1  CRCR_written=0x005d5481
 *   dwc3: [OTG0] HCSPARAMS1=0x02000140 MaxSlots=64  HCSPARAMS2=0x0c0000f1 MaxScratchpad=32
 *   dwc3: [OTG0] MaxSlotsEn set to 2
 *   dwc3: [OTG0] scratchpad: 32 bufs  array_phys=0x005d5000
 *   dwc3: [OTG0] xHCI running  USBSTS=0x00000000
 *   dwc3: [OTG0] port1  no device  PLS=RxDetect  PORTSC=0x000002a0
 *   dwc3: [OTG1] port1  *** DEVICE CONNECTED ***  speed=FullSpeed  PLS=Polling
 * ──────────────────────────────────────────────────────────────────────────────*/

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

    /* ── 1. 停止控制器（如果正在运行），以便重新配置数据结构 ─── */
    uint32_t usbcmd = xhci_op_read(op_base, XHCI_OP_USBCMD);
    uint32_t usbsts = xhci_op_read(op_base, XHCI_OP_USBSTS);

    KLOG_INFO("dwc3: [OTG%d] current USBCMD=0x%08x  USBSTS=0x%08x\n",
              id, usbcmd, usbsts);

    /* 如果控制器正在运行，先停止它 */
    if (usbcmd & XHCI_USBCMD_RS) {
        KLOG_INFO("dwc3: [OTG%d] stopping controller for reconfiguration...\n", id);
        usbcmd &= ~XHCI_USBCMD_RS;  /* 清除 RS 位 */
        xhci_op_write(op_base, XHCI_OP_USBCMD, usbcmd);

        /* 等待控制器停止（HCH=1），最多 ~100ms */
        uint32_t timeout = 1000000U;
        while (timeout--) {
            usbsts = xhci_op_read(op_base, XHCI_OP_USBSTS);
            if (usbsts & XHCI_USBSTS_HCH)
                break;
            dwc3_poll_delay(10);
        }
        usbsts = xhci_op_read(op_base, XHCI_OP_USBSTS);
        KLOG_INFO("dwc3: [OTG%d] controller stopped  USBSTS=0x%08x\n", id, usbsts);

        /* 检查是否有严重错误标志（HSE/HCE/SRE），如果有则进行完整复位
         * 注意：USBSTS 错误标志是 RW1C（Write 1 to Clear），但某些持久性
         * 错误需要完整的控制器复位才能清除 */
        uint32_t error_flags = usbsts & 0x1013U;  /* HSE|HCE|SRE|HCH */
        if (error_flags & ~XHCI_USBSTS_HCH) {  /* 除了 HCH 外的其他错误 */
            KLOG_WARN("dwc3: [OTG%d] Error flags detected (0x%08x), performing full reset...\n",
                      id, error_flags);
            /* 执行 xHCI 控制器复位（HCRST）*/
            usbcmd = xhci_op_read(op_base, XHCI_OP_USBCMD);
            usbcmd |= XHCI_USBCMD_HCRST;
            xhci_op_write(op_base, XHCI_OP_USBCMD, usbcmd);

            /* 等待 HCRST 清零 */
            timeout = 1000000U;
            while (timeout--) {
                usbcmd = xhci_op_read(op_base, XHCI_OP_USBCMD);
                if (!(usbcmd & XHCI_USBCMD_HCRST))
                    break;
                dwc3_poll_delay(10);
            }

            /* 等待 CNR 清零（控制器就绪）*/
            timeout = 1000000U;
            while (timeout--) {
                usbsts = xhci_op_read(op_base, XHCI_OP_USBSTS);
                if (!(usbsts & XHCI_USBSTS_CNR))
                    break;
                dwc3_poll_delay(10);
            }

            usbsts = xhci_op_read(op_base, XHCI_OP_USBSTS);
            KLOG_INFO("dwc3: [OTG%d] Full reset complete  USBSTS=0x%08x\n", id, usbsts);
        }
    }

    /* ── 2. 清零所有数据结构 ─── */
    memset(hw, 0, sizeof(*hw));

    /* ── 3. DCBAA ─── */
    uint32_t dcbaa_phys = xhci_phys32(hw->dcbaa);
    xhci_op_write(op_base, XHCI_OP_DCBAAP_LO, dcbaa_phys);
    xhci_op_write(op_base, XHCI_OP_DCBAAP_HI, 0U);

    /* ── 4. Command Ring（末尾 Link TRB 回绕到起点）─── */
    uint32_t cmd_phys = xhci_phys32(hw->cmd_ring);
    /* 读取 CRCR 仅用于诊断日志 */
    uint32_t crcr_lo = xhci_op_read(op_base, XHCI_OP_CRCR_LO);

    KLOG_INFO("dwc3: [OTG%d] CRCR after init: RCS=%u  crcr_lo=0x%08x\n",
              id, (unsigned)(crcr_lo & XHCI_CRCR_RCS), (unsigned)crcr_lo);

    /* xHCI 规范 §4.9.3: 命令环始终以 PCS=1 初始化。
     * 空 TRB cycle=0 阻止硬件越过未写入的槽位；
     * ring_enq 写 cycle=1 的 TRB 供硬件消费。
     * CRCR.RCS=1 使硬件的 CCS=1，只消费 cycle=1 的 TRB。 */

    /* 清零所有 TRB（cycle=0 = 空，硬件不处理）*/
    for (uint32_t i = 0; i < XHCI_CMD_RING_TRBS; i++) {
        hw->cmd_ring[i].param_lo = 0U;
        hw->cmd_ring[i].param_hi = 0U;
        hw->cmd_ring[i].status   = 0U;
        hw->cmd_ring[i].control  = 0U;
    }

    /* Link TRB: type=Link, TC=1, cycle=0（PCS=1 的取反，初始阻止硬件回绕）*/
    hw->cmd_ring[XHCI_CMD_RING_TRBS - 1U].param_lo = cmd_phys;
    hw->cmd_ring[XHCI_CMD_RING_TRBS - 1U].param_hi = 0U;
    hw->cmd_ring[XHCI_CMD_RING_TRBS - 1U].control  =
        XHCI_TRB_TYPE_LINK | XHCI_TRB_LINK_TC | 0U;

    /* CRCR: cmd_ring 物理地址 | RCS=1（硬件 CCS=1，消费 cycle=1 的 TRB）*/
    /* 先写 CS=1 确保 CRR=0（DWC3 在上一次启动后可能保持 CRR=1）*/
    xhci_op_write(op_base, XHCI_OP_CRCR_LO, XHCI_CRCR_CS);
    xhci_op_write(op_base, XHCI_OP_CRCR_HI, 0U);
    {
        uint32_t wait = 500000U;
        while (wait--) {
            if (!(xhci_op_read(op_base, XHCI_OP_CRCR_LO) & (1U << 3)))
                break;
            dwc3_poll_delay(10);
        }
    }
    xhci_op_write(op_base, XHCI_OP_CRCR_LO, cmd_phys | 1U);
    xhci_op_write(op_base, XHCI_OP_CRCR_HI, 0U);
    /* 读回验证写入是否生效 */
    uint32_t crcr_rb = xhci_op_read(op_base, XHCI_OP_CRCR_LO);
    KLOG_INFO("dwc3: [OTG%d] Command Ring initialized: PCS=1  CRCR_written=0x%08x  readback=0x%08x\n",
              id, (unsigned)(cmd_phys | 1U), (unsigned)crcr_rb);

    /* ── 5. Event Ring Segment Table ─── */
    uint32_t evt_phys  = xhci_phys32(hw->evt_ring);
    uint32_t erst_phys = xhci_phys32(hw->erst);

    /* 初始化事件环 TRB：Cycle Bit = 0（空槽，硬件尚未写入）
     * xHCI 规范：软件初始 ccs=1，硬件写入事件时用 cycle=1。
     * 若此处置 1，drain 会把所有 16 个空 TRB 当有效事件消费，
     * 导致 deq 回绕、ccs 翻转，使 wait_cmd 永远等不到真正的事件。 */
    for (uint32_t i = 0; i < XHCI_EVT_RING_TRBS; i++) {
        hw->evt_ring[i].param_lo = 0U;
        hw->evt_ring[i].param_hi = 0U;
        hw->evt_ring[i].status   = 0U;
        hw->evt_ring[i].control  = 0U;  /* cycle=0: 空槽 */
    }

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

    /* ── 6. MaxSlotsEn（打印 HCSPARAMS2，设置 MaxSlotsEn）─── */
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
    /* MaxSlotsEn: 某些控制器需要至少 2 个 slot 才能正常工作
     * 根据 xHCI 规范，设置为 0 表示使用 MaxSlots */
    uint32_t max_slots_en = 2U;  /* 至少 2 个 slot */
    if (max_slots_en > max_slots)
        max_slots_en = max_slots;
    xhci_op_write(op_base, XHCI_OP_CONFIG, max_slots_en);
    KLOG_INFO("dwc3: [OTG%d] MaxSlotsEn set to %u\n", id, (unsigned)max_slots_en);

    /* ── 6b. Scratchpad Buffers（xHCI spec §4.20，必须在 R/S=1 之前完成）─── */
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

    /* ── 7. 启动：USBCMD = RS | INTE | HSEE ─── */
    usbcmd = xhci_op_read(op_base, XHCI_OP_USBCMD);
    usbcmd |= XHCI_USBCMD_RS | XHCI_USBCMD_INTE | (1U << 3);
    xhci_op_write(op_base, XHCI_OP_USBCMD, usbcmd);

    /* 等待 HCHalted 清零（最多 ~500 ms）*/
    uint32_t timeout = 5000000U;
    while (timeout--) {
        if (!(xhci_op_read(op_base, XHCI_OP_USBSTS) & XHCI_USBSTS_HCH))
            break;
        dwc3_poll_delay(10);
    }

    usbsts = xhci_op_read(op_base, XHCI_OP_USBSTS);
    if (usbsts & XHCI_USBSTS_HCH) {
        KLOG_ERROR("dwc3: [OTG%d] xHCI failed to start! USBSTS=0x%08x\n",
                   id, usbsts);
        return;
    }

    KLOG_INFO("dwc3: [OTG%d] xHCI running  USBSTS=0x%08x\n", id, usbsts);

    /* ── 8. 等待端口稳定后读连接状态 ─── */
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
