# RISC-V ecall 后 CPU 进入 OpenSBI 的问题修复

## 问题现象

用户进程在 RISC-V 64 上执行 `ecall` 后，CPU 没有进入内核 trap handler，而是跑到了 OpenSBI（`PC = 0x80003a14`），GDB 单步调试显示 CPU 在 OpenSBI 地址范围（`0x80000000–0x80200000`）内循环。

```
(gdb) b *0x10000
Breakpoint 1: hit
(gdb) si   # 单步到 ecall 指令
(gdb) si   # → GDB 卡住，Ctrl+C 后：
0x0000000080003a14 in ?? ()   # OpenSBI!
```

## 根本原因

**用户 PGD 破坏了内核的 UART MMIO 映射。**

### 问题链路

1. **RISC-V 内核页表（`rv_l2_root`）** 中 L2[0] 是一个 1GB giga-page leaf，覆盖 VA `0x00000000–0x3FFFFFFF → phys 0x00000000–0x3FFFFFFF`（恒等映射）。UART 物理地址 `0x10000000` 通过这条映射访问。

2. **`device_profile.table` 中 `riscv64/qemu` 的 `mmio_vma=0`**，导致 `DEVICE_MMIO_ADDR(raw) = raw`，即 C 驱动中 `UART_BASE = 0x10000000`（裸物理地址），依赖恒等映射。

3. **`vm_create_user_process`** 在创建用户 PGD 时，调用 `mm_vm_map_pages(pgd, 0x10000, ...)` 将用户代码映射到 VA `0x10000`（Sv39 VPN[2]=0）。这会在新 PGD 的 L2[0] 位置写入一个 **L1 页表指针**，覆盖掉原来的 1GB giga-page leaf。

4. **`process_create`** 把内核 PGD 的 L2[0x100] 和 L2[0x102] 复制到用户 PGD（覆盖内核高半区别名），但没有复制 L2[0] 和 L2[2]（低恒等映射）。

5. 用户进程切换到该 PGD 后执行 `ecall`，CPU 跳到 `stvec`（`0xffffffc08020fca0`，在 L2[0x102] 范围内，映射正常）。trap handler 随即调用 `KLOG_INFO` → 访问 `UART_BASE = 0x10000000`（VA 在 L2[0] 范围）→ **L2[0] 是 L1 页表指针，0x10000000 未在该 L1 表中映射** → load page fault。

6. 嵌套异常（trap handler 内部 page fault）无法被 S 模式再处理 → **委托升级到 M 模式 → OpenSBI**。

### 为什么 AArch64 不受影响

AArch64 的 `mmio_vma=1`，UART 通过内核别名 VA（KERNEL_VMA + 物理地址）访问，映射在 TTBR1 高半区，用户 PGD 切换不影响内核 MMIO 映射。

## 修复方案

将 `riscv64/qemu` 的 `mmio_vma` 从 `0` 改为 `1`，使所有 C 驱动通过内核别名 VA 访问 MMIO：

```
UART_BASE = KERNEL_VMA + 0x10000000 = 0xffffffc010000000
```

该 VA 的 Sv39 VPN[2] = `0x100`，对应 L2[0x100]（已在用户 PGD 中复制），映射正常。

### 修改的文件

**`config/device_profile.table`**（源配置）：
```diff
-riscv64 qemu      1        dw     none  rv       0         0x10000000UL ...
+riscv64 qemu      1        dw     none  rv       1         0x10000000UL ...
```

**`include/device_profile.h`**（自动生成，同步修改）：
```diff
 #elif ARCH_RISCV64 && PLATFORM_QEMU
 #define DEVICE_PROFILE_VALID          1
-#define DEVICE_MMIO_NEEDS_VMA         0
+#define DEVICE_MMIO_NEEDS_VMA         1
```

## 验证结果

修复后 QEMU 输出：

```
[INFO]  [user-entry] trampoline: entry=0x10000 ...
[INFO]  [trap] exc code=8 from_user sepc=0x1000e
[INFO]  [syscall] pid=2 nr=20 args=[0x10074,0x22,...]
[rv-user] Hello from RISC-V user!
[INFO]  [trap] exc code=8 from_user sepc=0x10014
[INFO]  [syscall] pid=2 nr=2 ...
[rv-user] PID: 2
[rv-user] Exiting...
[INFO]  [syscall] process 'rv_user_test' (id=2) exiting with status 0
```

ecall → sys_write → sys_getpid → sys_exit 全部走通。

## 涉及架构说明

| 字段 | AArch64/QEMU | RISC-V64/QEMU（修复前） | RISC-V64/QEMU（修复后） |
|------|-------------|----------------------|----------------------|
| `mmio_vma` | 1 | 0 | **1** |
| `UART_BASE` | `KERNEL_VMA + 0x09000000` | `0x10000000`（裸物理）| `KERNEL_VMA + 0x10000000` |
| 用户进程 UART 访问 | 正常（高半区）| **崩溃** | 正常（高半区）|

## 经验教训

1. 创建用户 PGD 时，凡是内核 C 代码运行期间会访问的 MMIO，必须确保映射通过内核高半区（`KERNEL_VMA` 偏移后的 VA），而不能依赖低地址恒等映射——因为用户 PGD 会覆盖低地址的 L2 条目。

2. `mmio_vma` 应统一设为 `1`（通过内核别名访问），除非有明确理由使用裸物理地址（如 MMU 开启前的 boot 代码，那部分用硬编码汇编访问，不走 C 宏）。
