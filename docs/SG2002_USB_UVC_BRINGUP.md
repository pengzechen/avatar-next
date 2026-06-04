# SG2002 USB/UVC Bring-up 记录

本文记录 SG2002 上 DWC2 USB Host 与 UVC 摄像头 bring-up 过程中已经确认的关键事实和坑点。

## 构建命令

SG2002 USB/UVC 调试使用：

```bash
make PLATFORM=sg2002-riscv64 ETH=none build/kernel_riscv64.bin LOG=debug -j4
cp build/kernel_riscv64.bin imgs/kernel_riscv64.bin
```

`ETH=none` 用于避开无关的 Rust ETH float ABI 链接问题。

## U-Boot 启动

```text
fatload mmc 0:1 0x89000000 rootfs.img ; fatload mmc 0:1 0x80200000 kernel_riscv64.bin ; go 0x80200000
```

## 已验证的 USB 枚举路径

已确认的成功路径：

1. DWC2 init 完成，SG2002 root port 显示 `CONNSTS=1 SPD=FS`。
2. 默认地址阶段直接发 `GET_DESCRIPTOR(Device, 18) @ MPS=64`，不要先发 `GET_DESCRIPTOR(8) @ MPS=8`。
3. `SET_ADDRESS(1)` 成功。
4. 读取完整 configuration descriptor。
5. 解析到 UVC VideoControl / VideoStreaming 接口。
6. `SET_CONFIGURATION(1)` 成功。

已验证摄像头示例：

```text
VID=0x32e6 PID=0xd412 class=239 MPS=64 SPD=FS
Config desc: total=399 interfaces=4 cfg=1 attr=0x80 max_power=50
UVC selected: vc_if=0 vs_if=1 alt=1 ep=2 Isoch mps_raw=0x0370 fmt=1 frame=1 160x120 interval=333333 mjpeg=1
```

## 坑 1：默认地址 8 字节探测会 STALL

现象：

```text
GET_DESCRIPTOR(8) @addr=0 MPS=8
8B SETUP rc=0
8B DATA rc=-2 HCINT=0x0000000a
```

`HCINT=0x0a` 包含 `CHHLTD|STALL`。该设备在当前 DWC2/PHY 组合下会拒绝这种 8 字节探测路径。

修复：对齐 `sg200x-bsp`，默认地址阶段直接 `GET_DESCRIPTOR(Device, 18)`，通道 MPS 固定 64，再从完整设备描述符解析 `bMaxPacketSize0`。

## 坑 2：SG2002/C906 DMA cache 维护必须是真操作

DWC2 使用 internal DMA，EP0 setup/data buffer 必须做真实 cache clean/invalidate。SG2002 的 C906 需要 T-Head 私有 cache 指令：

- `dcache.cva`
- `dcache.iva`
- `dcache.ciall`

RISC-V fallback 只有 `fence` 或空操作时，USB DMA 一致性不可靠。

## 坑 3：`GDFIFOCFG` 只改 `EPINFOBASE`

DWC2 FIFO 配置时，对齐 BSP/Linux 做法：保留 `GDFIFOCFG` 低 16 位，只更新高 16 位 `EPINFOBASE`。

成功日志示例：

```text
GDFIFOCFG=0x05380c40
```

## 坑 4：不要把 USB/UVC 大对象放在 Lua C 调用栈

现象：UVC 配置描述符解析和 `SET_CONFIGURATION` 都成功，最后打印：

```text
=== Enumerated: addr=1 ... UVC=1 ===
```

随后立即触发 RISC-V 异常：

```text
[exception] sync: code=1 pc=0x0 stval=0x0 ...
```

根因：`lua_dwc2_usb_init()` 中原先使用局部变量 `usb_enumerate_result_t r`。加入 UVC 字段后，`usb_enumerate_result_t` 会随 `USB_MAX_DEVICES=16` 放大，在 Lua C 调用链上增加栈压力，表现为返回路径被破坏，最终跳到 `pc=0`。

修复：改用静态存储：

```c
static usb_enumerate_result_t g_dwc2_usb_enum_result;
```

成功验证日志：

```text
[USB] Lua binding: enumeration OK devices=1 first_uvc=1
Lua platform phases (earlycon/irqcore/drivers) complete
```

后续规则：USB/UVC 枚举结果、DMA 缓冲、帧缓冲都放静态/全局区域，不放 Lua C 调用栈或普通内核深调用栈。

## 下一阶段

接下来的实现顺序：

1. UVC `PROBE` / `GET_CUR` / `COMMIT` 协商。
2. `SET_INTERFACE(vs_if, alt)` 打开 VideoStreaming alt setting。
3. DWC2 Isoch IN 单微帧读取。
4. 解析 UVC payload header，按 FID/EOF 组装 MJPEG 帧。