# RK3588：把 NPU 透传给 aarch64 Linux guest

**目标**：宿主（Avatar OS）起一个 Linux guest，在 **guest 里**跑 YOLOv8
（`yolo 7` 三核），NPU 是**真硬件**，不是模拟。

**结论**：跑得通的路径是「寄存器 MMIO 转发 + 一块恒等映射的 DMA 窗口 +
VMM 轮询注入中断」。三件事缺一不可，而且每一件失败的方式都**不报错**
（见下面各自的 ⚠️）。

本文是**从零重建**的全部步骤。已经入库的东西：

| 东西 | 在哪 |
|---|---|
| guest 内核配置改动 | 本文 §2（3 个符号） |
| rknpu DKMS 驱动的移植补丁 | `tools/rknpu_6.2.15_port.patch` |
| guest DTB 源码 | `imgs/guests/aarch64/linux-gicv3-npu.dts` |
| 打包脚本 | `tools/make_npu_rootfs.sh` |
| 宿主侧透传实现 | `kernel/vmm/vdev/npu/vnpu.c`、`driver/npu/rknpu.c` |

**guest 内核 Image 和 rknpu.ko 不在仓库里**（39 MB / 408 KB 的外部构建产物），
要按 §2、§3 自己编 —— 编一次就行，编完 `tools/make_npu_rootfs.sh` 会自动找到。

---

## 1. 全貌

```
        ┌─────────────────────────── 宿主（Avatar OS, EL1+EL2）──────────────┐
        │                                                                   │
  guest │  ① MMIO 转发            ② DMA 窗口            ③ 中断注入          │
  ──────┼───────────────────────────────────────────────────────────────   │
        │                                                                   │
  写 0xFDAB0010 ──► stage-2 无映射 ──► 陷入 EL2 ──► MMIO 总线 ──► 真寄存器   │
        │                                    (vnpu.c)                       │
  DMA 描述符里的"物理地址" ──► 恒等窗口 IPA==PA ──► 真物理内存                 │
        │                    (stage2_map_identity_block)                    │
  NPU 中断 142/143/144 ──► 宿主 GIC **关闭** ──► VMM 轮询 INT_STATUS           │
        │                     ──► vmm_vgic3_set_pending ──► LR ──► guest      │
        └───────────────────────────────────────────────────────────────────┘
```

三块各自的关键决定：

### ① 寄存器为什么走「转发」而不是 stage-2 直通

直通（把 0xFDAB0000 那段直接映射进 guest）也能跑，但**宿主侧一个观测点都没有**。
转发多一次 EL2 往返，而 rknpu 跑一个作业只写十来个寄存器（regcmd 是走 DMA 的，
不经过 MMIO），这点开销可以忽略，换来的是「guest 到底读写过哪些寄存器」可见。

代价：**这个窗口在 stage-2 里必须保持无映射** —— 一旦映射了访问就不会陷入 EL2，
也就走不到 MMIO 总线。所以它不能出现在任何 `stage2_map_range` 的调用里。

### ② DMA 窗口：为什么非要恒等

NPU 是**没有 IOMMU 的物理设备**，只认物理地址。而 guest 的普通 RAM 是宿主按页
从 PMM 现分配的 —— 物理不连续，且 PA 与 IPA 毫无关系。所以 guest 驱动把
「guest 物理地址」写进 DMA 描述符时，NPU 会拿着这个数去访问**宿主**物理内存，
落在哪一页完全随机。

解法是划一块 **IPA == PA** 的窗口。三处必须一致，缺一个就是随机写内存：

| # | 位置 | 值 |
|---|---|---|
| 1 | guest DTB 的 `reserved-memory`（NPU 节点的 `memory-region` 指向它） | `0x79000000 + 0x03000000` |
| 2 | `include/guest_loader.h` 的 `GUEST_LINUX_NPU_DMA_GPA/SIZE`（VMM 建恒等映射用） | 同上 |
| 3 | `platforms/rk3588-aarch64/platform.conf` 的 `reserves`（把同一段**物理**内存从宿主 PMM 摘出去） | 同上 |

第 3 条最容易被忘：不摘的话宿主会把这段物理内存分给别的用途（文件缓存、
别的进程的匿名页），NPU 写进去就把那些数据砸了 —— 而且 symptom 在完全无关的
地方出现。

> ⚠️ **initrd 的可用空间只有 16 MiB**（`0x78000000..0x79000000`），因为上面紧挨着
> NPU 恒等窗口、下面紧挨着内核自己挑的 CMA（32 MiB @ `0x76000000`）。
> initrd 越过上界不会报错，而是把窗口那段 IPA 覆盖掉，**症状是 NPU 读到
> initramfs 的内容**。所以 `guest_loader.c` 在**加载前**用 `vfs_stat_file`
> 查了大小，超了直接拒绝启动（放在加载后检查是没用的：那时页已经映射进去了）。
> 带 NPU 的 initramfs 现在 9.2 MiB（gzip 后），余量 7 MiB。
>
> ⚠️ **想把 initrd 做大，不能只改 `GUEST_LINUX_INITRD_GPA`。** 得先在 guest
> DTS 里把 `linux,cma` 显式钉死在一个确定的位置，再挪 initrd —— 否则 CMA
> 会被挤到别处，或者和 initrd 抢同一段内存，而两者都不报错。

### ③ 中断为什么是轮询，不是装宿主 ISR

这是整个设计里最反直觉的一条。GICv2 的 vtimer 那条路是「装宿主 ISR、在 ISR 里
注入」，NPU 不能照抄，三个理由：

1. **宿主 ISR 收不到同步时机。** guest 运行中到达的宿主中断是在 EL2 **就地处理完
   直接 `eret` 回 guest** 的，不经过 vCPU 主循环 —— 也就拿不到
   `vmm_arch_restore_guest_ctx()` 里那次 `lr_switch_in`。
2. **宿主 ISR 会把 guest 的中断吃掉。** 中断处理程序的正常收尾是写 `INT_CLEAR`，
   那正是 guest 在等的那个中断 —— 两边都不报错，guest 一直等到超时。
3. **电平线会卡死宿主机。** NPU 的中断是电平触发，状态位一直有效到被清。
   宿主 ack 之后线**立刻**重新拉高 → 宿主再次进 ISR → 死循环，guest 永远拿不到
   CPU。

轮询把这三个问题一次绕开：宿主在 GIC 里**关掉** 142/143/144
（`rknpu_host_release()`），VMM 在每次进入 guest 前读一次 `INT_STATUS & ~INT_MASK`，
为真就把对应 INTID 置进 vGIC 的 pending 位图（`vnpu_irq_asserted()` →
`aarch64_check_vnpu()`）。

代价是每次入口多读 6 个寄存器。收益是**电平语义天然正确**：guest 应答一次
vGIC 会清 pending，但状态位还在，下次入口会重新置 —— 这正是电平线该有的行为，
和 vPL011 的 RX 完全同构（见 `include/vmm/vmm_virq.h`）。

**为什么轮询不会漏中断**：guest 的 rknpu 驱动在 `rknpu_job_wait()` 里
`wait_event_timeout` 睡等，睡眠走 WFI，而 `HCR_EL2.TWI=1` 让 WFI **陷入 EL2**
→ 回到 vCPU 主循环 → 又一次 `restore_guest_ctx` → 又一次轮询。
所以 guest 越是在等中断，轮询得越勤。

### 独占性

NPU 是单实例硬件（三核共享一组 PC 寄存器和电源域）。两个 VM 同时透传必然互相
踩，而硬件层没有隔离手段（我们没启 SMMU），所以 `vnpu_init()` 有一个
owner 守卫，**拒绝**第二个申请者 —— 拒绝是可诊断的，静默错乱不是。
VM 销毁时 `vnpu_destroy()` 把 NPU 还给宿主驱动（`rknpu_host_reclaim()`），
所以停掉 VM 之后还能再起一个。

---

## 2. 编 guest 内核

需要一个 6.2.15 的源码树。**配置直接从现成的 guest Image 里取** ——
那份 Image 开了 `CONFIG_IKCONFIG=y`，配置就嵌在镜像里，不用去猜。

```bash
mkdir -p ~/kbuild && cd ~/kbuild
curl -LO https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.2.15.tar.xz
tar xf linux-6.2.15.tar.xz && mv linux-6.2.15 linux-guest-arm64
cd linux-guest-arm64

python3 - <<'PY'
import zlib
raw = open('/path/to/avatar-next/imgs/guests/aarch64/linux.bin','rb').read()
a = raw.find(b'IKCFG_ST'); b = raw.find(b'IKCFG_ED')
open('.config','w').write(zlib.decompress(raw[a+8:b], 16+zlib.MAX_WBITS).decode())
PY
```

配置**只动三处**（实测 diff 出来就这三行）：

```bash
scripts/config --enable DMABUF_HEAPS          # n -> y   rknpu 走 dma-heap 要
scripts/config --enable DMABUF_HEAPS_SYSTEM   # n -> y   同上（system heap）
scripts/config --enable DRM                   # m -> y   见下
```

> ⚠️ **`CONFIG_DRM` 必须是 `=y` 而不是 `=m`。** rknpu.ko 用了 DRM 的内部符号
> （`drm_gem_*` / `drm_dev_*`）。若 DRM 是模块，`modinfo` 出来的 `depends`
> 会带上 `drm`，那么每个用它的人都得先 `insmod drm.ko`，而 initramfs 里
> 我们只放了一个 rknpu.ko。编成 `=y` 让这些符号进 vmlinux，rknpu.ko 就
> 自包含了（实测 `depends` 为空）。

然后编：

```bash
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-musl- olddefconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-musl- Image modules -j"$(nproc)"
```

- `olddefconfig` 之后**再确认一次**那三行没被改回去（`grep CONFIG_DMABUF_HEAPS_SYSTEM .config`）。
- **`modules` 不能省**：它生成 `Module.symvers`，没有它 modpost 解析不了
  rknpu.ko 引用的内核符号（`make modules_prepare` 不生成这个文件）。
- 产物：`arch/arm64/boot/Image`（约 39 MB）。

---

## 3. 编 rknpu.ko

驱动来自 DKMS 仓库 `github.com/bmilde/rknpu-driver-dkms` 的 `rknpu-0.9.8`，
针对 mainline 6.2.15 打 4 处补丁。

```bash
mkdir -p ~/kbuild/rknpu-mod && cd ~/kbuild/rknpu-mod
git clone --depth 1 https://github.com/bmilde/rknpu-driver-dkms
cd rknpu-driver-dkms/rknpu-0.9.8        # 注意：源码在仓库的**子目录**里

patch -p1 < /path/to/avatar-next/tools/rknpu_6.2.15_port.patch

make -C ~/kbuild/linux-guest-arm64 M=$PWD ARCH=arm64 \
     CROSS_COMPILE=aarch64-linux-musl- modules
```

产物 `rknpu.ko`（约 408 KB）。验一下它认得那个内核：

```bash
modinfo rknpu.ko | grep -E '^(vermagic|depends)'
# vermagic: 6.2.15 SMP preempt mod_unload aarch64
# depends:            <- 空，理由见 §2 的 CONFIG_DRM 说明
```

### 补丁改了四处（每一处都对应一个真实报错）

| 文件 | 改动 | 为什么 |
|---|---|---|
| `Makefile` | 去掉 `-Werror`；去掉 `rknpu_devfreq.o`、加 `rknpu_devfreq_stub.o`；补 `ccflags-y` 的 `-DCONFIG_ROCKCHIP_RKNPU*` | 见下面两条 + 配置宏在 out-of-tree 构建里拿不到 |
| `include/linux/version_compat_defs.h` | **新增**（41 行） | BSP 那个 524 行的 Mali 兼容头我们没要；只补 `vm_flags_set/clear`（Linux **6.3** 才有）这一个小口子 |
| `include/rockchip_iommu.h` | 守卫从 `#if IS_REACHABLE(CONFIG_ROCKCHIP_IOMMU)` 改成 `#if 0` | **同名陷阱**见下 |
| `rknpu_devfreq_stub.c` | **新增**，6 个空函数 | `-UCONFIG_PM_DEVFREQ` 在 ccflags 里**不生效** |

> ⚠️ **`CONFIG_ROCKCHIP_IOMMU` 是同名陷阱。** arm64 defconfig 会把它打开，
> 但它指的是 **mainline** 那个 IOMMU 驱动，而 rknpu 要的是 **BSP 版**的
> `rockchip_iommu_is_enabled()` 等一整套函数 —— mainline 树里根本没有，
> 于是链接期一片 undefined。判据（`IS_REACHABLE`）本身没错，错在
> "同名 = 同一个东西"这个假设。我们没有 SMMU 的需求，直接关掉这条支路。

> ⚠️ **`-U` 覆盖不了配置宏。** kbuild 会 `-include autoconf.h`，那是在
> `ccflags-y` **之后**处理的，所以 `ccflags-y += -UCONFIG_PM_DEVFREQ` 会被
> 重新定义回去。唯一可靠的办法是提供一个 stub 实现（或者改 `Kconfig`）。

---

## 4. guest DTB

`imgs/guests/aarch64/linux-gicv3-npu.dts` 是在原 `linux-gicv3.dts` 上加了两块：

```dts
npu-clk { compatible = "fixed-clock"; clock-frequency = <200000000>; };

npu: npu@fdab0000 {
    compatible = "rockchip,rk3588-rknpu";
    reg = <0x0 0xfdab0000 0x0 0x10000 0x0 0xfdac0000 0x0 0x10000 0x0 0xfdad0000 0x0 0x10000>;
    interrupts = <0 0x6e 4 0 0x6f 4 0 0x70 4>;   /* 110/111/112 → INTID 142/143/144 */
    interrupt-names = "npu0_irq", "npu1_irq", "npu2_irq";
    clocks = <&npu_clk>;
    memory-region = <&npu_pool>;
    dma-coherent;               /* ⚠️ 不能省，与池的 no-map 是一套 */
};

reserved-memory {
    npu-pool@79000000 {
        compatible = "shared-dma-pool";
        reg = <0x0 0x79000000 0x0 0x3000000>;
        no-map;                 /* ⚠️ 不能省，见下 */
    };
};
```

> ⚠️ **`no-map` 不能省 —— 少了它 NPU 拿不到一个字节的 DMA 池。**
>
> 没写 `no-map` 时这块内存仍然是 System RAM，而
> `dma_init_coherent_memory()` 写死了 `memremap(..., MEMREMAP_WC)`
> （`kernel/dma/coherent.c:48`），memremap 对 **System RAM 用 WC** 是
> 「告警 + `return NULL`」（`kernel/iomem.c:106`）→
> `of_reserved_mem_device_init()` 失败 → 池没建起来。
>
> **这个坑的恶劣之处在于它长得像无害的啰嗦**：
>
> ```
> ------------[ cut here ]------------
> memremap attempted on ram 0x0000000079000000 size: 0x3000000
> WARNING: CPU: 0 PID: 47 at kernel/iomem.c:107 memremap+0x1d0/0x220
> ```
>
> 而**紧接着 probe 仍然报成功**（`[drm] Initialized rknpu ... on minor 0`），
> 所以很容易当成"一个 warning 而已"跳过。真的去看才知道那个分支是
> `return NULL`，池根本没建。
>
> 加了 `no-map` 之后，arm64 的 `request_standard_resources()` 会把这个区标成
> `IORESOURCE_MEM` 而不是 `IORESOURCE_SYSTEM_RAM`
> （`arch/arm64/kernel/setup.c:241`）→ `is_ram` 为假 → memremap 走
> `ioremap_wc` ✓。语义上也对：这块内存是给设备专用的，内核的线性映射不该
> 直接碰它。

> ⚠️ **但 `no-map` 单独用会崩 —— 它和 `dma-coherent` 是一套，必须同时给。**
>
> `no-map` 的代价是这块内存**没有线性映射**。而内核在 `dma_sync_*` 里：
>
> ```c
> /* kernel/dma/direct.c:422 */
> if (!dev_is_dma_coherent(dev))
>         arch_sync_dma_for_device(paddr, sg->length, dir);
> /* arch/arm64/mm/dma-mapping.c:19 */
>         dcache_clean_poc(phys_to_virt(paddr), ...);   ← 走线性映射
> ```
>
> 设备不是 coherent 时，内核对这块内存做 cache 维护 —— 而线性映射根本不存在，
> 于是 **level 2 translation fault → guest 内核 Oops**：
>
> ```
> pc : dcache_clean_poc+0x20
> lr : arch_sync_dma_for_device+0x24
> rknpu_gem_sync_ioctl+0x1b0/0x980 [rknpu]
> FSC = 0x06: level 2 translation fault   CM=1 WnR=1
> ```
>
> 给设备节点加 `dma-coherent` 之后 `dev_is_dma_coherent()` 为真，那句直接跳过。
> 这正是 `no-map` 池的**设计前提**：CPU 侧走 `ioremap_wc`（非缓存），写出去
> 就到内存，本来就不需要维护缓存。
>
> 真机上这两个属性各踩了一次：先缺 `no-map`（作业永远超时），补上之后
> 缺 `dma-coherent`（一跑就 Oops）。宿主内核会在 guest 启动前把两个都查一遍
> 并报出来（`guest_loader_dtb_node_has_prop`），`tools/guest_npu_qemu_test.sh`
> 也会先查 —— 但**注意必须按节点查**：`dma-coherent` 在 QEMU virt 的
> fw-cfg / virtio_mmio / pcie 上本来就有，"属性名在不在整份 DTB 里"这种查法
> 永远通过，等于没查（第一版就是这个假阳性）。

**故意不写**的东西，都是有原因的，别顺手加回去：

- **`iommus`** —— 宿主没启 SMMU。写了 guest 会去等 IOMMU 初始化。
- **`power-domains`** —— 宿主已经在 `rknpu_init()` 里把 NPU 域上电了
  （`rkpm_power_on(PD_NPU/NPUTOP/NPU1/NPU2)`）。DTB 里没有对应的
  power-controller 节点，写了只会 probe 失败。
- **`resets` / `assigned-clocks` / `opp`** —— 同上，宿主侧没有这些 provider。
- **`rockchip,grf` 之类** —— 不需要。

`npu_clk` 给的是 200 MHz 的假时钟：驱动只是要有个能 `clk_get_rate` 的东西，
而真时钟由宿主在 boot 时配好。**编 DTB**：

```bash
dtc -I dts -O dtb -o imgs/guests/aarch64/linux-gicv3-npu.dtb \
    imgs/guests/aarch64/linux-gicv3-npu.dts
```

（入库的那份就是这么来的，重编一次能字节重现。）

---

## 5. 打包：一条命令

```bash
# 宿主内核（带 NPU 透传；vnpu.c 在 aarch64 上无条件编进去，
# 平台没有 NPU 时 vnpu_init 直接返回，不会报错）
make PLATFORM=rk3588-aarch64 kernel

# 宿主侧的验收工具（可选）
make PLATFORM=rk3588-aarch64 drmtest dmatest

# 一次做完 guest initrd + 宿主 rootfs
tools/make_npu_rootfs.sh
```

`tools/make_npu_rootfs.sh` 干三段：

- **§1** 造 guest initrd：`rknpu.ko` + 一整套 glibc + `librknnrt.so` +
  `rknn_yolov8_demo` + `yolov8n.rknn` + `/bin/yolo` 包装，
  经 `tools/guest-initrd.py aarch64 --gzip --extra <树>` 打成一个包。
  写回 `imgs/guests/aarch64/initrd.gz`。
- **§2** 在宿主 rootfs 基镜像上加 `/usr/lib/rknn{,-yolo}`、`/bin/{rknn,yolo}`、
  `/guests/linux/{linux.bin,linux.dtb,initrd.gz}`、`/bin/vmm-run`、验收工具。
- **§3** 重打包成 `imgs/rootfs-aarch64.img`（就地覆盖，`git checkout` 可退）。

guest 镜像从哪来：

| 文件 | 来源 |
|---|---|
| `linux.bin` | `$GUEST_IMAGE`，默认 `~/kbuild/linux-guest-arm64/arch/arm64/boot/Image` |
| `linux.dtb` | 仓库里的 `imgs/guests/aarch64/linux-gicv3-npu.dtb` |
| `initrd.gz` | §1 现造 |
| `rknpu.ko` | `$GUEST_KO`，默认 `~/kbuild/rknpu-mod/rknpu-0.9.8/rknpu.ko` |

**找不到 guest 内核时不会失败** —— 退回仓库里那份没有 NPU 驱动的
`linux.bin`，只打警告。因为宿主侧的东西能独立验证，不该被一个路径拖住。

### guest 侧怎么装上驱动的

`imgs/guests/initrd-common/init`（三个架构共用）里加了：

```sh
[ -f /lib/modules/rknpu.ko ] && insmod /lib/modules/rknpu.ko
```

判存在是必须的：x86_64 / rv64 的包里没有这个文件。
位置在 mount 之后 —— 驱动 probe 时要 `/sys` 和 `/dev`
（`/dev/dri/renderD128`、`/dev/dma_heap/*` 由 devtmpfs 建）。

---

### 先本地自检，再上板

```bash
tools/guest_npu_qemu_test.sh      # 不需要 RK3588，QEMU 里跑 guest 内核 + DTB + initramfs
```

它验三条：预留 DMA 池建起来没有（`initialize reserved memory for rknpu device`）、
rknpu probe 完没完、有没有那条 `memremap attempted on ram`。
**"DTB 里少一个 `no-map`" 这个错在 QEMU 里一眼可见**，不用上板试 —— 这条
是实测踩出来的：真机上连着两轮都卡在同一个 no-map 上，而它在本地就能查。

它**测不了** NPU 寄存器访问和作业执行（QEMU 没有 NPU 硬件），那部分只能上板。

## 6. 真机上怎么用

```sh
~ # vmm-run              # 起 guest，Ctrl+T 是前缀键
                      # 进 guest shell 后：
~ # yolo              # 单核（librknnrt 默认 AUTO）
~ # yolo 7            # 三核
```

宿主编译期要看到 `[vnpu] NPU 透传已注册`；首次注入中断时打一行
`[vnpu] 首次向 guest 注入 NPU coreN 中断 (INTID 14N)`。
三行都出现（core0/1/2），才说明三条中断线都通了。

guest 里 `insmod` 之后 dmesg 应当是干净的（没有 `memremap attempted on ram`，
见 §4）；看到它说明 DTB 里的 `no-map` 丢了。

> ⚠️ **两次尝试之间要重启板子。** 一个作业超时后对应的 NPU 核会**卡在半路**
> （`PC_OP_EN` 读回 1、`task_status` 停在中间值），之后同一核上的任何提交都
> 起不来 —— 看起来像"别的也坏了"，其实是上一次的余波。这一条在宿主侧踩过一次，
> guest 侧同理（同一个硬件）。

---

## 7. 已知限制与**未验证项**

写清楚哪些是实测的、哪些只是设计上应当成立，免得后面有人把"应该"当"已经"。

**已经在真机上实测的**：

- 宿主侧：rknpu 驱动跑通官方 `librknnrt.so`（未修改的二进制）；
  三核提交 `yolo 7` 结果与单核逐位相同，三核 interrupt 各一次。
- **guest 开机到 shell。** guest 内核、带 NPU 节点的 DTB、9.2 MiB 的
  initramfs 全都在真机上跑起来了，`~ #` 提示符正常。
- **guest 里 `insmod rknpu.ko` 成功。** dmesg 有
  `[drm] Initialized rknpu 0.9.8 20240828 for fdab0000.npu on minor 0`，
  `depends` 为空这条判断得到证实（宿主树编出来的 .ko 直接就能装）。
- reserved-memory 被正确解析：
  `Reserved memory: created DMA memory pool at 0x0000000079000000, size 48 MiB`。

**没有验证的（拿到板上要重点看这几条）**：

1. **恒等窗口的实际行为。** `stage2_map_identity_block()` 是这次新写的
   （[`kernel/mm/aarch64/stage2.c`]），**没有在真机上触发过**。第一次
   NPU DMA 时应当看到 `[vnpu]` 或缺页调试日志；如果 NPU 读到垃圾数据，
   第一个要查的就是这里的三处地址是否一致。
2. **guest 里真的跑一次推理。** 驱动 probe 成功 ≠ 能出结果。要看到
   `yolo` 打出检测框才算通。
3. **guest 侧的中断延迟。** 轮询注入的延迟上界是"guest 两次陷入之间的时间"，
   驱动睡眠时是 WFI 陷入（很快），但**没有实测数字**。
4. **多 VM。** 第二个 VM 申请 NPU 会被拒绝（有守卫），但"停掉再起"这条
   路径（`vnpu_destroy` → `rknpu_host_reclaim`）没在真机上走过。

### 宿主内核的版本怎么认

**别用开机横幅那行 `=== Avatar OS Kernel aarch64 build ... ===`** —— 它来自
`main.c` 的 `__DATE__`/`__TIME__`，只改 VMM 里的文件时 `main.c` 不会重编，
横幅就还印着上一次的时间。真机上认版本看**行为**：

```
[mmio] registered 'vnpu' base=0xfdab0000 size=0x30000      ← 有这行 = 新内核
[vnpu] NPU 透传已注册: guest 0xfdab0000..0xfdaE0000 -> ...
[guest] loaded '/guests/linux/initrd.gz': ... -> GPA 0x78000000
```

反过来，如果 MMIO 只注册了 `vpl011` / `vgic3r` / `vgic3d` 三个，那就是**没有
透传**的旧内核。

---

## 8. 相关文档

- `docs/vmm/GUEST_CONSOLE.md` —— `/dev/vmm` 协议、Ctrl+T 前缀键、helper 模式
- `docs/vmm/X86_GUEST_LINUX.md` §11 —— 「同内核多 VM」的另一套实现，
  EPT 与 G-stage 的按需分页在这里是同一套思路
- `docs/npu/RK3588_NPU_DRIVER.md` —— 宿主侧 NPU 驱动的寄存器/ABI 细节
- `apps/rknn-aarch64/README.md` —— YOLO demo 怎么编出来的、为什么带两套 glibc
