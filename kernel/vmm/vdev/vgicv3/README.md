# vGICv3（虚拟 GICv3）实现现状

> 最后更新：2026-09-23
> 对应配置：`make PLATFORM=qemu-virt-aarch64 GIC=v3 test-guest-linux`
> （默认配置仍是 `GIC=v2`，走同级的 `../vgic/`）

## 1. 它是什么

AArch64 宿主跑在 EL2 + VHE，把 Linux 当 EL1 guest 跑。这个目录实现 **guest 视角的
GICv3**：GICD / GICR 的 MMIO 模拟 + 通过 `ICH_LR<n>_EL2` 注入虚拟中断。

和 GICv2 版（`../vgic/`）的本质差别：

| | GICv2（`../vgic/`） | GICv3（本目录） |
|---|---|---|
| CPU interface | GICC MMIO（`0x08010000`）被 stage-2 陷入，**VMM 软件代答** `GICC_IAR`/`EOIR` | `ICV_*` 系统寄存器，**由 GIC 硬件直接服务**，VMM 无法陷入 |
| SGI/PPI | GICD 里的 banked 寄存器 | 每核的 GICR（`0x080A0000`，RD 帧 + SGI 帧） |
| 注入 | `GICH_LR<n>`（32 位，MMIO） | `ICH_LR<n>_EL2`（64 位，系统寄存器） |
| VMM 的角色 | 替 guest ack/EOI | 只管「写入 LR」+「退出后回读 LR 状态」 |

前提条件（缺一不可，在 `../aarch64/el2_vmcs.S` 与 `driver/irq/gicv3.c` 里）：

- 进 guest 时置 `HCR_EL2.IMO=1` —— 否则 guest 在 EL1 访问 `ICC_*_EL1` 不会被重定向到
  `ICV_*`，而是直接操作**物理** GIC。
- `ICH_HCR_EL2.En=1` —— 否则 `ICC_IAR1_EL1` 永远返回 spurious。
- 宿主 `GICD_CTLR` 必须写 `EnableGrp1A(bit1)`，否则物理中断一条都进不来。见第 6 节。

## 2. 文件构成

```
vgicv3.c   VM 级状态机 + ICH_LR 注入 / 状态回读
vgicd3.c   GICD（Distributor）MMIO 模拟 —— ARE 模式下只管 SPI
vgicr3.c   GICR（Redistributor）MMIO 模拟 —— 每核的 SGI/PPI
../../../../include/vmm/vmm_vgicv3.h   数据结构与 API
```

集成点：

- `kernel/vmm/aarch64/vm_init.c` —— `vmm_vgic3_init()` / `vgic3r_init()` / `vgic3d_init()` /
  `vmm_vgic3_hw_init()`，全部在 `#if DRIVER_GIC_V3` 下，与 v2 路径编译期二选一。
  （从前这四处写在 `kernel/vmm/vmm.c` 里，那个文件已按对象拆掉。）
- `kernel/vmm/aarch64/el2_run.c` —— 进 guest 前 `vmm_vgic3_sync_entry()`，退出后
  `vmm_vgic3_sync_exit()`，定时器到期时 `vmm_vgic3_set_pending(PPI 27)`。
- `kernel/vmm/vdev/irq_route.c` —— GICv3 下**故意不注册**宿主 PPI 27，注入完全靠
  轮询 `CNTV`（原因见该文件注释）。

## 3. 已实现 **且已实测可工作**

验证环境：QEMU 8.2.2，`-M virt,virtualization=on,gic-version=3`，1 vCPU，
guest 是 Linux 6.2.15 + initrd。判定标准：guest 打印 `Run /init as init process`
并出现 `~ #` 提示符，全程无 `[ERROR]`。

| 功能 | 证据 |
|---|---|
| GICD 探测 | guest 日志 `GICv3: 988 SPIs implemented`（来自 `GICD_TYPER`） |
| GICD_CTLR 的 ARE/分组配置 | guest 的 `gic_dist_init()` 读写通过，`ARE` 生效后 SPI 配置不丢 |
| GICR 发现 | guest 日志 `GICv3: CPU0: found redistributor 0 region 0:0x00000000080a0000` —— 说明 `GICR_TYPER`（亲和性 + `Last`）被正确解析 |
| GICR 唤醒握手 | `GICR_WAKER.ChildrenAsleep` 读回 0，guest 的 `gic_enable_redist()` 不卡 |
| GICR SGI 帧初始化 | guest 写 `IGROUPR0`/`IPRIORITYR0-7`/`ICENABLER0`/`ICACTIVER0` 均正确落库 |
| **PPI 27（虚拟定时器）投递** | 全链路打通：guest 使能 `GICR_ISENABLER0` bit27 → 宿主 `aarch64_check_vtimer()` 轮询到期 → `ICH_LR<n>_EL2` 注入 → guest 侧 `ICV_IAR1` 取到 27 → `ICV_EOIR1` 完成 → 宿主 `ICH_ELRSR_EL2` 回读清 active。guest 能正常跑完内核初始化、挂载 initramfs、起 login —— 没有 tick 是做不到的 |
| guest PMR / IGRPEN1 / CTLR 配置 | QEMU trace 里能看到 `gicv3_icv_pmr_write`、`gicv3_icv_igrpen_write`、`gicv3_icv_ctlr_write` 等事件，说明 guest 的 `ICC_*_EL1` 访问确实被重定向到了虚拟接口 |
| 优先级传递 | LR 里填的是 guest 自己写进 `GICR_IPRIORITYR` 的优先级（默认 0xA0），与 guest 的 VPMR 同源 |
| LR 复用 / 状态回读 | `ich_hcr_write` 观测到 `En=1`，`ICH_VTR_EL2` 报 4 个 LR（`VGIC3_MAX_LRS=16` 只是上限） |
| **SPI 33（虚拟 PL011 RX）投递** | 唯一的 SPI 源是 `vpl011.c`。全链路：宿主控制台按键 → `vmm_console_pump()` 推入 RX FIFO → 进 guest 前 `aarch64_check_vpl011_rx()` 置 pending → LR → guest pl011 驱动收到。判据：在 `~ #` 后键入 `ls /`、`uname -a` 能逐字回显并正常输出 |

> 注：投递过的中断类型是 PPI 27（虚拟定时器）与 SPI 33（虚拟 PL011 RX）。
> **SGI（0-15）仍然没有源**，从未投递过。

## 4. 已实现但**没有验证过**

这些代码路径写全了，运行中从未被触发，改动时要格外小心：

- **SPI 软触发路径**：`GICD_ISPENDR` / `GICD_ICPENDR` / `GICD_ISACTIVER` /
  `GICD_ICACTIVER` 已实现但没观察过效果。（**SPI 投递本身已经验证**，见第 3 节 ——
  源是 `vpl011.c` 的 RX 中断，走的是 `set_pending()` 而非这些寄存器。）
- **SGI 投递**（INTID 0-15）。`vmm_vgic3_set_sgi_pending()` 会记源核位图，但 GICv3 的
  LR 里没有「SGI 源核」字段，这个信息实际上被丢弃（1 vCPU 下无所谓）。单核启动过程
  中 Linux 没发过 SGI。
- **软触发 / 软清除路径**：`GICD_ISPENDR`、`GICD_ICPENDR`、`GICD_ISACTIVER`、
  `GICD_ICACTIVER` 以及 GICR SGI 帧的同名寄存器。写进去了但没观察过效果。
- **`GICD_IROUTER`**：原样存进字节后备存储，**没有**参与路由决策（没有 SPI，也就没验证过）。
- **`GICD_ICFGR` / `GICR_ICFGR0/1`（边沿/电平）**：只做字节存取，不改变任何行为。
- **多 vCPU**：`vgic3_t` 的数组按 `VGIC3_MAX_VCPUS=8` 开，GICR 按偏移分核，但
  `vm_cfg.nr_vcpus` 一直是 1，且 `vpsci` 的 `PSCI_CPU_ON` 直接返回 NOT_SUPPORTED。
- **LR 满时的行为**：`sync_entry()` 在 `lr_empty_slot() < 0` 时直接返回，剩下的中断
  等下一次 exit 再排。4 个 LR 从没被用满过。
- **`vmm_vgic3_clear_active_word()` / `GICR_CTLR` / `PROPBASER` / `PENDBASER` 的语义**。

## 5. 未实现 / 有意省略

- **LPI 与 ITS**：`GICD_TYPER.LPIS=0`、`ESPI=0`、`GICR_TYPER.PLPIS/VLPIS=0`，
  也没有 ITS 设备。guest 因此不会去碰 `PROPBASER`/`PENDBASER`。
- **HW=1 的 LR（物理中断直通）**：`vgic3_lr_value()` 从不置 `ICH_LR_HW`，
  `vmm_irq_route_host_hwirq_for_guest_irq()` 恒返回 0。宿主 PPI 27 不使能，
  所以不存在「物理中断需要由 guest EOI 来 deactivate」的场景。
- **维护中断**：`ICH_HCR_EL2` 只置 `En`，`EOIEn`/`VGrp*IE` 全 0。改成在**每次 VM exit
  主动轮询** `ICH_ELRSR_EL2` + LR.State。代价见第 6 节第二条。
- **`GICD_ITARGETSR`**：ARE 模式下按规范读 0、写忽略。
- **多个 redistributor region / `redist_stride`**：只支持「一个连续区、每核 0x20000」。
- **GICv4 的 vSGI 直投**。

## 6. 已知限制与坑

1. **`ICH_HCR_EL2.En` 是每 CPU 的，必须设在 vCPU 实际运行的那个核上。**

   `ICH_*` 全都属于 CPU interface，是 per-CPU 系统寄存器。曾经只在
   `vm_create()` 里设一次，而那里跑在**调用者的核**上 —— 从 `/dev/vmm` 启动时
   那是用户态 helper 的 `write` 系统调用所在的核，SMP>1 时完全可能是另一个核；
   而 vCPU 任务被 `vcpu_task_create()` 钉在 CPU0。结果：`En=1` 落在 CPU1，
   guest 在 CPU0 上跑，`ICC_IAR1_EL1` 永远只返回 spurious，**一个虚拟中断都
   收不到**。SMP=1 时只有一个核，所以从没暴露。

   症状很有迷惑性（实测）：guest 照常启动、设备探测全部走完，然后停住；
   宿主侧看到 vtimer 注入计数一路涨，而 guest 的 `CNTV_CVAL` 再也不变 ——
   因为它压根没收到中断，自然没重编程。

   现在 `vmm_vgic3_hw_init()` 在**每次进入 guest 前**由
   `vmm_arch_restore_guest_ctx()` 调用（对同一核重复写是幂等的，几条指令）。
   `vm_create()` 里那个调用点已经删掉，只留注释。启动日志里那行
   `[vgicv3] ICH_HCR_EL2=… set on cpu=N` 就是给这类问题留的线索。

2. **LR 状态只在 VM exit 时回读。** guest ack+EOI 之后如果一直不 exit，软件里的
   `active` 位不会及时清掉，这一轮就不会重复注入同一个中断。目前靠 guest 频繁
   `WFI` 陷入（`HCR_EL2.TWI=1`）来兜底 —— 实测 guest 空闲时每秒数万次 exit，
   足够。**但如果将来 guest 进了长时间不陷入的忙循环，且中断是「电平型、会重复
   触发」的，就可能丢一次。** 正规做法是打开 `ICH_HCR_EL2.EOIEn` 走维护中断。

   这条限制现在有了一个**具体的实例**：vpl011 的 RX 中断就是电平型的（FIFO 里还有
   数据就该一直有效）。`aarch64_check_vpl011_rx()` 在每次进入 guest 前重拉 pending
   来模拟电平语义 —— 如果 guest 长时间不陷入，中断在 guest 应答后不会被重拉，
   输入要等到下一次 exit 才恢复。交互式场景下 guest 都在 WFI 等输入，不触发这个坑。

3. **`VMM 用 ICH_HCR_EL2.En=1` 期间，宿主自己的物理中断仍然正常**（QEMU 实测
   `[VMM] async exit type=1`），因为 `HCR_EL2.IMO=1` 把物理中断路由到了 EL2。
   这一点比 v2 配置好：v2 下宿主 tick 在 guest 运行期间是收不到的。

4. **`vmm_vgic3_sync_entry()` 每次进入 guest 都扫全部 1024 个 INTID**
   （32 个字 × 查位 + `lr_has_irq`/`lr_empty_slot` 各扫一遍 LR）。当前中断数
   极少，开销可以忽略；中断源变多时这里要改成维护「待注入队列」。

5. **死代码 / 未使用接口**（留着是为了和 v2 版 API 对齐，改动时注意别被误导）：
   - `vmm_vgic3_inject_timer()` —— 没有任何调用者，`el2_run.c` 直接调 `set_pending()`。
   - `gicv3_read_eisr()` / `gicv3_read_misr()` —— 定义了但没用（用的是 `ELRSR` + 回读 LR.State）。
   - `VGIC3_MAX_LRS=16` —— 实际按 `ICH_VTR_EL2.ListRegs+1` 走，QEMU virt 给 4 个。

6. **`_gicv3.nr_lrs` 依赖宿主驱动先初始化。** `vmm_vgic3_init()` 会读它来打日志、
   `sync_entry()` 也用它决定 LR 数量。`gicv3_init()`（`platform_init_runtime_drivers()`）
   早于 `vm_create()`，顺序是对的，但**不能调换**。

7. **guest 写 `ICC_SGI1R_EL1` 不被陷入**，会直接落到物理 GIC 上。1 vCPU 的 Linux
   不用它；将来上多 vCPU 或 guest 内部发 IPI 时必须补陷入逻辑（`HCR_EL2` 没有现成
   的开关，需要在 `handle_sysreg()` 里拦，或用 `ICH_HCR_EL2.TALL1` 之类）。

8. **两个口径不一致的地方**（不影响功能，但看日志时别困惑）：
   - `VGIC3_MAX_IRQS=1024`，guest 看到 `GICD_TYPER` 报 988 个 SPI；而**物理** GIC
     报 288 条线。
   - GICR MMIO 设备注册的窗口是 `0x20000 * 8`，但 guest DTB 只声明了 `0x20000`。

## 7. 怎么复现验证

```bash
# 切 GIC 版本不必再手动 clean：Makefile §7 的 _CFG_CHECK 会在检测到配置
# 变化（GIC=/SMP=/LOG=）时自动清掉已编译的目标文件。
make PLATFORM=qemu-virt-aarch64 GIC=v3 test-guest-linux -j8
# 期望：guest 打印 "Run /init as init process" 后出现 "~ #" 提示符，日志无 [ERROR]
# 交互验证：在 "~ #" 后键入 ls /、uname -a，应逐字回显并正常输出
#          （这同时验证了 SPI 33 的投递与宿主控制台输入桥）

# 反向验证（默认配置）
make PLATFORM=qemu-virt-aarch64 clean
make PLATFORM=qemu-virt-aarch64 test-guest-linux -j8
```

排查 guest 侧 GIC 行为时，QEMU 的 GICv3 trace 事件带参数，比读文档快得多：

```bash
qemu-system-aarch64 ... \
  -trace enable="gicv3_icv_*" -trace enable="gicv3_ich_*" \
  -trace enable="gicv3_cpuif_update" -trace enable="gicv3_cpuif_set_irqs" \
  -D /tmp/qtrace.log
```

- `gicv3_icv_iar_read` / `gicv3_icv_eoir_write` —— guest 有没有真的在收/结束虚拟中断
- `gicv3_ich_lr_write` —— VMM 有没有把中断排进 LR
- `gicv3_cpuif_update` 的 `HPPI update: irq %d group %d prio %d` —— **QEMU 内部**判定的
  最高优先级中断（`prio 255` = 判不出，此时中断一定送不到 PE）

## 8. 参考

- **guest 控制台（两种运行模式、`/dev/vmm` 协议、Ctrl+T k 退出）**：`docs/vmm/GUEST_CONSOLE.md`
- GICv2 版实现与注释：`../vgic/`
- 宿主 GICv3 驱动：`driver/irq/gicv3.c`（注意 `GICD_CTLR.EnableGrp1A` 那个坑）
- guest DTB：`imgs/guests/aarch64/linux-gicv3.dts`
- 平台配置：`platforms/qemu-virt-aarch64/platform.conf` 的 `irq` 块（`gicd` / `gicr`）
