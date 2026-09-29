# 裸跑 guest Linux（不经过 Avatar VMM）

> 最后更新：2026-09-24
> 用途：把 guest 镜像直接交给 QEMU 启动，作为**基准**来量 Avatar VMM 的开销。

## 1. 这是什么

Avatar 的 `test-guest-linux` / `vmm-run` 会把同一份 guest 镜像（`imgs/guests/aarch64/linux.bin`
+ `initrd.gz`）加载进自己的 guest 物理内存再跑。本文的命令**绕开 Avatar 全部代码**，
让 QEMU 直接启动那份镜像 —— 于是「裸跑用时」与「经过 VMM 的用时」之差，就是 VMM 的净开销。

## 2. 准备一份「等效 DTB」（重要）

直接跑要先解决一件事：**Avatar 会在运行时改 DTB**。`guest_loader` 除了打内存/initrd
补丁之外，还会把 7 个 VMM 未模拟的设备节点 **NOP 掉**（`guest_loader_nop_dtb_nodes`）：

```
/…/v2m@8020000   /virtio_mmio@a000000   /pcie@10000000   /pl061@9030000
/pl031@9010000   /flash@0               /fw-cfg@9020000
```

拿原始 `linux.dtb` 直接给 QEMU 是**不公平的基准**：QEMU 真的实现了这些设备（PCIe、
PL061、PL031、flash、fw-cfg…），guest 会去探测它们，比 Avatar 那边多做一大堆事。

用 `fdtput` 生成一份等价 DTB（一次性，之后可重复使用）：

```bash
cp imgs/guests/aarch64/linux.dtb /tmp/nopdtb.dtb
for n in /fw-cfg@9020000 /virtio_mmio@a000000 /pl061@9030000 \
         /pcie@10000000 /pl031@9010000 /flash@0 /intc@8000000/v2m@8020000; do
    fdtput -r /tmp/nopdtb.dtb "$n"
done
```

判据：裸跑的输出里应当出现和 Avatar 那边**一模一样的**报错签名（这是 NOP 掉 pl061
之后 `gpio-keys` 的 phandle 悬空导致的，两边都有）：

```
OF: /gpio-keys/poweroff: could not find phandle 32772
gpio-keys gpio-keys: failed to get gpio: -22
```

> GICv3 配置就换成 `imgs/guests/aarch64/linux-gicv3.dtb`，节点路径相同。

## 3. 命令

在仓库根目录执行。`imgs/guests/aarch64/` 下的文件是前提。

### 变体 A：直接进 shell（**推荐**，不需要 root 密码）

```bash
qemu-system-aarch64 -cpu cortex-a76 -M virt -smp 1 -m 1G -nographic \
  -kernel imgs/guests/aarch64/linux.bin \
  -initrd imgs/guests/aarch64/initrd.gz \
  -dtb /tmp/nopdtb.dtb \
  -append "console=ttyAMA0 rdinit=/bin/sh"
```

`rdinit=/bin/sh` 让内核把 `/bin/sh` 当 init 直接跑 —— initrd 里带了一条
`bin/sh → busybox` 软链接，所以这条能用。**它在裸跑下能生效**是因为命令行走
`-append`（QEMU 自己往 `/chosen/bootargs` 写），不受 Avatar 那个
「DTB 里 bootargs 槽位只有 79 字节」的限制。
（`rdinit=/init` 现在也进同一个 shell，见变体 B。）

按 **Ctrl-A X** 退出（`-nographic` 下 Ctrl-C 是发给 guest 的）。

### 变体 B：走包里的 `/init`（同样直接进 shell）

把 `-append` 换成：

```bash
  -append "console=ttyAMA0 rdinit=/init"                    # 详细日志
  -append "quiet console=ttyAMA0 rdinit=/init"              # 静音，快 0.86s（见 §5）
```

### 变体 C：GICv3（与 `GIC=v3` 的 Avatar 构建对照）

`-M virt,gic-version=3` + `-dtb imgs/guests/aarch64/linux-gicv3.dtb`（先按 §2 同样方式 NOP 一份）。

## 4. 参数为什么这么写

| 参数 | 说明 |
|---|---|
| **没有 `-enable-kvm`** | 宿主是 x86_64、跑的是 `qemu-system-aarch64`，**跨架构无法用 KVM**，全程 TCG 软件模拟。这决定了性能上限。（对比：x86_64 的 `QEMU_FLAGS` 有 `-enable-kvm`，aarch64 的没有，见 Makefile §5。） |
| `-m 1G` | **不能小**：DTB 里 `memory@70000000` 声明的是 `0x70000000 + 192 MiB`，而 `-M virt` 的 RAM 从 `0x40000000` 起。要覆盖到 `0x7c000000` 至少得 `-m 960M`。 |
| `-dtb /tmp/nopdtb.dtb` | 见 §2。想快速判断「QEMU 本身能不能跑起来」时可以省掉，用 QEMU 自动生成的 DTB，但那样不是公平基准。 |
| `-M virt`（**不带** `virtualization=on`） | 裸跑不需要虚拟化扩展；`virtualization=on` 是给宿主内核当 hypervisor 用的。 |
| `-cpu cortex-a76` | 与 Avatar 的 `QEMU_FLAGS` 一致。 |
| `console=ttyAMA0` | guest 控制台走 PL011，与 Avatar 的 vpl011 同一个地址（0x09000000）。 |

## 5. ⚠️ 计时方法有坑

**`~ #` 提示符没有换行。** 用 `python3`/`while read` 之类**按行**打时间戳的过滤器，
会把这行卡在管道缓冲区里，直到后面来了 `\n` 或者进程被杀 —— 于是你会看到
「`~ #` 出现在 timeout 杀进程的那一毫秒」，误以为启动花了 20~70 秒。

**正确做法：后台跑，用 `grep` 轮询输出文件。** GNU grep 能匹配「没有换行的最后一行」：

```bash
log=/tmp/guest-native.log; rm -f $log
t0=$(date +%s.%N)
qemu-system-aarch64 -cpu cortex-a76 -M virt -smp 1 -m 1G -nographic \
  -kernel imgs/guests/aarch64/linux.bin -initrd imgs/guests/aarch64/initrd.gz \
  -dtb /tmp/nopdtb.dtb -append "console=ttyAMA0 rdinit=/bin/sh" \
  > $log 2>&1 < /dev/null &
qpid=$!
until grep -qa "~ #" $log 2>/dev/null; do sleep 0.02; done
echo "→ 提示符: $(echo "$(date +%s.%N)-$t0" | bc) s"
kill $qpid 2>/dev/null
```

（`grep -a` 必须加：输出里有 ANSI 转义序列，不加会被当成二进制文件而静默不匹配。）

## 6. 本机实测（2026-09-24，x86_64 宿主 / TCG，单次采样，±0.05 s）

| 路径 | 详细日志 | `quiet` |
|---|---|---|
| **裸跑**（等效 DTB，`rdinit=/init`） | **1.016 s** | **0.995 s** |
| **Avatar VMM**（`vmm-run` → `~ #`） | **2.593 s** | **1.683 s**（当前默认） |
| VMM 净开销 | +1.58 s | **+0.69 s** |

> avatar 侧的 `quiet` 现已是默认（`GUEST_LINUX_BOOTARGS`，见 §7），详细日志那一列
> 是把 `quiet ` 去掉之后的数据。

### 这张表里最值得看的一行

**裸跑下 verbosity 几乎不要钱（1.016 → 0.995，差 0.02 s），VMM 下要 0.86 s。**

原因：guest 每写一个字符到 UARTDR，PL011 区间在 stage-2 里都是无效的 → 一次
data abort → **一次完整 VM exit**（世界切换：128 字节系统寄存器存取 + `stage2_activate`
+ vGIC 同步），实测约 35~70 µs。启动日志约 1.2 万个字符，累计 0.86 s。
裸跑的 QEMU 直接内联处理 UART 写入，没有这个代价。

### 剩下的开销分解

`quiet` 之后的 1.730 s 几乎可以完全解释为：

| 项 | 耗时 |
|---|---|
| guest 自身启动（= 裸跑） | 0.995 s |
| 从 ext4 读 38 MB 内核镜像 + DTB 修补 + `vm_create` | ~0.55 s |
| `memset` 192 MB guest RAM | 98 ms |
| `clean_dcache_range` 192 MB | 38 ms |
| console 之外的 VM exit 开销 | ~0.05 s |
| **合计** | **≈ 1.73 s** ✓ |

也就是：**加了 `quiet` 之后，虚拟化本身的开销基本可以忽略，剩下的主要是
「从磁盘镜像把 guest 装进内存」这件 Avatar 独有的准备工作。**

> 量法：Avatar 路径下用 `grep` 轮询内核日志里的 `[vmmdev] boot: starting guest`
> 与 guest 的 `~ #` 两个标记（同样**不要**用按行过滤器）。
> （上表采于 2026-09-24，当时 initrd 停在 `root login:`；2026-09-27 起 initrd 直接进
> shell，标记换成 `~ #`，**数值没有重采**。）

## 7. 降低 guest 控制台开销

**Avatar 侧已经默认开了 `quiet`**，定义在 `kernel/vmm/guest_loader.c` 的
`GUEST_LINUX_BOOTARGS`：

```c
#define GUEST_LINUX_BOOTARGS \
    "quiet console=ttyAMA0 rdinit=/init panic_on_warn=0 oops=panic"
```

想恢复完整启动日志：去掉 `"quiet "` 即可。**命令行有 78 字节的硬上限**
（DTB 里 `/chosen/bootargs` 槽位只有 79 字节，补丁是原地改写、不支持加长），
所以文件里跟了一句 `_Static_assert` —— 超长直接编译不过。别去掉它：这条补丁
失败时表现得和成功一模一样（guest 照常启动），历史上就这样静默失败了很久。

| 做法 | 效果 |
|---|---|
| `quiet` | console_loglevel 7→4，只放行 ERR 及以上。**快 0.91 s** |
| `loglevel=N` | 直接设级别（6=INFO，7=DEBUG） |
| `loglevel=0` | 全静音（比 `quiet` 更彻底） |
| `ignore_loglevel` | 反向：无视级别全打，最慢 |
| `earlycon=…` | **别和 `quiet` 一起开**：早期消息同样被 loglevel 压掉，纯浪费；而且它会让每条消息打印两遍（bootconsole + 真 console），白白多一倍 exit |

**`quiet` 不丢信息**：内核环形缓冲里永远是完整的，只是不往串口写。登进去之后
`dmesg` 全都看得到（而且 `dmesg` 是一次性 dump，比启动期一路上持续输出更划算）。

## 8. 相关

- 两种运行模式、`/dev/vmm` 协议、Ctrl+T 前缀键语义：`docs/vmm/GUEST_CONSOLE.md`
- 命令行的长度约束与取舍理由：`kernel/vmm/guest_loader.c` 的
  `GUEST_LINUX_BOOTARGS` 注释
