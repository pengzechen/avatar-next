# ARCH + PLATFORM 配置指南（内存与设备单一真源）

本文档说明 Avatar OS 当前的“架构 + 平台”配置模型，以及如何在未来接入真机平台。

## 1. 目标

本次改造将原先分散在多个文件中的硬编码配置，统一为“表驱动 + 自动生成”的模式：

- 内存布局：单一真源
- 设备画像：单一真源
- C 代码与 Makefile 共享同一配置来源
- 支持 `ARCH + PLATFORM` 两级选择，而非仅 `ARCH`

这样做的主要收益：

- 避免地址和设备参数在多个文件漂移
- 为同一架构接入多个平台（QEMU + 真机）预留清晰扩展点
- 降低移植和回归成本

## 2. 配置总览

### 2.1 内存配置（单一真源）

- 源表：`config/mem_layout.table`
- 生成脚本：`tools/gen_mem_layout.sh`
- 生成文件：
  - `include/mem_layout.h`（C 侧）
  - `build/mem_layout.mk`（Makefile 侧）

内存相关使用点：

- PMM：`include/pmm.h` -> `MEM_RAM_BASE/MEM_RAM_SIZE`
- RAMBLK/rootfs 保留区：`driver/blk/ramblk_cfg.h` -> `MEM_ROOTFS_BASE/MEM_ROOTFS_SIZE`

### 2.2 设备配置（单一真源）

- 源表：`config/device_profile.table`
- 生成脚本：`tools/gen_device_profile.sh`
- 生成文件：
  - `include/device_profile.h`（C 侧）
  - `build/device_profile.mk`（Makefile 侧）

设备相关使用点：

- 设备默认选择与 MMIO 地址：`driver/driver_cfg.h`
- 驱动源码选择：`Makefile` 中 `DEV_UART_SRC/DEV_IRQ_SRC/DEV_TIMER_SRC`

## 3. Makefile 选择逻辑

### 3.1 入口参数

- `ARCH ?= aarch64`
- `PLATFORM ?=`（为空时，自动回落到该 ARCH 的默认平台）

### 3.2 解析流程

1. 先生成并加载 `build/mem_layout.mk`
2. 根据内存表规范化 `PLATFORM`
3. 再生成并加载 `build/device_profile.mk`
4. 用设备画像驱动默认 UART/GIC/Timer 与驱动对象列表

### 3.3 覆盖能力

可在命令行覆盖设备默认值：

- `UART=pl011|dw|x86`
- `GIC=v2|v3|none`

并带有基本合法性检查（例如非 aarch64 禁止 GIC）。

## 4. QEMU loader addr 报错说明

### 4.1 问题

QEMU 的 `-device loader,addr=...` 不接受 C 字面量后缀（如 `UL`）。

错误示例：

- `addr=0x60000000UL` -> 报参数格式错误

### 4.2 修复

`tools/gen_mem_layout.sh` 在输出 Makefile 数值时会去掉 `U/L` 后缀。

因此：

- C 头文件可保留 `UL`（类型安全）
- Makefile 里用于 QEMU 参数的是纯数字（兼容 QEMU）

## 5. 新增真机平台的方法

以新增 `aarch64 + rk3588` 为例：

1. 在 `config/mem_layout.table` 增加内存布局行
2. 在 `config/device_profile.table` 增加设备画像行
3. 新增平台目录与入口文件：`platforms/rk3588/platform.c`
4. 编译验证：

```bash
make ARCH=aarch64 PLATFORM=rk3588 kernel
```

如需运行目标（非 QEMU）可新增对应 run 逻辑或独立脚本。

## 6. 常用命令

```bash
# 默认平台（由表中 default=1 决定）
make ARCH=aarch64 kernel

# 显式平台
make ARCH=aarch64 PLATFORM=qemu kernel

# 覆盖设备选择
make ARCH=aarch64 PLATFORM=qemu UART=dw GIC=v3 kernel

# 带 rootfs 启动
make ARCH=aarch64 PLATFORM=qemu run-fs
```

## 7. 维护建议

- 配置只改 table，不直接改生成文件
- 将 `include/*.h` 与 `build/*.mk` 视为生成产物
- 新平台接入时，优先补齐 table，再补平台代码
- 每次改 table 后，至少做三架构编译回归

---

如需进一步统一，可把 QEMU 启动参数（`QEMU`/`QEMU_FLAGS`）也迁移为表驱动，形成与内存/设备一致的单一真源体系。
