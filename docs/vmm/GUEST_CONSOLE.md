# Guest 控制台：两种运行模式与 /dev/vmm

> 最后更新：2026-09-24
> 相关代码：`kernel/vmm/vdev/vpl011.c`、`kernel/fs/pseudofs/vmm_dev.c`、
> `apps/c/vmm_run.c`、`kernel/vmm/vmm.c`

## 1. 两种模式

同一套 vpl011 设备支持两种完全不同的控制台归属方式，**由宿主侧决定**，
guest 侧看不出来：

| | 直启模式 | helper 模式 |
|---|---|---|
| 怎么进 | `make PLATFORM=qemu-virt-aarch64 GIC=v3 test-guest-linux` | `make PLATFORM=qemu-virt-aarch64 GIC=v3 run-fs`，然后在 shell 里敲 `/bin/vmm-run` |
| x86_64 对应命令 | `make PLATFORM=qemu-virt-x86_64 GUEST_LINUX=1 test-guest-linux` | `make PLATFORM=qemu-virt-x86_64 run-fs`，然后在 shell 里敲 `/bin/vmm-run` |
| 宿主 shell | 没有（`pass:[RUN_GUEST_LINUX]` 与 busybox 互斥） | 有，guest 停掉后立刻可用 |
| guest 输入从哪来 | `vmm_console_pump()` 在 vCPU 退出路径直接轮询真实 PL011 | 用户态 helper 读自己的 stdin 再 write 到 `/dev/vmm` |
| guest 输出到哪去 | 逐字节直打宿主控制台 | 进 vpl011 的 TX 环形缓冲，helper 读走再写自己的 stdout |
| 怎么停 guest | 只能等 guest 自己 poweroff | Ctrl+] → helper 退出 → 内核停止 guest |

## 2. 为什么需要「控制台归属」这件事

宿主只有一个真实 PL011，而有**两个**可能的消费者：

1. 宿主 tty 层 —— `signal_check_uart()`（`kernel/syscall/fs/tty.c`）在
   **每个 tick 的定时器 ISR 里**就把硬件 FIFO 抽干进 `g_uart_rb`；
2. `vmm_console_pump()`（`kernel/vmm/vmm.c`）。

这两条路**没有任何仲裁**，谁先跑谁拿到字节。所以只要 helper 在场，
就必须把 (2) 关掉，否则用户按键会被随机分给两条路。

判据是 `vpl011_tx_channel_enabled()`：`/dev/vmm` 在启动 guest 前把 TX 通道
打开，`vmm_console_pump()` 见到通道打开就直接 return。反过来，直启模式下
没人开通道，泵照常工作。

> ⚠️ `vpl011_init()` 里那句 `memset(&g_vpl011, 0, ...)` **必须保留 tx_channel**
> （见该函数注释）。它是宿主侧的模式选择，不是设备寄存器状态；清掉它会让
> 通道模式失效、泵复活，症状是 helper 收不到任何按键 —— 包括 Ctrl+]。

## 3. `/dev/vmm` 协议

对标 x-kernel 的 `/dev/kvmm-vm`（`virt/kvmm-api/src/device.rs`），但控制面
做了改动（见下方「与 kvmm 的偏离」）。

```
open("/dev/vmm", O_RDWR)
ioctl(fd, VMM_IOC_BOOT)       启动 guest；已经有在跑的就接入它
write(fd, bytes, n)           **永远**是 guest 的串口输入
read(fd, buf, n)              guest 的串口输出；无数据返回 -EAGAIN
poll(fd)                      TX 有数据 → EPOLLIN；guest 已退出 → EPOLLHUP
ioctl(fd, VMM_IOC_GET_STATUS) 出参 1 = 有 guest 在跑
ioctl(fd, VMM_IOC_DETACH)     分离：本次 close 不停 guest（Ctrl+[）
ioctl(fd, VMM_IOC_STOP)       停止 guest（Ctrl+]）
close(fd)                     默认停止 guest；DETACH 过则不停止
```

ioctl 号定义在 `include/pseudofs.h`（内核侧）与 `apps/c/vmm_run.c`（用户侧，
用 musl 的 `<sys/ioctl.h>` 宏独立写了一遍；两边都是标准 `_IOC` 编码）。

guest 镜像路径是写死的 `/guests/linux/{linux.bin,linux.dtb,initrd.gz}`，
不像 kvmm 那样由命令行参数带进来。

### 与 kvmm 的偏离：控制全部走 ioctl，没有「首次写是命令」

kvmm 的 `/dev/kvmm-vm` 用「第一次 write 是 `bootlinux` 命令，之后是数据」，
因为它只有一个 chardev、没有 ioctl。**这个约定在「接入」场景下有二义性**，
实测踩过：重新接入一个已在跑的 guest 时，设备的 `booted` 标志已经是 1，
helper 发的 `bootlinux` 会被当成输入推进 guest，guest 把它回显出来。

所以这里所有控制都走 ioctl，`write()` 永远只是数据。代价是丢了与 kvmm
的一点点形似，换来的是不存在「这次写到底是命令还是数据」这个判断。

`VMM_IOC_BOOT` 同时承担启动和接入：内核按「有没有在跑的 guest」决定新建
还是接入，于是用户只需要记住一条命令 `vmm-run`。

## 3a. 分离与重新接入

| 按键 | 语义 |
|---|---|
| Ctrl+] `0x1d` | 停止 guest，回宿主 shell（destroy） |
| Ctrl+[ `0x1b` | guest 留在后台继续跑，回宿主 shell（detach） |

重新接入就是再跑一次 `vmm-run`。`vmm-run -k` 可以不接管控制台、直接停掉
后台的 guest。

分离期间：vpl011 的 TX 通道**保持打开**（关了 `vmm_console_pump()` 就会复活
跟宿主 shell 抢 UART），RX FIFO 也不清（那是给当前这个 guest 的）。guest
的输出继续进 TX 环形缓冲，最多攒 8 KiB，接入时一起吐出来（类似 `screen -r`）。
超出就丢（`put_char_locked` 丢最新字节）。

`close` 默认仍然停 guest，只有 `VMM_IOC_DETACH` 过才不停 —— 这条兜底是为了
helper 被 `kill -9` 或崩溃时不要把 guest 无声地留在后台烧 CPU。

### ⚠️ Ctrl+[ 就是 ESC，必须消歧

方向键 / Home / End / Fn 发出的都是以 `0x1b` 开头的转义序列（上箭头 =
`\x1b[A`），而宿主内核在 raw mode 下的 `read()` **每次只返回 1 字节**
（`read_handler` 的 raw 分支），所以 `\x1b[A` 会拆成三次到达 —— 见到 `0x1b`
就分离的话，guest 里的方向键会全部失效。

`vmm_run.c` 的 `esc_is_standalone()` 用一次 ~40ms 的 `poll` 消歧：有后续
字节就是转义序列，`0x1b` 照常转发；没有才是单独的 Ctrl+[。正常敲方向键时
后续字节几乎同时到达，感知不到延迟。`poll` 出错时按「不是单独 ESC」处理 ——
宁可把字节转给 guest，也不要因为一次信号打断就把用户踢出 guest。

实测：`\033[A\033[B` 被原样转发并由 guest 回显，没有触发分离。

## 4. 停止是异步的

`close()` 只置 `g_vmm_stop_requested` 就返回。vCPU 任务在**下一个安全点**
（`vmm_run_vcpu` 循环顶部：上一次 guest 已退出、`HCR_EL2` 已被
`el2_trap_exit` 还原成 host 模式）看到标志后返回并 `task_exit()`。

延迟上限取决于 guest 在干什么：
- guest 空闲（getty 等输入）：每条 WFI 都陷入 EL2，几乎立刻；
- guest 满载（长时间不陷入的忙循环）：宿主定时器每 10ms 踹它一次。

所以 `VMM_IOC_BOOT` 在旧 guest 还没收完尾时会失败（`guest_loader_run_linux`
的重入保护会拒绝），`vmm-run` 用 1ms 间隔重试最多 2000 次。内核侧不做任何
定时等待。

## 5. vCPU 任务必须主动让出 CPU

`vmm_run_vcpu` 循环里那句 `sched_check_and_yield()` **不是可选的**。

guest 退出走的是 VMM 自己的 `guest_vec_table`（`el2_vmcs.S` 在进 guest 前
装到 `VBAR_EL2`），**不经过** `sched_check_and_yield_from_trap()` —— 那个
钩子挂在宿主正常异常向量表的返回路径上。少了这个调用，vCPU 任务在循环里
永远不进调度器，宿主其它任务被完全饿死。

实测症状（值得记住，因为看起来完全不像调度问题）：宿主 shell 里跑
`/bin/vmm-run`，内核日志显示 `bootlinux: starting guest`、guest 正常启动，
但 helper 连自己的 banner 都打不出来，之后再无任何输出 —— 因为它再也抢不到
cpu0。

## 6. 怎么验证

### x86_64（2026-09-26 接入）

```bash
make PLATFORM=qemu-virt-x86_64 LOG=warn SMP=1 run-fs
# 宿主 shell 起来后直接 /bin/vmm-run —— 四条语义都实测过：
#   启动 guest ✓ / 输入输出透传 ✓ / Ctrl+] 停止后**再启一次**（重入）✓ /
#   Ctrl+[ 分离 → 宿主 shell 可用 → vmm-run 重新接入 ✓
```

x86 侧曾经只差 Makefile 里一行：`VMM_RUN_BIN` 的架构过滤写着
`filter $(ARCH),aarch64 riscv64`，把 x86_64 排除了（helper 本身是纯 C + ioctl，
`apps/c/vmm_run.c` 三架构共用）。另外**重入**还需要一处修复：VMXON 是"每 CPU
一次"的，而原来全代码没有任何 VMXOFF，第二次启动会卡在
`[VMX] VMXON failed` → `vm_create failed`（看起来像内存/EPT 问题），
现在 `vmx_global_init()` 里用一次性标志挡住（见该函数注释）。

### aarch64

**GICv2 与 GICv3 都支持**，下面两条命令任选。切 GIC 版本不必手动 `clean`：
Makefile §7 的 `_CFG_CHECK` 检测到配置变化（GIC=/SMP=/LOG=）会自动清掉已编译的目标文件
（`GIC=` 会改 CFLAGS 和源文件列表，不清理就会混用两套 flag 编出来的 `.o`）。

```bash
# GICv3（显式指定；构建树里同时只能用一套 vGIC 后端）
make PLATFORM=qemu-virt-aarch64 GIC=v3 clean
make PLATFORM=qemu-virt-aarch64 GIC=v3 run-fs

# GICv2（qemu-virt-aarch64 的设备画像默认值，GIC= 可以省略）
make PLATFORM=qemu-virt-aarch64 clean
make PLATFORM=qemu-virt-aarch64 run-fs
```

> 两条路的差别只在中断投递后端：GICv3 由 GIC 硬件经 `ICH_LR<n>_EL2` 服务
> 虚拟中断，GICv2 的 GICC MMIO 被 Stage-2 陷入、由 VMM 软件代答。vpl011
> 的 RX 中断都是 SPI 33，两条路实测都能送到 guest（判据同样是登录名能回显）。
> 切换 GIC 版本会换 guest DTB（`linux.dtb` ↔ `linux-gicv3.dtb`），
> `$(GUEST_GIC_STAMP)` 负责让 rootfs 跟着重建。

```bash
# 宿主 shell 起来后：
#   / # vmm-run
#   [vmm-run] guest started — Ctrl+] stop, Ctrl+[ detach
#   ... guest 启动日志 ...
#   ~ # （能逐字回显，说明输入通了）
#   ^]            ← Ctrl+]
#   [vmm-run] guest stopping
#   / #           ← 回到宿主 shell，立刻可用
```

**分离 / 接入**：

```
#   / # vmm-run          （guest 已在跑就接入，否则新建）
#   [vmm-run] attached to running guest — Ctrl+] stop, Ctrl+[ detach
#   ^[                   ← Ctrl+[：guest 留后台
#   [vmm-run] detached — guest keeps running; run vmm-run to reattach
#   / # echo alive       （宿主 shell 立刻可用，guest 仍在跑）
#   alive
#   / # vmm-run          ← 接回来
#   / # vmm-run -k       ← 或者直接停掉后台的 guest
```

再跑一次 `vmm-run` 应当能重新启动（重入是支持的：guest RAM 每次重新
memset + 重新加载镜像 + `vm_create()` 重建 Stage-2 与 vGIC/vPL011）。

反向验证直启模式没被破坏：`make PLATFORM=qemu-virt-aarch64 test-guest-linux`
（不指定 `GIC=` 时走设备画像默认的 v2），应照旧直接进 `~ #` shell 并能
逐字回显输入 —— 那条路走 `vmm_console_pump`，不经过 helper。

**已验证的组合**（2026-09-24，均为 0 个 `[ERROR]`）：

| 配置 | shell + vmm-run（含分离/接入/`-k`） | 直启 test-guest-linux |
|---|---|---|
| `GIC=v2`（默认） | ✅ | ✅ |
| `GIC=v3` | ✅ | ✅ |
