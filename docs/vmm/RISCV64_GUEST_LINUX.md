# RISC-V 64 guest Linux（H-extension / VS-mode）

> 最后更新：2026-09-25
> 状态：guest 能启动到 root shell，CPU 密集压力下不再丢用户态（§7 是那次
>       随机 SIGSEGV 的根因分析）
> 对标实现：`docs/vmm/GUEST_CONSOLE.md` / AArch64 的 `el2_run.c` 那一套；
> 参考内核：`/home/ajax/Desktop/Project/Kernel/x-kernel` 的 `virt/kvmm`

## 1. 怎么跑

```bash
# 一键：编 rootfs（会装 imgs/guests/rv64/* → /guests/rv64）+ 编 GUEST_LINUX 内核 + 起 QEMU
make PLATFORM=qemu-virt-riscv64 test-guest-linux

# 或者分开：
make PLATFORM=qemu-virt-riscv64 GUEST_LINUX=1 kernel
make PLATFORM=qemu-virt-riscv64 rootfs
make PLATFORM=qemu-virt-riscv64 run-fs        # 需要内核已用 GUEST_LINUX=1 编译
```

起来之后是 guest 的 **root shell**（`/ #`）。

另一个入口是**宿主 shell 模式**，与 aarch64 完全同一条路径：不带 GUEST_LINUX
编译内核、`run-fs` 起来宿主 busybox，然后在宿主 shell 里跑 `/bin/vmm-run`
（它打开 `/dev/vmm`、发 `VMM_IOC_BOOT`，guest 的输出经 TX 通道回到 helper 的
stdout）。两条路都已实测：

```
/ # /bin/vmm-run
[vmm-run] guest started (vm1) — Ctrl+T ? for help
kylin-x login:
```

注意 `vmm-run` 是**静态链接**的（`apps/vmm-run-$(ARCH)`，由 `make … rootfs`
装进 `/bin`），所以不依赖 rootfs 里的动态 loader。

**不需要 `-cpu`**：QEMU 8.2.2 + OpenSBI v1.3 下，`-M virt` 默认 CPU 的 misa
里 H 位就是 1（OpenSBI 会打印 `Boot HART Base ISA : rv64imafdch`）。
换 QEMU 版本后如果 `hext_check_support()` 报错，加 `-cpu rv64,h=true`。

## 2. 架构分工

| 组件 | aarch64 | riscv64 |
|---|---|---|
| 二级地址翻译 | `stage2.c`（VTTBR_EL2） | `mm/riscv64/gstage.c`（hgatp, Sv39x4） |
| 进入 guest | `el2_vmcs.S`（eret / VHE） | `hext_vcpu.S`（sret / H-extension） |
| exit 分发 | `el2_run.c` | `hext_run.c` |
| 控制台设备 | PL011 @0x09000000（`vpl011.c`） | 16550A @0x10000000（`vuart16550.c`） |
| 中断控制器 | vGICv2 / vGICv3 | vPLIC（`vplic.c`） |
| 定时器 | `CNTV` + vtimer 注入 | SBI `set_timer` + `hvip.VSTIP` |
| 时钟源 | 系统计数器 | `hcounteren` 放开 `rdtime` |
| 引导协议 | x0 = DTB 物理地址 | a0 = hartid，a1 = DTB 物理地址 |

宿主侧的差异被收进 `include/vmm/vmm_console.h`：`/dev/vmm`（`vmm_dev.c`）和
VMM 主循环（`vmm_console.c` 的 `vmm_console_pump`）只调这一组转发函数，不再写
`#if ARCH_AARCH64`。加架构时改这一处即可。

guest 内存布局在 `include/guest_loader.h`（按架构挑选），DTB 修补逻辑
（`patch_dtb_memory` / `_initrd` / `_bootargs` / `nop_dtb_nodes`）两个架构共用。

## 3. guest 内存布局（RISC-V）

```
0xA0000000 + 192 MiB   guest RAM
0xA0200000             kernel Image（2 MiB 对齐）
0xA4000000             DTB
0xA8000000             initrd
```

**基址为什么不是 QEMU virt 惯用的 0x80000000**：

* `0x80000000` 起被宿主自己占着（kernel 0x80200000、rootfs 0x88000000..0x98000000）
* `0xC0000000` 以上宿主**没有建直接映射** —— `kernel/mm/riscv64/mmu.S` 的
  Sv39 只覆盖 `0x00000000..0xBFFFFFFF`，碰 `phys_to_virt(0xC0000000)` 直接缺页
* `0xA0000000..0xAC000000` 正好落在「已映射、且在 rootfs 之上」的空洞里，
  并在 `platforms/qemu-virt-riscv64/platform.conf` 的 `reserves` 里**预先占掉**。
  必须在那里占：等到启动 guest 时才 `pmm_mark_allocated` 的话，中间任何一次
  宿主分配都可能落进窗口，随后被 guest RAM 的 `memset` 静默踩掉。

> 顺带记一笔**未修的宿主侧隐患**：`mmu.S` 只映射了 1 GiB，而 platform.conf
> 声明 RAM 有 2 GiB，PMM 也按 2 GiB 管。目前分配是低位优先、够不到
> 0xC0000000 以上，所以没暴露；把 guest RAM 放在 0xC0000000 之前也正是不想
> 撞上它。要修的话是给 mmu.S 补 `L2[3]` 和高地址别名 `L2[0x103]`。

## 4. guest DTB（`imgs/guests/rv64/linux.dts`）

由 DTS 用 `dtc` 生成，**相对 x-kernel 原版改了三处**，缺一不可：

1. **PLIC 的 `interrupts-extended` 从 `<&cpu0_intc 11>` 改成 `9`**。
   11 是 M-mode 外部中断、9 是 S-mode。原版只声明了 M 上下文，而 Linux 的
   `irq-sifive-plic` 会跳过所有非 S 上下文 → `nr_handlers == 0` →
   `no PLIC context available` 探测失败，**一个中断都进不来**。
   改成 9 之后这台 PLIC 只有一个上下文（索引 0），正好对上 vPLIC 的
   「context N == vCPU N」。
2. **UART 补上 `interrupt-parent` / `interrupts = <10>`**。原节点没有任何中断
   属性 → Linux 把 8250 当轮询口：输出能出来，但**输入永远收不到**。
3. **bootargs** 去掉 `root=/dev/vda`（没有 virtio-blk），改成 initramfs。
   那句字符串同时是运行期补丁的**容量上限**（`guest_loader_patch_dtb_bootargs`
   是原地改写、不支持加长），所以故意留的是详细日志那一版（90 字节），
   运行时换成带 `quiet` 的短版本一定放得下。

`virtio_mmio@a000000` 节点在启动前被 `nop_dtb_nodes` 摘掉（VMM 没实现 virtio）。

## 5. SBI 表面

`handle_vs_ecall()` 实现了 guest 真正会用到的那一小块：

| 扩展 | 行为 |
|---|---|
| BASE | spec 0.2 / impl id `"AVTR"` / probe 支持 TIME、RFENCE、SRST、legacy 0x00 0x01 0x02 0x08 |
| TIME `set_timer` | 记 `vcpu->timer_deadline`，入口处按它置 `hvip.VSTIP` |
| RFENCE | 单 vCPU，直接回成功 |
| SRST | 回成功后 `EL2_VMEXIT`（guest 里 `poweroff` 就走到这） |
| legacy 0x01/0x02 | console putchar/getchar |
| **IPI / HSM** | **明确回「不支持」** —— 只有 vCPU0 一个 hart，报支持会让 guest 去启从核然后永远等不到 |

未实现的扩展一律回 `SBI_ERR_NOT_SUPPORTED`（不是崩溃），新版 Linux 探测
DBCN/CPPC/PMU 时会安静地换别的路径。

## 6. 移植时踩过的坑（都写进代码注释了，这里给索引）

按「重新实现一遍大概还会再踩一次」的顺序：

1. **binutils 会把 hypervisor CSR 的符号名静默映射到 VS 级编号。**
   工具链是 `-march=rv64gc`（不含 h），而 gcc 11.2.1 **拒绝**把 h 写进
   `-march`。于是 `csrw hedeleg` 汇编出来是 `0x202`(=vsedeleg)、
   `hstatus` 是 `0x200`(=vsstatus)、`hie` 是 `0x204`(=vsie) —— 编译链接全绿，
   运行期才炸。**本文件历史上的「写 hedeleg 会 illegal instruction」就是
   这条**（0x202 在 QEMU 8.2 里没实现）。规避：H 扩展 CSR 一律写数字地址，
   改完拿 `riscv64-linux-musl-objdump -d` 确认编码。
   见 `include/riscv64/hext.h` 顶部。
2. **`tp`（x4）是 per-CPU 指针，进出 guest 必须存/恢复。**
   `hext_enter_guest` 会把 guest 的 31 个 GPR 全部装载，`hext_trap_vector`
   却只恢复了 `ra/s0-s11/sp/gp`。少恢复 `tp` 的后果是宿主一回到 C 代码就
   拿 guest 的值索引 per-CPU 数据 —— 表现为 `klog_cpu_id` 取址缺页 + 陷在
   异常处理里的死循环，日志上一串 page fault，**看不出跟虚拟化有关**。
   见 `include/vmm/vmm.h` 的 `host_ctx` 说明。
3. **`hstatus.SPVP` 决定 sret 回 VS 还是 VU，必须按快照还原。**
   写死成 1 的话 guest 的用户态永远回不去：用户代码在内核态取指，U=1 的页
   内核态不可执行 → guest 报 `Unable to handle kernel access to user memory
   without uaccess routines`。配套地，**第一次进入 guest 时快照必须预置成
   SPVP=1**（`hext_vm_init` 里 `vcpu->hstatus_save = HSTATUS_SPVP`），
   否则首次 sret 落进 VU-mode，guest 一开机的特权指令全成非法指令。
   `sstatus.SPP` 也要跟着 SPVP 一起还原（HS 的 sret 会看它），
   见 `hext_vcpu.S` 第 7/8 步。
4. **`hedeleg` 必须委托给 VS-mode。** 不委托的话 guest 用户进程每次缺页都
   陷到 HS，而我们没有把 trap 反射回 VS 的代码，用户态一跑就死。
   `HEDELEG_COMMON` 里**不含** bit2：`hstatus.VTW=1` 时 VS-mode 的 WFI 报
   virtual instruction（cause 22），必须陷到 HS 才能换成宿主 yield。
5. **`hcounteren` 要放开 CY/TM/IR**，否则 guest 读 `time` 是非法指令，
   而 Linux RISC-V 的时钟源和 delay 循环都直接读它。
6. **虚拟 16550 必须实现 THR 空中断（IIR=0x02）。** Linux 的 8250 驱动只有
   收到它才会推进发送队列；缺了会让输出在第一批字符之后静默停住，且没有任何
   报错。`vuart16550.c` 的 LSR 恒置 THRE|TEMT、IIR 按 RX > THR 空 > 无 的
   优先级报，就是为这个。
7. **`handle_wfi` 不能无条件把 cause 2/22 当 WFI 跳过 4 字节。** 别的虚拟
   指令异常也是 cause 22，压缩指令只有 2 字节。现在会对不上 WFI 编码
   （`0x10500073`）就记一笔 `KLOG_WARN_SAMPLE`。
8. **控制台 vdev 的 `init` 必须保留宿主侧的 `tx_channel` 标志。**
   `/dev/vmm` 在启动 guest **之前**就把通道打开，而 `guest_loader_run_linux()`
   → `vm_create()` → `vmm_console_init()` → `uart16550_init()` 里那记整片
   `memset` 会把它清回 0。后果：
   * guest 输出不再进 TX 缓冲，而是经 klog 直打控制台（helper 模式下日志
     会和宿主的 `[INFO]` 混在一起）；
   * `vmm_console_pump()` 复活，**直接读真实 UART 抢宿主键盘**，和用户态
     helper 争同一个 FIFO。

   最典型的症状就是「**Ctrl+[ 之后回不到宿主 shell**」：内核侧 detach 明明
   成功了（日志两条都在），但按键仍被泵喂给 guest —— helper 已经退出，没人
   再读它的 stdin，于是宿主 shell 永远收不到输入。Ctrl+] 收不到也是同一个
   根因。

   这条 aarch64 早就踩过并修好了（`vpl011_init` 里那段带注释的保存/恢复），
   本架构的 `uart16550_init` 是重写时漏掉的 —— **改任何一个控制台 vdev 的
   `init` 都回头看另一个**。

9. **`vsepc` 是 guest 自己的 `sepc`，不是 VMM 的「恢复 PC」。** 两者混用会让
   随机出现的 guest 用户态 SIGSEGV。这条最反直觉（VMM 抢断时硬件把恢复 PC 放在
   **HS `sepc`**，`vsepc` 原封不动），单独写在 **§7**。

10. **HS 级 CSR 是 per-hart 的，而 `hext_vm_init()` 跑在 helper 那颗 hart 上。**
    helper 模式（`/bin/vmm-run`）下，VM 的 setup 全在**调用 ioctl 的那颗 hart**
    上做，而 vCPU 任务钉在 hart0 上（`task_set_cpu_affinity`）。下面这些只写了
    一处的 CSR 于是全是**复位值**：

    | CSR | 不补的后果 |
    |---|---|
    | `hgatp` | = Bare → **G-stage 形同关闭**：guest 直接访问宿主物理地址，MMIO 也不再陷入 |
    | `hedeleg`/`hideleg`/`hie` | guest 的缺页、VS 定时器/外部中断全不委托 |
    | `hcounteren` | guest 读 `time` 直接非法指令（Linux 的时钟源就是它） |
    | `hstatus.VTW` | VS-mode 的 `wfi` 不再陷入，宿主 yield 路径失效 |

    **症状**：SMP=1 永远正常、直启模式也正常，**SMP>1 时概率性起不来**（helper
    落在哪颗 hart 决定，≈50%）。修法：`vmm_arch_restore_guest_ctx()` 里加
    `hext_per_hart_csrs_ensure()`，每次进 guest 前在**真正跑 vCPU 的 hart** 上
    重设一遍；`rv_gstage_activate()` 做成幂等（hgatp 没变就不写、也不刷
    G-stage TLB），否则每次入口都白白 `hfence.gvma`。

    > 这是「每核/每 hart 一份的状态，初始化点必须和使用点同核」这一类的实例之一；
    > x86 上有三处同族问题（`IA32_FEATURE_CONTROL`、VMXON 区域、宿主 MSR 载入表），
    > 一并记在 `docs/bugfix/SMP_HELPER_MODE_BUGFIX.md` 与
    > `docs/vmm/X86_GUEST_LINUX.md` §9.10。**helper 模式 + SMP>1** 是唯一会
    > 暴露它们的组合，回归用 `tools/vmm_helper_regress.sh riscv64 6 2 --restart`。
    > 实测（2026-09-27，`run-net` helper 模式）：修前 SMP=2 约 1/2 起不来；
    > 修后 **SMP=2 启动 6/6 通过**。

## 7. VMM 抢断 guest 时的现场保存：三个同源 bug

这三个是同一类问题 —— **VS→HS 抢断绕过了 guest 自己的 `vstvec`，所以 guest
现场（CSR + GPR）的保存/恢复全靠 VMM，任何一处存错都会表现为「guest 随机
跑飞」**。症状都是偶发的，CPU 密集时更容易（抢断更频繁）。

症状归纳（同一根因的多种表现）：

```
# A：guest 用户进程收到 SIGSEGV，停在 trap 向量上
ls[95]: unhandled signal 11 at 0xffffffff80003358      ← vstvec + 4
epc : ffffffff80003358  status: 0000000200004020  cause: 0xc   ← SPP=0

# B：反过来，`epc` 是用户地址而 SPP=1（guest 内核跳到了用户地址）
epc : 00000000000c45f8  status: 0000000200004100  cause: 0xc

# C：guest 内核 oops，寄存器堆里 t0 == a0（值完全相同）
t0 : 00ffffff9eca20a2
a0 : 00ffffff9eca20a2
```

**回归验证**（修复后，标记用法见 7.6）：

| 压力组合 | 轮数 | 结果 |
|---|---|---|
| `cat /proc/cpuinfo; free; hostname; ls /` | 150 | 0 次 SIGSEGV / 0 Oops / 0 panic |
| 再加 `cat /proc/mounts; ls -la /; ls -la /proc; date; ps` | 1000 | 0 次 SIGSEGV / 0 Oops / 0 panic |

修复前同一台 guest 跑 20 轮就会出 2~3 次。

### 7.1 `x5` 被 `t0` 覆盖（最严重，最隐蔽）

```asm
csrrw   a0, sscratch, a0
csrr    t0, sscratch          /* t0 就是 x5 —— guest 的 x5 在这一刻没了 */
...
sd      x5,  VCPU_R0 + 5*8(a0)   /* ← 存进去的是 guest_a0，不是 guest 的 x5 */
```

`t0` 就是 `x5`，而保存 `x5` 的那条 `sd` 排在使用 `t0` **之后**，于是**每次抢断
都把 guest 的 `t0` 换成它的 `a0`**。

用户态受害有限（`t0` 是 caller-saved），**抢断落在 guest 内核里时才是致命的**：
guest 内核只会被自己的 `vstvec` 保存现场，VMM 抢断绕过了那一步，回来时它的
`t0` 已经是 `a0` 的值 —— 于是算出垃圾地址 / 跳到用户地址（症状 B、C）。

修法：`csrrw` 之后**立刻**存 `x5`，再动 `t0`；后面那条 `sd x5` 必须删掉
（那时 x5 已经是 guest_a0 了）。

> 参考实现 `/home/ajax/Desktop/Project/Kernel/x-kernel` 的
> `virt/kvmm/src/arch/riscv64/hext_vcpu.S` **也是这个顺序**，属于参考实现里
> 潜伏的 bug —— 比对时不要照抄这一处。

### 7.2 `vsepc` 不是「恢复 PC」

`vsepc` 是 **guest 自己的 `sepc`**：guest 自己的陷阱（VU→VS）由硬件往那里写
现场，guest 的 `sret` 回用户态读的也是它。而 VMM 从 guest 抢断时，硬件把
「被打断的那条指令」放在 **HS 的 `sepc`**，`vsepc` 原封不动 —— 这正是 H 扩展
把它拆成两个 CSR 的原因。

原来的陷阱向量写了这么一行：

```asm
csrr t0, sepc
sd   t0, VCPU_VSEPC(a0)   /* ← 覆盖 vsepc */
```

于是**每次抢断都把 guest 尚未消费的陷阱现场冲掉**。触发条件是「抢断正好落在
guest 处理程序入口」：那时 guest 才被送进 `vstvec`、还没读走 `vsepc` 里的用户
PC，被覆盖之后 guest 的 `sret` 就跳到了 VMM 的恢复点上（症状 A）。

修复（对齐 x-kernel）：`vcpu_t` 里用独立字段 **`pc`** 存恢复 PC，只写回
**HS `sepc`**；`vsepc` 与 `vsstatus` **原样保存、原样恢复**。C 侧所有「被打断/
恢复 PC」语义的 `vcpu->vsepc` 全部改用 `vcpu->pc`。

| | x-kernel | 修复前 | 修复后 |
|---|---|---|---|
| 恢复 PC | 独立字段 `VCPU_PC` → HS `sepc` | 写进 `vsepc` ✗ | 同 x-kernel ✓ |
| guest 的 `vsepc` | 原样保存/恢复 | 被恢复 PC 覆盖 ✗ | 原样 ✓ |

### 7.3 `htval` 不是 `htinst`

MMIO 模拟在「从 guest PC 取指失败」时有一条回退：

```c
if (vcpu->htval_save != 0) {        /* ✗ htval 是出错 GPA>>2，不是指令 */
    *inst_out = vcpu->htval_save;
```

指令在 **`htinst`（0x64a）** 里，而且我们**压根没保存它**（x-kernel 有
`VCPU_HTINST`）。这条路径会把一个地址当指令解码，**解出的长度/寄存器号全是
垃圾，并据此推进 `vcpu->pc`**（还有可能写错目标寄存器）。修法：补上 `htinst`
保存，回退改用它 —— 解码失败就 `EL2_EXIT`，绝不猜。

### 7.4 附带：`hie` 不是可选项

`hie` 要置 `VSTIE|VSEIE`，与 x-kernel `hext_init()` 的
`csrs hie, HIE_VSEIE | HIE_VSTIE` 一致。它管的**只**是「已委托给 VS 的中断，
在 guest 当时接不了时（例如正跑在处理程序入口、`vsstatus.SIE=0`）怎么办」——
置位则升到 HS 让 VMM 抢断。**留 0 会让这类中断一直挂在 `hvip` 里，guest 的
定时器从此不再推进（实测 guest 完全起不来）**。抢断本身是安全的，前提就是
7.1~7.3 这三处现场保存都对。

### 7.5 固化：偏移用编译期断言钉死

`include/vmm/vmm.h` 现在用 `_Static_assert` 把 `vcpu_t` 的每个偏移钉到
`hext_vcpu.S` 的 `VCPU_RV_*` / `HCTX_*` 宏上。这套偏移只靠注释约定过很久，
加一个字段就会整体错位（历史上 `tp` 槽位就漏加过一次，见 §6 第 2 条），而
错位的表现正是「guest 随机跑飞」这类极难反查的故障。

> **踩坑记录（构建）**：调试期间在两次 `GUEST_LINUX=1` 编译之间插了一次不带
> `GUEST_LINUX` 的 riscv64 编译，`.d` 不被 `-include` 导致半新半旧的 `.o`
> 混链，症状是「guest 一个字符都不输出」—— 与代码无关。**切 CFLAGS 必须
> 先 `make clean`**，见 CLAUDE.md。

### 7.6 怎么验证这类修复（验证方法本身也踩过坑）

压力测试的结束标记**不能**用回显里也会出现的字符串。串口是带终端回显的：
你发过去的命令会原样回显一遍，所以 `... done; echo HEAVY-DONE` 在日志里
**命令回显**和**命令输出**各出现一次 —— 采集脚本一看到回显就以为跑完了，
提前退场，循环其实还在跑。看起来「0 失败」，其实什么都没测到。

正确做法是让输出与回显**可区分**：

```sh
# 回显里是字面 "$i"，输出里才是展开后的 1000
i=0; while [ $i -lt 1000 ]; do ...; i=$((i+1)); done; echo "BIGSOAK-$i-token"
#                                                         ^ 等 BIGSOAK-1000-token
```

另外，起 QEMU 前先验产物（见 7.5 的构建踩坑）：

```bash
strings build/qemu-virt-riscv64/kernel_riscv64.bin | grep -c 'GUEST_LINUX mode'   # 必须是 1
```

## 8. 同内核多 VM（每 VM 一份 G-stage + 按需分页）

对照 aarch64 侧同名改造做的，两边现在是**同构**的（G-stage ↔ stage-2，
hgatp ↔ VTTBR_EL2）。手写 G-stage 的就是 `kernel/mm/riscv64/gstage.c` 顶部
那段注释，这里只记要点和踩过的坑。

### 8.1 跑法

```bash
make PLATFORM=qemu-virt-riscv64 clean && make PLATFORM=qemu-virt-riscv64 kernel rootfs
tools/vmm_multivm_regress.sh riscv64 1        # vm1 → Ctrl+T d → vmm-run -n → vm2
```

宿主 shell 里就是两条命令：`vmm-run` 起 vm1，`Ctrl+T d`（0x14 0x64）把它留在后台，
`vmm-run -n` **新建** vm2。两个 guest 各自跑自己那份 Linux。

### 8.2 每 VM 一份的东西

| 从前（文件级 static，只能有一个 VM） | 现在 |
|---|---|
| `g_gstage_root[2048]` / `g_gstage_l1[4][512]` | `vm->gstage`（静态表按 slot 索引，`GSTAGE_MAX_VMS=4`） |
| `g_rv_mmio_bus` / `g_rv_uart_dev` / `g_rv_plic_dev` | `vm->mmio_bus_storage` / `vm->uart_dev` / `vm->plic_dev` |
| `uart16550.c` 的 `g_uart16550` + 锁 | `vm->uart16550` / `vm->uart16550_lock`（riscv 与 x86 共用，两边都改了） |
| `vplic.c` 的 `g_vplic`（**连锁都没有**） | `vm->vplic` / `vm->vplic_lock` |
| `g_guest_stack[MAX_VCPUS][4K]`（按 vcpu_id 索引 ⇒ 两个 VM 的 vcpu0 共用一块栈） | `vm->guest_stack[]` |
| `rv_gstage_activate()` 不收参数 | `rv_gstage_activate(&vm->gstage)` —— hgatp 不只是 per-hart，还是 **per-VM** |

最后一条是关键：`hext_per_hart_csrs_ensure()` 从前不收 vcpu，于是**只在
helper 所在的 hart 上**补 hgatp。多 VM 之后同一颗 hart 上时间片轮转两个
VM 的 vCPU 任务，每次入口都必须把 hgatp 换回本 VM 的 —— 不换就是"guest 拿着
另一个 VM 的 G-stage 在跑"，两个 VM 互相看见对方的内存。改签名时顺带把
vcpu 传了进去（`hext_per_hart_csrs_ensure(vcpu)`）。

### 8.3 按需分页

`rv_gstage_vm_init()` 建出来的是**空表**（"空表即全 trap"，所以
`rv_gstage_enable_mmio_trap()` 整个删掉了）：guest RAM 不再预映射、
不再需要 `platform.conf` 里的 192 MiB `guest_ram` 预留（那段也删了）。
guest 首次访问某页 → G-stage fault（scause 20/21/23）→
`handle_ram_fault()` 分配一整块 2 MiB 并装映射 → 返回 `EL2_RESUME`；
**非 RAM** 的才走 `handle_gstage_fault()` 交给 MMIO 总线。

宿主自己要写的那几块（内核映像/DTB/initrd）在加载期由
`guest_loader.c` 的 `vm_gpa_ptr()`/`vm_write_guest()` 显式映射 —— 那是宿主
碰 guest 内存的**唯一**入口。

### 8.4 ★ 三个坑

**① `PTE → PA` 必须移位，不能像 ARM 那样 `pte & ~0xFFF`。**
ARM 的 LPAE 把物理地址原样放在 `pte[47:12]`；RISC-V 存的是 **PPN** 在
`pte[53:10]`。直接抹低位得到的是 `pa >> 2`。两个症状都不指向"翻译错了"：

- 查表返回偏小的 PA ⇒ 宿主从**错误的物理页**取指 ⇒
  `undecodable MMIO inst=0x2781`（看着像指令解码器坏了）；
- 释放路径按错地址 `pmm_free` ⇒ 满屏 `PMM: invalid free address: 0x204xxxxx`
  （这些地址连 RAM 都不在），而真正的页一页都没还回去。

修法是 `PTE_TO_PA()` 宏（`gstage.c`）。**跟 aarch64 的 stage2.c 逐行对照时
尤其容易栽在这条上** —— 那边 `t[j] & ~0xFFF` 是对的。

**② 根页表必须 16 KiB 对齐，且每个 VM 独占一个 16 KiB 块。**
hgatp 的 PPN 低两位被硬件忽略，对齐不够时"映射加到了 A 表，硬件在用 B 表"。
用 16 KiB 的 `gstage_root_page_t` 补满 + `aligned(16384)`，与 aarch64 的
`s2_l1_page_t` 同一手法。

**③ 空表也要把 root → L1 那几条装上。**
那几条是"表结构"不是"映射"。漏掉时软件侧（`l0_ensure`/`map_page`/`lookup`）
看上去**完全正常**，但硬件从 root 就走进了死胡同 —— guest 在同一条取指上
无限重复 fault。

### 8.5 附带修掉的两处（都不是本架构独有）

- **`kernel/mm/riscv64/mmu.S` 只直接映射到 `0xbfffffff`**，而 PMM 按 2 GiB
  管（`ram = 0x80000000 + 0x80000000`）。PMM 一旦分配到 `0xc0000000` 以上，
  `phys_to_virt()` 就踩到不存在的映射 → 宿主缺页 → **整机挂死**。
  改成按需分页后 guest 的每一页都从 PMM 现拿，多开几个 VM 就能推到那条线，
  所以补上了 `L2[3]` / `L2[0x103]`。旧版本没炸只是因为分配没推到那儿。
- **`kernel/main.c` 的 `#include "guest_loader.h"` 只在 `ARCH_AARCH64` 分支里**：
  riscv/x86 的 `GUEST_LINUX=1` 变体因此报
  `implicit declaration of 'guest_loader_run_linux'`（现代 GCC 当错误），
  两条直启路径其实一直是编不过的。两个分支都补上了。

### 8.6 日志量

`rv_gstage_activate()` 每次进入 guest 前都会被调一次，多 VM 时每轮都在两个
VM 的 hgatp 之间来回切 —— 那行 `hgatp=... activated` 若是 INFO，实测会占掉
整个日志的 **81%**（670/825 行）。已经降成 DEBUG
（klog 规范里「INFO 不得进循环体」的典型案例）。

## 9. 相关

- 两种运行模式、`/dev/vmm` 协议、Ctrl+T 前缀键语义：`docs/vmm/GUEST_CONSOLE.md`
- 裸跑基线怎么做：`docs/vmm/GUEST_NATIVE_QEMU.md`
- 多 VM 的验收脚本：`tools/vmm_multivm_regress.sh`（riscv64/aarch64/x86_64）
- H-extension 陷阱委托与特权级：RISC-V Privileged Spec 1.12 §19-20
