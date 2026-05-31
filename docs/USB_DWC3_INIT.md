# USB DWC3 控制器初始化流程

本文档详细说明 RK3588 DWC3/xHCI USB 控制器的初始化流程。

## 目录

1. [硬件概述](#硬件概述)
2. [初始化流程总览](#初始化流程总览)
3. [阶段 1: 探测](#阶段-1-探测)
4. [阶段 2: 主机模式初始化](#阶段-2-主机模式初始化)
5. [阶段 3: xHCI 启动](#阶段-3-xhci-启动)
6. [阶段 4: 设备枚举](#阶段-4-设备枚举)
7. [TRB Ring 管理](#trb-ring-管理)
8. [RK3588 特定 Quirks](#rk3588-特定-quirks)

---

## 硬件概述

### DWC3 控制器架构

```
┌─────────────────────────────────────────────────────────────┐
│                    DWC3 USB3 DRD 控制器                      │
├─────────────────────────────────────────────────────────────┤
│                                                               │
│  ┌──────────────────┐         ┌──────────────────┐          │
│  │   xHCI 主机      │         │   Device 设备     │          │
│  │   控制器         │         │   控制器         │          │
│  └──────────────────┘         └──────────────────┘          │
│           │                            │                      │
│           └────────────┬───────────────┘                      │
│                        │                                      │
│                  ┌─────▼─────┐                                │
│                  │  UTMI/    │                                │
│                  │  PIPE PHY │                                │
│                  └─────┬─────┘                                │
│                        │                                      │
└────────────────────────┼──────────────────────────────────────┘
                         │
                    USB2/3 端口
```

### 寄存器布局

| 偏移范围 | 内容 |
|---------|------|
| 0x0000 - 0x7FFF | 标准 xHCI 寄存器 |
| 0xC100 - 0xCFFF | DWC3 全局寄存器 |

### RK3588 地址

| 控制器 | 物理地址 |
|--------|----------|
| USB3 OTG0 | 0xFC000000 |
| USB3 OTG1 | 0xFC400000 |

---

## 初始化流程总览

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        DWC3 初始化流程                                   │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  platform.lua register_device("usb0")                                  │
│       │                                                                 │
│       ▼                                                                 │
│  ┌─────────────────┐                                                   │
│  │  dwc3_probe()   │  探测控制器，打印硬件信息                           │
│  └────────┬────────┘                                                   │
│           │                                                             │
│           ▼                                                             │
│  ┌─────────────────┐                                                   │
│  │ dwc3_host_init()│  切换到 Host 模式，配置 xHCI                       │
│  └────────┬────────┘                                                   │
│           │                                                             │
│           ▼                                                             │
│  ┌─────────────────┐                                                   │
│  │ dwc3_xhci_start()│ 启动 xHCI，扫描设备                               │
│  └────────┬────────┘                                                   │
│           │                                                             │
│           ▼                                                             │
│  ┌─────────────────────┐                                               │
│  │ dwc3_hid_enumerate()│ 枚举 HID 设备                                  │
│  └─────────────────────┘                                               │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 阶段 1: 探测

### 函数: `dwc3_probe()`

### 目的

读取并打印 DWC3/xHCI 控制器的硬件信息。

### 步骤

1. **获取 MMIO 地址**
   - 从平台配置读取 `usb.base0` 和 `usb.base1`
   - 自动添加 `KERNEL_VMA` 偏移

2. **读取 xHCI 能力寄存器**（基地址 + 0x00）
   - `CAPLENGTH`: 操作寄存器偏移（通常 0x20）
   - `HCIVERSION`: xHCI 规范版本（例如 0x110 = 1.10）
   - `HCSPARAMS1`: MaxSlots/MaxPorts/MaxIntrs
   - `HCCPARAMS1`: AC64（64-bit 能力）、CSZ（上下文大小）

3. **读取 DWC3 全局寄存器**（基地址 + 0xC100）
   - `GSNPSID`: IP 版本 ID（0x5533300a = v3.00a）
   - `GCTL`: 全局控制，包括当前工作模式
   - `GHWPARAMS0/1`: 硬件参数

### 输出示例

```
[INFO][C0] driver/usb/dwc3.c:151: dwc3: probing RK3588 DWC3/xHCI USB controllers
[INFO][C0] driver/usb/dwc3.c:112: dwc3: ─── USB3 OTG0 @ 0xffff0000fc000000 ───
[INFO][C0] driver/usb/dwc3.c:116: dwc3:   xHCI 1.10  caplength=32  MaxSlots=64  MaxPorts=2  MaxIntrs=1
[INFO][C0] driver/usb/dwc3.c:119: dwc3:   HCCPARAMS1=0x0220fe64  AC64=0  CSZ=1
[INFO][C0] driver/usb/dwc3.c:127: dwc3:   GSNPSID=0x5533300a  => DWC3 v3.00a
[INFO][C0] driver/usb/dwc3.c:143: dwc3:   GCTL=0x30c12004  mode=Device
```

---

## 阶段 2: 主机模式初始化

### 函数: `dwc3_host_init()`

### 目的

将 DWC3 从 Device/OTG 模式切换到 Host 模式，并完成 xHCI 基础配置。

### 步骤

#### 2.1 PHY 配置（GUSB2PHYCFG0）

```
初始值: 0x40102400
操作:
  - 清除 SUSPHY (bit6): 禁止 PHY suspend
  - 设置 USBTRDTIM=9 (bits[13:10]): 8-bit UTMI 接口
结果:   0x00102409
```

#### 2.2 Core 软复位（可选，RK3588 跳过）

```
GCTL.CORESOFTRESET (bit11) 自清零
跳过原因: BootROM/U-Boot 已初始化 PHY，软复位会扰动时序
```

#### 2.3 切换到 Host 模式

```
GCTL.PRTCAPDIR (bits[13:12]) = 01b (Host)
操作:
  - 清除 PRTCAPDIR 掩码
  - 设置 PRTCAPDIR = 1
```

#### 2.4 RK3588 特定配置

**GUCTL (0xC12C)**
```
初始值: 0x02004210
设置:
  - USBHSTINAUTORETRYEN (bit14): Host IN 自动重试
  - TX_IPGAP_LINECHECK_DIS (bit9): 禁用 TX IP Gap 检查
  - PARKMODE_DISABLE_SS (bit17): 禁用 SS Park 模式
结果:   0x02024210
```

**GUCTL2 (0xC19C)**
```
初始值: 0x0000140d
设置:
  - DIS_DEL_PHY_POWER_CHG (bit12): 禁用延迟 PHY 功耗变化
结果:   0x0000140d
```

#### 2.5 恢复 SUSPHY

```
GUSB2PHYCFG0.SUSPHY (bit6) = 1
结果: 0x40102440
```

#### 2.6 xHCI 控制器复位

```
USBCMD.HCRST (bit1) = 1
等待:
  - HCRST 清零
  - USBSTS.CNR (bit11) 清零
```

#### 2.7 启动控制器

```
USBCMD.RS (bit0) = 1
等待 USBSTS.HCHalted (bit0) 清零
```

### 输出示例

```
[INFO][C0] driver/usb/dwc3.c:363: dwc3: === host mode init ===
[INFO][C0] driver/usb/dwc3.c:202: dwc3: [OTG0] host init  caplength=32  op_base=0xffff0000fc000020
[INFO][C0] driver/usb/dwc3.c:211: dwc3: [OTG0] GUSB2PHYCFG0=0x40102400
[INFO][C0] driver/usb/dwc3.c:229: dwc3: [OTG0] core soft reset skipped  GCTL=0x30c12004  GSTS=0x7e800000
[INFO][C0] driver/usb/dwc3.c:241: dwc3: [OTG0] GCTL after host mode set: 0x30c11004  mode=Host
[INFO][C0] driver/usb/dwc3.c:252: dwc3: [OTG0] GUCTL=0x02024210
[INFO][C0] driver/usb/dwc3.c:259: dwc3: [OTG0] GUCTL2=0x0000140d
[INFO][C0] driver/usb/dwc3.c:272: dwc3: [OTG0] SUSPHY re-enabled  GUSB2PHYCFG0=0x40102440
[INFO][C0] driver/usb/dwc3.c:313: dwc3: [OTG0] xHCI reset done  USBSTS=0x00000001  halted
[INFO][C0] driver/usb/dwc3.c:319: dwc3: [OTG0] starting controller...
[INFO][C0] driver/usb/dwc3.c:327: dwc3: [OTG0] controller started  USBSTS=0x00000000
```

---

## 阶段 3: xHCI 启动

### 函数: `dwc3_xhci_start()`

### 目的

初始化 xHCI 数据结构并启动控制器，扫描已连接设备。

### 步骤

#### 3.1 VBUS 上电（GPIO3_B7）

```
1. IOMUX 配置: GPIO3_B7 = GPIO function
2. 方向: output
3. 数据: HIGH（使能 VBUS 5V）
4. 等待: ~300ms（VBUS 稳定 + 设备上电）
```

#### 3.2 停止控制器（如果正在运行）

```
USBCMD.RS = 0
等待 HCHalted = 1

检测错误标志:
  - HSE (Host System Error)
  - HCE (Host Controller Error)
  - SRE (Save/Restore Error)

必要时执行完整复位
```

#### 3.3 初始化数据结构

**DCBAA（Device Context Base Address Array）**
```
DCBAA[0]: Scratchpad 指针数组
DCBAA[1..64]: Slot 设备上下文指针（初始为 NULL）
```

**Command Ring**
```
16 个 TRB + 1 个 Link TRB
初始 PCS=1（Producer Cycle State）
CRCR.RCS=1（硬件 Consumer Cycle State）
```

**Event Ring**
```
16 个 TRB，初始 cycle=0（空槽）
ERST: Event Ring Segment Table
ERDP: Event Ring Dequeue Pointer
```

**Scratchpad Buffers**
```
32 × 4KB 页
DCBAA[0] = 指针数组物理地址
```

#### 3.4 配置寄存器

| 寄存器 | 值 | 说明 |
|--------|-----|------|
| DCBAAP_LO/HI | DCBAA 物理地址 | 设备上下文基址 |
| CRCR_LO/HI | Command Ring 指针 \| RCS=1 | 命令环控制 |
| ERSTSZ | 1 | ERST 条目数 |
| ERSTBA_LO/HI | ERST 物理地址 | ERST 基址 |
| ERDP_LO/HI | Event Ring 起始地址 | 初始 dequeue 指针 |
| CONFIG | MaxSlotsEn=2 | 最大插槽数 |
| IMAN | IP=1, IE=1 | 中断管理 |

#### 3.5 启动控制器

```
USBCMD.RS = 1
等待 HCHalted 清零
```

#### 3.6 扫描端口

```
for each port:
  读取 PORTSC
  if CCS=1: 设备已连接，打印速度
  else: 无设备
```

### 输出示例

```
[INFO][C0] driver/usb/dwc3.c:678: dwc3: === xHCI start (device scan) ===
[INFO][C0] driver/usb/dwc3.c:76: dwc3: GPIO3_B7 HIGH — USB Host VBUS 5V enabled
[INFO][C0] driver/usb/dwc3.c:459: dwc3: [OTG0] xhci_start  rt_base=0xffff0000fc000440
[INFO][C0] driver/usb/dwc3.c:534: dwc3: [OTG0] CRCR after init: RCS=0  crcr_lo=0x00000000
[INFO][C0] driver/usb/dwc3.c:572: dwc3: [OTG0] Command Ring initialized: PCS=1  CRCR_written=0x005d5481
[INFO][C0] driver/usb/dwc3.c:619: dwc3: [OTG0] HCSPARAMS1=0x02000140 MaxSlots=64  HCSPARAMS2=0x0c0000f1 MaxScratchpad=32
[INFO][C0] driver/usb/dwc3.c:628: dwc3: [OTG0] MaxSlotsEn set to 2
[INFO][C0] driver/usb/dwc3.c:643: dwc3: [OTG0] scratchpad: 32 bufs  array_phys=0x005d5000
[INFO][C0] driver/usb/dwc3.c:667: dwc3: [OTG0] xHCI running  USBSTS=0x00000000
[INFO][C0] driver/usb/dwc3.c:437: dwc3: [OTG0] port1  no device  PLS=RxDetect  PORTSC=0x000002a0
[INFO][C0] driver/usb/dwc3.c:430: dwc3: [OTG1] port1  *** DEVICE CONNECTED ***  speed=FullSpeed  PLS=Polling
```

---

## 阶段 4: 设备枚举

### 函数: `dwc3_hid_enumerate()`

### 目的

枚举 USB HID 设备并读取报告数据。

### 步骤

#### 4.1 端口发现

```
扫描所有控制器和端口
找到 CCS=1 的端口
```

#### 4.2 端口复位

```
PORTSC.PR = 1
等待 PR 清零且 PED=1
确定设备速度（FS/LS/HS/SS）
```

#### 4.3 Enable Slot

```
发送 Enable Slot Command (TRB type=9)
等待 Command Completion Event
获得 slot_id（1~64）
```

#### 4.4 Address Device（BSR=1）

```
初始化 Input Context:
  - Slot Context: 速度、端口
  - EP0 Context: MaxPacketSize、传输环指针

发送 Address Device Command (BSR=1)
等待 Command Completion Event
```

**BSR=1 模式说明**:
- 将设备设为 Addressed 状态，但不发 SET_ADDRESS
- 设备仍处于 Default 状态（地址 0）
- 后续通过 Control EP 手动发送标准请求

#### 4.5 GET_DESCRIPTOR(Device, 8)

```
Control 传输:
  - SETUP: GET_DESCRIPTOR(Device, 8)
  - DATA: 读取 8 字节
  - STATUS: 等待完成

提取 bMaxPacketSize0
```

#### 4.6 GET_DESCRIPTOR(Device, 18)

```
读取完整设备描述符
提取 VID/PID
```

#### 4.7 GET_DESCRIPTOR(Configuration, wTotalLength)

```
读取完整配置描述符
解析端点描述符
找到 HID Interrupt IN 端点
```

#### 4.8 SET_CONFIGURATION(1)

```
Control 传输: SET_CONFIGURATION(1)
激活配置，使能端点
```

#### 4.9 SET_PROTOCOL(boot=0)

```
HID 类请求: SET_PROTOCOL(0)
设置为 Boot Protocol
```

#### 4.10 Configure Endpoint

```
初始化 Input Context:
  - EP Context: HID Interrupt IN 端点配置

发送 Configure Endpoint Command
等待 Command Completion Event
```

#### 4.11 读取 HID 报告

```
for i = 0 to 49:
  挂起 Normal TRB
  发送门铃
  等待 Transfer Completion Event
  打印鼠标按钮和移动数据
```

### 输出示例

```
[INFO][C0] driver/usb/dwc3_enum.c:706: dwc3_enum: === USB HID enum start ===
[INFO][C0] driver/usb/dwc3_enum.c:744: dwc3_enum: [OTG1] port 1
[INFO][C0] driver/usb/dwc3_enum.c:745:   speed=1 starting enumeration
[INFO][C0] driver/usb/dwc3_enum.c:799: dwc3_enum: port reset...
[INFO][C0] driver/usb/dwc3_enum.c:839: dwc3_enum: PR=0 detected  PORTSC=0x00220e03  PED=1  PLS=0  Speed=3
[INFO][C0] driver/usb/dwc3_enum.c:908: dwc3_enum: port reset complete  speed=HighSpeed  PORTSC=0x00000e03
[INFO][C0] driver/usb/dwc3_enum.c:968: dwc3_enum: slot_id=1
[INFO][C0] driver/usb/dwc3_enum.c:1210: dwc3_enum: BSR=1 wait_cmd rc=0  USBSTS=0x00000018
[INFO][C0] driver/usb/dwc3_enum.c:1232: dwc3_enum: Trying GET_DESCRIPTOR first (device at addr 0)...
[INFO][C0] driver/usb/dwc3_enum.c:1268: dwc3_enum: bcdUSB=0200
[INFO][C0] driver/usb/dwc3_enum.c:1269:   bDevClass=0x00 bMaxPkt0=64
[INFO][C0] driver/usb/dwc3_enum.c:1293: dwc3_enum: VID=0x046d PID=0xc52b
[INFO][C0] driver/usb/dwc3_enum.c:1311: dwc3_enum: Configuration wTotalLength=59 bNumIf=1
[INFO][C0] driver/usb/dwc3_enum.c:1355: dwc3_enum: EP addr=0x81 maxpkt=4
[INFO][C0] driver/usb/dwc3_enum.c:1373: dwc3_enum: EP1 IN configured
[INFO][C0] driver/usb/dwc3_enum.c:1377: dwc3_enum: === Start reading HID reports (50 times) ===
```

---

## TRB Ring 管理

### Producer Ring（命令环 / 传输环）

```
┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐
│ TRB0│ TRB1│ TRB2│ ... │ TRB14│ TRB15│LINK│     │
└─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘
  ↑                                     ↑
  enq=0, PCS=1                         Link TRB
                                        (TC=1, cycle=PCS)
```

**软件操作**:
1. 写入 TRB，设置 cycle bit = PCS
2. 推进 enq 指针
3. 到达 Link TRB 时，更新其 cycle bit = 当前 PCS，然后切换 PCS
4. 发送门铃通知硬件

**硬件操作**:
1. 检查 TRB cycle bit == CCS
2. 执行 TRB
3. 推进 dequeue 指针
4. 遇到 Link TRB 时，跳转到环起始并切换 CCS

### Consumer Ring（事件环）

```
┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐
│ EVT0│ EVT1│ EVT2│ ... │ EVT14│ EVT15│     │     │
└─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘
  ↑    ↑
  deq  deq+1
  CCS=1
```

**硬件操作**:
1. 写入事件 TRB，cycle bit = PCS
2. 更新 ERDP

**软件操作**:
1. Invalidate 缓存，读取 TRB
2. 检查 cycle bit == CCS
3. 处理事件
4. 推进 deq 指针
5. 更新 ERDP
6. 清除 IMAN.IP

### 关键要点

- **Link TRB cycle bit**: 必须与当前 PCS 相同（TC=1 时）
- **Event Ring 初始状态**: cycle=0（空槽），否则 drain 会误消费
- **缓存一致性**: 写入前 clean，读取前 invalidate
- **ERDP 更新**: 必须包含 EHB 位，清除 IMAN.IP

---

## RK3588 特定 Quirks

### 1. SUSPHY 枚举期间禁用

```
原因: PHY 进入 Suspend 后 SET_ADDRESS 可能挂死
操作: 枚举前清除 SUSPHY，完成后恢复
```

### 2. U2_FREECLK_EXISTS 清除

```
原因: RK3588 USB2 PHY 没有自由运行时钟
操作: GUSB2PHYCFG0.bit30 = 0
```

### 3. TX_IPGAP_LINECHECK_DIS 置位

```
原因: HS 枚举必须禁用 TX IP Gap 行检查
操作: GUCTL.bit9 = 1
```

### 4. DIS_DEL_PHY_POWER_CHG 置位

```
原因: 禁用延迟 PHY 功耗变化，防 SET_ADDRESS 挂死
操作: GUCTL2.bit12 = 1
```

### 5. PARKMODE_DISABLE_SS 置位

```
原因: RK3588 OTG1 DTS quirk
操作: GUCTL.bit17 = 1
```

### 6. CRCR 读回值为 0

```
原因: DWC3 v3.00a 在消费第一个 TRB 前始终返回 0
解决: 使用确定的 PCS=1，不依赖读回值
```

---

## 参考资料

- xHCI 规范 v1.2
- DWC_usb3 databook
- Linux drivers/usb/dwc3/core.c
- Linux drivers/usb/host/xhci.c
