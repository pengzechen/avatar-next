# RISC-V64 SG2002 Busybox Bring-up 坑点记录

本文记录 SG2002 上用 ramfs/rootfs 启动 busybox 过程中踩到的关键坑点。目标是保留能解释问题根因的事实，避免后续为了“看起来像 QEMU”而破坏真机路径。

## 1. 平台配置和 rootfs 地址

SG2002 当前使用 ramblk/rootfs 路径，rootfs 物理地址为 `0x89000000`，大小 `0x04000000`。SDMMC 暂时关闭，避免在 busybox bring-up 阶段把问题混到 SD 卡驱动里。

检查点：

```bash
make PLATFORM=sg2002-riscv64 ETH=none build/kernel_riscv64.bin LOG=info -j4
grep -E '^(MEM_PLATFORM_DEFINE|MEM_ROOTFS_BASE|MEM_ROOTFS_SIZE|DEV_TPU_TYPE|DEV_SDMMC_TYPE)' build/platform.mk
```

期望看到 SG2002 平台、rootfs base 为 `0x89000000`，SDMMC 为 `none`。

## 2. SG2002 PTE 高位属性必须只在 SG2002 启用

SG2002/CVITEK/T-Head 页表项需要额外内存属性位：

- Normal memory: `7 << 60`
- IO/remap: `(1 << 63) | (1 << 60)`

这些位已经加在早期 Sv39 1GB 映射和运行时用户页表映射里，但必须由 `PLATFORM_SG2002` 保护。QEMU RISC-V 不需要这些 vendor attr，不能全局打开。

同时，解析 PTE 物理地址时必须屏蔽 PPN 位宽：

```c
((pte >> 10) & ((1ULL << 44) - 1ULL)) << 12
```

否则 SG2002 的高位属性会被误当成 PPN，导致页表遍历或 `mm_vm_get_paddr()` 得到错误物理地址。

## 3. 用户页表必须复制完整内核高半映射

RISC-V trap 路径不切回单独的内核页表，而是在当前 satp 下进入 `stvec`。因此用户页表必须包含内核高半区映射，否则用户态一触发 ecall/中断就可能在 trap 入口失联。

当前至少需要复制：

- `L1[0x100]`: `KERNEL_VMA + 0x00000000..0x3fffffff`，MMIO 高半别名
- `L1[0x101]`: `KERNEL_VMA + 0x40000000..0x7fffffff`，MMIO 高半别名
- `L1[0x102]`: `KERNEL_VMA + 0x80000000..0xbfffffff`，RAM 高半别名

`L1[0x101]` 容易漏掉。SG2002 的部分设备/MMIO 地址落在这个 1GB 区间内，只复制 `0x100` 和 `0x102` 会让 trap 路径中的设备访问出问题。

## 4. RISC-V 首次进入用户态不要在 C 调度层提前切 satp

`sched_schedule()` 不应在 C 层对 RISC-V 提前 `csrw satp`。首次调度到用户进程时，内核仍要读取下一个任务的内核栈、保存前一个任务状态，并执行架构切换代码；过早切到用户 satp 会让这些内核访问依赖用户页表映射，风险很高。

当前策略：

- 普通任务切换由 `arch_task_switch()` 根据传入 pgd 处理。
- 首次用户进程入口由 `arch_switch_to_user()` 在贴近 `sret` 的位置切换 satp。
- x86_64 仍可在调度层按 CR3 逻辑处理，RISC-V 不照搬。

## 5. `sscratch` 必须尽量贴近 `sret` 设置

trap 入口用 `sscratch == 0` 判断是否来自 S-mode，`sscratch != 0` 判断是否来自 U-mode。如果在 `arch_switch_to_user()` 很早就写入用户任务的 kernel stack，然后还停留在 S-mode 执行代码，一旦这段窗口发生 S-mode 异常，会被误判成 U-mode trap。

因此 `csrw sscratch, kernel_sp` 应放在 `fence.i` 后、`sret` 前的最后阶段。

## 6. SG2002/C906 没有 H 扩展，不能访问 `hstatus`

QEMU 或带 H 扩展平台可以清 `hstatus.SPV/SPVP`，防止从 HS trap 后 `sret` 误入 VU-mode。但 SG2002/C906 没有 H 扩展，读写 `hstatus` 会触发 illegal instruction。

规则：

```c
#if !defined(PLATFORM_SG2002)
    /* hstatus access */
#endif
```

不要用“QEMU 能跑”作为 SG2002 可以访问 H CSR 的依据。

## 7. SG2002 用户态返回前要设置 `sstatus` bit 23

SG2002 用户态入口需要在 `sstatus` 中补 bit 23，同时设置 `SPIE`，清 `SPP/SIE`。该位是平台要求，缺失时可能表现为用户态进入异常或行为不稳定。

当前入口逻辑保留：

```assembly
li      t1, ((1 << 23) | 0x20)
or      t0, t0, t1
csrw    sstatus, t0
```

## 8. 用户 satp 下的运行时 MMIO 必须走高半地址

timer trap 会调用 `signal_check_uart()` 等路径。此时 satp 可能仍是用户页表，低物理 MMIO 地址通常不在用户页表中。如果 runtime UART 使用低物理地址，会出现 trap 内 load page fault。

SG2002/QEMU 的 DW UART 基址应由平台宏决定：

```c
DEVICE_UART_BASE_RAW + (DEVICE_MMIO_NEEDS_VMA ? KERNEL_VMA : 0)
```

SG2002 下 `DEVICE_MMIO_NEEDS_VMA=1`，UART 必须使用高半别名。

## 9. raw UART 打点只适合短期定位

`A/S/F/B/C/D`、`U/H/E/Y`、`I05`、`UX08` 这类 raw marker 对定位 first user entry/trap 很有用，但不能长期留在正常路径：

- 会污染串口输出，干扰 busybox 交互。
- 宏内嵌汇编如果使用重复 numeric label，容易在嵌套宏中跳错位置。
- 调试完成后应删除宏和调用，而不是只把宏置空。

## 10. 日志级别约定

busybox 启动路径中，ELF segment、stack dump、PTE walk、syscall entry/return 都属于诊断日志。默认 `LOG=info` 不应打印这些高频细节。

约定：

- 错误和异常保留 `KLOG_ERROR`/`KLOG_WARN`。
- ELF 加载、用户页表映射、进程创建细节使用 `KLOG_DEBUG`。
- syscall 逐条入口/返回日志默认删除，需要时临时加回或改成 DEBUG。

## 11. 验证命令

SG2002 构建：

```bash
make PLATFORM=sg2002-riscv64 ETH=none build/kernel_riscv64.bin LOG=info -j4
cp build/kernel_riscv64.bin imgs/kernel_riscv64.bin
sha256sum imgs/kernel_riscv64.bin
```

QEMU RISC-V 回归：

```bash
make ARCH=riscv64 build/kernel_riscv64.bin LOG=info -j4
```

QEMU 回归尤其重要：SG2002 的 PTE attr、H CSR guard、平台宏导出都必须不能破坏普通 QEMU RISC-V。