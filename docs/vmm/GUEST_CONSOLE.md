# Guest 控制台：两种运行模式与 /dev/vmm

> 最后更新：2026-09-24
> 相关代码：`kernel/vmm/vdev/vpl011.c`、`kernel/fs/pseudofs/vmm_dev.c`、
> `apps/c/vmm_run.c`、`kernel/vmm/vmm_console.c`

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
| 怎么停 guest | 只能等 guest 自己 poweroff | Ctrl+T k → helper 退出 → 内核停止 guest |

## 2. 为什么需要「控制台归属」这件事

宿主只有一个真实 PL011，而有**两个**可能的消费者：

1. 宿主 tty 层 —— `signal_check_uart()`（`kernel/syscall/fs/tty.c`）在
   **每个 tick 的定时器 ISR 里**就把硬件 FIFO 抽干进 `g_uart_rb`；
2. `vmm_console_pump()`（`kernel/vmm/vmm_console.c`）。

这两条路**没有任何仲裁**，谁先跑谁拿到字节。所以只要 helper 在场，
就必须把 (2) 关掉，否则用户按键会被随机分给两条路。

判据是 `vpl011_tx_channel_enabled()`：`/dev/vmm` 在启动 guest 前把 TX 通道
打开，`vmm_console_pump()` 见到通道打开就直接 return。反过来，直启模式下
没人开通道，泵照常工作。

> ⚠️ **这条陷阱已经消除，但要知道它当初为什么存在。**
>
> 归属标志从前叫设备 state 里的 `tx_channel`，而 `vpl011_init()` 会整片
> `memset` 掉 state —— 于是必须在 init 前先存、init 后恢复，否则通道模式
> 失效、泵复活，症状是 helper 收不到任何按键（包括 Ctrl+T 前缀）。
>
> 现在它是 `vm_t.console_owned`：**不在设备状态里**，`*_init()` 的 memset
> 碰不到它，所以那套"先存后恢复"连同陷阱一起消失了。
>
> 设备状态后来又按 `vm->slot` 搬进了 `kernel/vmm/vdev/console/vpl011.c` 的
> 静态池 —— 归属标志仍然留在 `vm_t`（它是宿主侧策略，不是设备寄存器状态），
> 所以这条结论不变。见 vmm.h 里该字段的注释。

## 3. `/dev/vmm` 协议

对标 x-kernel 的 `/dev/kvmm-vm`（`virt/kvmm-api/src/device.rs`），但控制面
做了改动（见下方「与 kvmm 的偏离」）。

```
open("/dev/vmm", O_RDWR)
ioctl(fd, VMM_IOC_BOOT)       启动 guest；已经有在跑的就接入它
ioctl(fd, VMM_IOC_BOOT_EX)    强制新建一个 VM，出参回填 vmid
ioctl(fd, VMM_IOC_LIST)       出参：所有 VM 的 {vmid,state} + 前台 vmid
ioctl(fd, VMM_IOC_ATTACH)     入参 vmid：把它设为前台（不必重新 boot）
ioctl(fd, VMM_IOC_STOP_VM)    入参 vmid：停指定的那个，**不要求先接入**
write(fd, bytes, n)           **永远**是 guest 的串口输入
read(fd, buf, n)              guest 的串口输出；无数据返回 -EAGAIN
poll(fd)                      TX 有数据 → EPOLLIN；guest 已退出 → EPOLLHUP
ioctl(fd, VMM_IOC_GET_STATUS) 出参 1 = 有 guest 在跑
ioctl(fd, VMM_IOC_DETACH)     分离：本次 close 不停 guest（Ctrl+T d）
ioctl(fd, VMM_IOC_STOP)       停止前台 guest（Ctrl+T k）
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

前缀键是 **`Ctrl+T`（0x14）**，按完再按一个键：

| 按键 | 语义 |
|---|---|
| `Ctrl+T d` | detach：guest 留在后台继续跑，回宿主 shell |
| `Ctrl+T k` | 停止当前前台 guest 并退出 helper（destroy） |
| `Ctrl+T l` | 列出所有 VM（带序号，供 `Ctrl+T <n>` 用） |
| `Ctrl+T n` | 新建一个 VM 并切过去 |
| `Ctrl+T 1..8` | 切到 `-l` 列表里的第 n 个 VM（`VMM_IOC_ATTACH`，不重新 boot） |
| `Ctrl+T ?` | 帮助 |
| `Ctrl+T Ctrl+T` | 把 `Ctrl+T` 本身发给 guest |

宿主 shell 里另有四个开关：

```bash
vmm-run                启动 guest；已有在跑的就接入它
vmm-run -a <vmid>      接入指定的那个 VM（不新建）
vmm-run -n             强制新建一个 VM
vmm-run -l             列出所有 VM，不接管控制台
vmm-run -k [vmid]      停止 VM，不接管控制台；不带 vmid 停全部
```

`-l` 的输出（`*` 是当前前台，第 1 列是给 `Ctrl+T <n>` 用的序号）：

```
[vmm-run]  #   vmid  state      fg
           1     1  RUNNING     *
           2     2  RUNNING
```

> ⚠️ **序号（第 1 列）和 vmid 是两套编号，别混。** vmid 从 1 自增、到 255
> 回绕（`vm.c` 的 `g_next_vmid`），长会话里 vm9 后面接的是 vm10 而不是
> 「第 10 个」；序号就是列表里的位置。`-a` / `-k` 收 vmid，`Ctrl+T <n>` 收序号。

### 为什么要有 `-l` / `-a` / `-k` 这一组

在此之前，**要停掉某个 VM 必须先接入它** —— 只有 `VMM_IOC_STOP`，而它打在
「前台」那一个上。更别扭的是接入路径只挑最小 vmid：vm1/vm2 同时在跑时，
`vmm-run` 永远接到 vm1，你想接回的 vm2 反而碰不到（`g_vmm_fg_vmid` 明明
记着 vm2，却没人看它）。现在：

- 接入优先「接回你上次离开的那个」（screen -r 语义），接不到才退回扫最小 vmid；
- `-a <vmid>` 显式指定；
- `-k <vmid>` 直接从宿主 shell 停掉它，**全程不用接入**。

分离期间：vpl011 的 TX 通道**保持打开**（关了 `vmm_console_pump()` 就会复活
跟宿主 shell 抢 UART），RX FIFO 也不清（那是给当前这个 guest 的）。guest
的输出继续进 TX 环形缓冲，最多攒 8 KiB，接入时一起吐出来（类似 `screen -r`）。
超出就丢（`put_char_locked` 丢最新字节）。

`close` 默认仍然停 guest，只有 `VMM_IOC_DETACH` 过才不停 —— 这条兜底是为了
helper 被 `kill -9` 或崩溃时不要把 guest 无声地留在后台烧 CPU。

### ⚠️ 会话 owner：纯查询不能有副作用

上面那条兜底有个要命的副作用：`/dev/vmm` 是**全局单例**设备，于是任何一个
进程 open 一下再 close（比如 `vmm-run -l` 这种纯查询）都会走到「停掉前台
VM」那个分支 —— **一个查列表的动作会把正在用的 guest 杀掉**。同理
`vmm-run -k <非前台vmid>` 停完目标之后，close 还会顺手把前台也停掉。

所以 `vmm_dev.c` 里加了 `g_vmm_owner_pid`：只有做过 BOOT / BOOT_EX / ATTACH
的那个任务（本次会话的主人）才有资格在 close 里停 VM、才有资格消费
`detached` 标志。`-l` / `-k` 从不 BOOT，于是天然退化成只读（`-k` 的停止走
的是显式 ioctl，不依赖 close 兜底）。

存 pid 而不是 `task_t *`：任务退出后指针会被复用。helper 被 `kill -9` 仍然
兜得住 —— `sys_exit` 会关闭它所有 fd，那一刻 `task_current()` 还是它自己。

### ⚠️ 同一时刻只支持一个交互式 helper

前台 VM 是内核里的一个全局变量（`g_vmm_fg_vmid`），所以 B 一 attach，A 那个
终端的读写就变成了 B 的 VM —— A 的窗口会不知不觉变成另一个 guest 的窗口。
`-a` 的语义是「切走前台」，不是「再开一个会话」。

### 为什么是 Ctrl+T（以及 ESC 消歧为什么消失了）

从前的键是 `Ctrl+]`（停）和 `Ctrl+[`（分离），两个都换掉了：

- `Ctrl+]` 是 **telnet** 的转义键；
- `Ctrl+[` 就是 **ESC(0x1b)** —— 方向键 / Home / End / Fn 发出的正是以
  `0x1b` 开头的转义序列（上箭头 = `\x1b[A`），而宿主内核在 raw mode 下的
  `read()` **每次只返回 1 字节**（`read_handler` 的 raw 分支），所以
  `\x1b[A` 会拆成三次到达 —— 见到 `0x1b` 就分离的话，guest 里的方向键会
  全部失效。当时靠一个 ~40ms 的 `poll` 窗口消歧（`esc_is_standalone()`：
  有后续字节就当转义序列，没有才是单独按下的 ESC），能用但很 hack。

换成「前缀 + 单键命令」（screen / tmux / QEMU 都是这个路子）之后，**那个
消歧窗口连同 `esc_is_standalone()` 一起删掉了**：前缀之后的那个字节一定是
命令，不可能是转义序列的一部分。顺带白送一个好处 —— ESC 不再被拦截，guest
里的方向键天然可用。

前缀选 `Ctrl+T`，是因为它是唯一一个既好按、又没被占用的控制字节：

| 字节 | 被谁占了 |
|---|---|
| `Ctrl+A` | QEMU `-nographic` 的转义前缀（三个架构的 Makefile 都这么启）+ screen / minicom / picocom —— **根本到不了这个内核** |
| `Ctrl+B` | tmux |
| `Ctrl+]` | telnet |
| `Ctrl+C` | 必须留给 guest（SIGINT） |
| `Ctrl+^` `Ctrl+_` | 没人用，但要 Shift+6 / Shift+-，部分键盘布局和终端发不出来 |
| `Ctrl+\` | kermit，而且是 guest 里的 SIGQUIT |

代价：guest 里 bash 的 `transpose-chars`（罕用）让位给控制台；要发字面量就
按两次（`Ctrl+T Ctrl+T`），或者单按一次前缀、等 ~1s 超时后它会被自动补发。

> ⚠️ 那个 ~1s 超时是**必须**的，不是装饰：没有它，单独误按一次 `Ctrl+T`
> 会把**下一次**按键当成命令执行 —— 而其中 `k` 是「停掉前台 VM 并退出」。
> 超时的实现是"poll 超时算一整步、有事件算一小步"的近似累加（helper 没有
> 时钟源可用），精度不重要。

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
#   启动 guest ✓ / 输入输出透传 ✓ / 停止后**再启一次**（重入）✓ /
#   分离 → 宿主 shell 可用 → vmm-run 重新接入 ✓
#
# ⚠️ 这是 2026-09-26 的记录，当时用的还是 Ctrl+] / Ctrl+[。四条**语义**与
#    按键无关，换键（2026-09-29，改成 Ctrl+T 前缀 + -a/-l/-k）不影响这些
#    结论，但换键之后的操作序列需要按 §3a 的新表重跑一遍。
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
#   [vmm-run] guest started (vm1) — Ctrl+T ? for help
#   ... guest 启动日志 ...
#   ~ # （能逐字回显，说明输入通了）
#   ^T k          ← Ctrl+T 然后 k
#   [vmm-run] guest stopping
#   / #           ← 回到宿主 shell，立刻可用
```

**分离 / 接入**：

```
#   / # vmm-run          （guest 已在跑就接入，否则新建）
#   [vmm-run] attached (vm1) — Ctrl+T ? for help
#   ^T d                 ← Ctrl+T 然后 d：guest 留后台
#   [vmm-run] detached — guest keeps running; run vmm-run to reattach
#   / # echo alive       （宿主 shell 立刻可用，guest 仍在跑）
#   alive
#   / # vmm-run -l       ← 看有哪些 VM 在跑
#   / # vmm-run          ← 接回来（接回**上次离开的那个**）
#   / # vmm-run -a 2     ← 或者显式指定接哪个
#   / # vmm-run -k       ← 不带参数：停掉**全部**；-k <vmid> 只停一个
```

再跑一次 `vmm-run` 应当能重新启动（重入是支持的：guest RAM 每次重新
memset + 重新加载镜像 + `vm_create()` 重建 Stage-2 与 vGIC/vPL011）。

反向验证直启模式没被破坏：`make PLATFORM=qemu-virt-aarch64 test-guest-linux`
（2026-09-27 起 `platforms/qemu-virt-aarch64/platform.conf` 的默认已是 **v3**，
不指定 `GIC=` 就走 v3；要回 v2 得显式 `GIC=v2`），应照旧直接进 `~ #` shell 并能
逐字回显输入 —— 那条路走 `vmm_console_pump`，不经过 helper。

**已验证的组合**（2026-09-24，均为 0 个 `[ERROR]`）：

| 配置 | shell + vmm-run（含分离/接入/`-k`） | 直启 test-guest-linux |
|---|---|---|
| `GIC=v2`（默认） | ✅ | ✅ |
| `GIC=v3` | ✅ | ✅ |
