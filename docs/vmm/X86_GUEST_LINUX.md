# x86_64 guest Linux（VMX / EPT）— 已跑通到 guest shell

> 最后更新：2026-09-25
> 状态：**已跑通**。guest Linux 6.2.15 在 avatar 的 x86_64 VMM 下启动到
>      交互式 shell（busybox ash），串口控制台可用、时钟正常、无 watchdog 告警。
>      实测会话见 §7。
> 对标实现：`docs/vmm/RISCV64_GUEST_LINUX.md`、`docs/vmm/GUEST_CONSOLE.md`

## 1. 怎么跑

```bash
# 同一变体，顺序不能反（make rootfs 会重编内核，见 CLAUDE.md 的警告）
make PLATFORM=qemu-virt-x86_64 GUEST_LINUX=1 rootfs
make PLATFORM=qemu-virt-x86_64 GUEST_LINUX=1 kernel
strings build/qemu-virt-x86_64/kernel_x86_64.bin | grep -c 'GUEST_LINUX mode'   # 必须是 1
qemu-system-x86_64 -machine q35 -enable-kvm -cpu host -smp 1 -m 2G \
  -display none -serial stdio \
  -kernel build/qemu-virt-x86_64/kernel_x86_64.bin \
  -device loader,file=build/qemu-virt-x86_64/rootfs-x86_64.img,addr=0x4000000,force-raw=on
```

`-enable-kvm -cpu host` 是硬前提（TCG 不模拟 VMX）。本机已验证 `/dev/kvm` 可用。

**裸跑基准**（用来区分「镜像问题」还是「VMM 问题」，非常重要）：

```bash
qemu-system-x86_64 -enable-kvm -cpu host -m 1G -display none -serial stdio \
  -kernel imgs/guests/x86_64/bzImage -initrd imgs/guests/x86_64/initrd \
  -append "console=ttyS0 earlycon=uart8250,io,0x3f8 rdinit=/init nox2apic \
           no_timer_check tsc=unstable irqpoll pci=conf1 pci=nomsi acpi=off"
```
**这份镜像裸跑能直接进 `~ #` shell** —— 所以任何失败都先怀疑 VMM，不要怀疑镜像。

## 2. 与另两个架构的结构差异

| | aarch64 / riscv64 | x86_64 |
|---|---|---|
| guest 物理基址 | 与宿主物理地址**同一块**（identity） | **GPA ≠ HPA**：guest 必须从物理 0 开始，宿主另找窗口（`GUEST_X86_HPA_BASE`） |
| 引导协议 | raw Image + DTB | bzImage + zeropage/E820 + 长模式入口 |
| 控制台 | MMIO PL011 / MMIO 16550 | **PIO 16550 @0x3F8**（I/O bitmap 拦） |
| 中断 | vGIC / vPLIC | vLAPIC（xAPIC MMIO，无 x2APIC）+ MSR load/save list |
| 虚拟化扩展 | VHE / H-extension | VMX + EPT |

## 3. 已经做完并**实测验证**的部分

- **VMX 底座**：VMXON/VMCS/VMLAUNCH；MSR bitmap（全拦 + 影子模拟）、I/O bitmap（全拦）、
  异常位图、事件注入框架（`VM_ENTRY_INTR_INFO` + interrupt-window exiting）、
  真 INVEPT（`.byte` 传统编码 `66 0F 38 0x80`，**没有 VEX 形式**）
- **vLAPIC**：`kernel/vmm/vdev/vlapic.c`（xAPIC 寄存器组 + 定时器 + ISR/EOI/TPR/SVR/ICR），
  MMIO 靠 EPT「非 RAM 置无效」自然陷入，不需要 APIC-access page
- **PIO 串口**：`x86_pio_handle()` 复用 MMIO 版 16550 的寄存器状态机；
  PIC/PIT/RTC/PCI 端口桩（给 Linux 探测一个自洽的「什么都没有」视图）
- **CPUID 模拟**：保守 CPU（SSE2 级、单核、无 XSAVE/AVX/VMX，有 APIC/TSC/PAE/NX/SYSCALL）
- **引导协议**：bzImage 解析（HdrS 校验）、E820、boot_params、临时页表
  （identity + 高半区 `0xffffffff80000000+X→X`）、GDT/空 IDT/空 TSS、长模式直入 64 位入口
- **入口路径已证明健康**（见 §5 的探针法）：长模式、guest 页表、EPT、COM1 PIO、`%rsi` 取值全部正常

## 4. 修掉的硬 bug（都是「不修就走不通」级别）

1. **`vmcs_init_host` 把 `HOST_BASE_FS/GS` 写成 0** —— 每次 VM-exit 用 0 装回宿主的
   per-CPU 指针，宿主任何 syscall 都在入口 `push %gs:0x60` 缺页到地址 0x60。
   已改为读真实值，并加 `vmx_refresh_host_state()`（CR0/CR3/CR4/FS/GS）**每次入口前刷新**。
2. **`CPU_EPT` 按能力位无条件开、EPTP 却没写** —— guest 每次取指都 EPT violation，
   而 exit 48 的处理是「解码不成就跳过指令」，看起来像设备没实现，实际 guest 在乱跑。
   现在 `want_ept` 跟着 `vm->cfg.mem_size != 0` 走。
3. **MSR load/save list 的地址字段编码错**（写成 0x2012/0x2016；权威值 `0x2008`/`0x200a`，
   查 `arch/x86/include/asm/vmx.h`）。症状极具误导性：guest 进去出不来、**没有** VM-entry
   failure、exit handler 一次都没进。
4. **guest 页表项里写了宿主物理地址** —— guest 页表里的地址必须是 **GPA**。
   判据：EPT violation 报出来的 gpa 落在宿主窗口（0x2xxxxxxx）里。
5. **INVEPT 编码**：只在传统形式 `66 0F 38 80 /r` 存在，VEX 形式直接 #6 invalid opcode。
6. **GDTR/IDTR/TR 的 base 用了宿主内核虚拟地址** —— 开 EPT 后按 GPA 检查，第一次 entry
   就被 reason 33「invalid guest state」拒绝。三者都要指向 guest 内存里的合法 GPA。
7. **CR4.VMXE**：它在 VMX 操作期间是**固定位**，`GUEST_CR4` 里清掉会 reason 33；
   但 guest 看到的 CR4 又不能有它（Linux 早期启动会清，non-root 下清固定位 → #GP →
   无 IDT → triple fault）。正解是 `CR4_MASK`(1<<13) + `CR4_READ_SHADOW` + CR 陷入处理。
8. **误关 `CPU2_UNRESTRICTED_GUEST`** —— 没有它，guest 清 CR0.PG 就 #GP。
9. **`GUEST_BASE_GS` 从不更新** —— guest 写 `wrmsr GS_BASE` 只进了影子，没写回 VMCS 字段
   （FS/GS base 同时是 VMCS 字段）。症状是 per-CPU 访问全落到地址 0。
10. **初始栈地址少两个数量级**（0x00BFF000 = 12.5 MiB，落在内核解压区里被覆盖）。
    现在放 190 MiB。

## 5. 调试方法（可直接复用）

### 入口探针 stub（最有效）
`kernel/vmm/x86_64/guest_boot.c` 里 `#define GUEST_X86_ENTRY_PROBE 1` 打开：
入口指向一段自写的机器码（GPA 0x9B000），**用「有没有 `reason=30`（PIO exit）」当信号灯**，
能在**不改 VMM 逻辑**的前提下验证任何寄存器/内存假设。靠它已经证明：

- `outb` → COM1 的通路正常（长模式/页表/EPT 都活着）
- `%rsi` 到达入口时确实是 `0x70000`（boot_params）
- `0x70000` / `0x2000000` / `0x2500000` 三个地址**都可读** → 内存映射没有洞

### 其他诊断开关（都在 `vmx.c`，默认关闭）
`ENTRY-DBG`（入口 VMCS 全状态）、`RIP-SAMPLE`（每 2000 次 exit 采 RIP + 原因直方图）、
`STUCK-RIP`（dump guest 内存字节）、`EXC-DBG`/`EXC-STACK`（每个异常 vector 的首次
现场含 **CR2**/error code/栈 —— 注意：VM-exit 不保存恢复 CR2，所以退出后读到的就是
guest 的值）、triple-fault dump、boot_params 字段日志。

### 符号化 guest 地址
guest 内核是 identity 运行在 `CONFIG_PHYSICAL_START=0x1000000`，
所以 **VA = 0xffffffff80000000 + 物理地址**，对 `System.map` 查即可。
若落在 `_end` 之后（`init_size` 工作区），那可能是**解压器自己**——它重定位到
`0x1000000 + init_size - <自身大小>`，用 `arch/x86/boot/compressed/vmlinux` 的符号表查。

## 6. 早期阶段修掉的 bug（历史记录）

### 6.1 `g_rip` 只在探针分支里赋值 ⇒ 关掉探针 guest 从地址 0 起跑
`guest_boot.c` 里 `vcpu->g_rip = ...` 原来写在 `#if GUEST_X86_ENTRY_PROBE` 里面。
`#define ... 0` 之后 `g_rip` 保持 0，guest 从 **GPA 0** 开始执行，第 1 次 VM-exit 就是
triple fault（`reason=2 rip=0x10000`），串口一个字都没有 —— 看上去像"整个 VMM 没起来"。
**症状与"VMX 坏了"完全一样，实际只是入口没设。** 现在入口在 `#if` 之前无条件设置。

### 6.2 `earlyprintk` 加到了错误的架构块
`include/guest_loader.h` 每个架构一个 `GUEST_LINUX_BOOTARGS`；用脚本 `replace()` 时
命中了**第一处**（aarch64/riscv64 的注释），x86 那条没改到 ⇒ 解压器的 `console_init()`
拿不到 `earlyprintk=`，`early_serial_base` 保持 0，**`extract_kernel` 里所有
`debug_putstr` 全被抑制**。改 x86 块后 guest 立刻开始报进度。
> 教训：改这种"每架构一份"的头文件，必须按行号/块确认，不能只靠字符串替换。

### 6.3 本轮达成的里程碑：**引导环境已证明与 QEMU 裸跑逐字节等价**

```
early console in extract_kernel
input_data: 0x29742e0   input_len: 0xaa8318
output: 0x1000000       output_len: 0x23d79f8
kernel_total_size: 0x2228000   needed_size: 0x2400000
trampoline_32bit: 0x9d000
```
**这 7 个值和 `imgs/guests/x86_64/bzImage` 裸跑时打印的完全相同** —— 说明 zeropage /
E820 / 命令行 / initrd / 页表 / 长模式入口 / 页表重定位几何**全部正确**。
再往后的失败**不是引导参数问题**，不要再往那个方向查。

## 7. 跑通的证据（实测会话）

```
=== X86 GUEST LINUX BOOTED (Avatar VMM) ===
Linux (none) 6.2.15 #2 SMP PREEMPT_DYNAMIC Fri Sep 25 18:19:37 CST 2026 x86_64 GNU/Linux
model name	: 06/0f
--- 自由内存 ---
              total        used        free      shared  buff/cache   available
Mem:         159332        8468      137620        5500       13244      140612
Swap:             0           0           0
/bin/sh: can't access tty; job control turned off
~ # uname -a
Linux (none) 6.2.15 #2 SMP PREEMPT_DYNAMIC Fri Sep 25 18:19:37 CST 2026 x86_64 GNU/Linux
~ # cat /proc/cpuinfo | grep -m1 'model name'
model name	: 06/0f
~ # ls /
bin   dev   etc   init  proc  root  sys   tmp
~ # cat /proc/uptime
256.57 252.85
~ # exit
[  268.049778] Kernel panic - not syncing: Attempted to kill init! exitcode=0x00000000
```

> `exit` 触发的 panic 是**正确行为** —— PID 1 退出时 Linux 一定 panic。

想自己打字进去交互：QEMU 用 `-nographic`，stdin 直通 guest 串口，所以
把输入喂给 `make` 即可（注意 guest 起来要几十秒，早期输入会被 shell 读走前先堆在 tty 缓冲里）：

```bash
( sleep 60; echo "uname -a"; sleep 5; echo "ls /"; sleep 20 ) \
  | make PLATFORM=qemu-virt-x86_64 LOG=warn SMP=1 test-guest-linux
```

建议用 `LOG=warn`：宿主内核的日志和 guest 串口**共用同一个 UART**，
`LOG=info` 的 `[RIP-SAMPLE]` 会把 guest 的输出冲得七零八落。

### 7.1 当前已知的遗留问题（不挡使用，但该修）

| # | 问题 | 影响 | 方向 |
|---|---|---|---|
| 1 | **HLT 空转**：guest 的 `default_idle` 是 `sti; hlt`，我们每次退出只步进 RIP + yield，于是空闲时以 ~2.8 万次/秒的 VM-exit 打转 | 性能（实测一轮 420s 里 1170 万次 HLT 退出） | 需要「vcpu 阻塞到有事件」的原语；`kernel/task` 目前只有 `task_yield()`，没有 sleep/等待队列 |
| 2 | **TSC 频率来自宿主 PIT 标定**（2417 MHz，真值 2419.2 MHz，差 0.09%） | guest 时钟每天漂 ~1 分钟 | 宿主侧改用 CPUID 0x15 真值（QEMU/KVM 会抹掉，需从别处取） |
| 3 | `[VMX-DBG]`/`[RIP-SAMPLE]`/`[HLT-*]` 等 TEMP-DBG 未清 | 噪声 | 见 §8.13 |
| 4 | 宿主无 FP/SIMD 上下文保存（`fxsave`/`fxrstor`） | guest 一旦真用 SSE/AVX 会踩 | 见 §7.2 旧第 4 条 |

## 8. 解压器阶段的新增结论（历史记录，结论仍然有效）

### 8.1 修掉的第三个硬 bug：CPUID 的 LM 位放错寄存器
`vmx_cpuid_emulate()` 里把 **长模式支持**写进了 `EAX`：
```c
case 0x80000001:
    *eax = (1u << 29);      /* ✗ LM 是 EDX[29] */
```
后果：guest 看到的 CPU「不支持长模式」⇒ 内核 `startup_32` 的 `verify_cpu`
（`arch/x86/kernel/verify_cpu.S`）检查不过 ⇒ **直接跳进 `hlt` 死循环**。
实测 235 万次 exit reason=12、RIP 原地不动、串口一字不发 —— 现象像「VMM 坏了」，
极难反推。已改为 `*edx = SYSCALL(11) | NX(20) | LM(29)`。

### 8.2 入口改用 **32 位引导协议**（对标 tgoskits `linux_boot.rs`）
不再走 64 位直启。现在是：32 位保护模式 + `CS=0x10/DS=0x18` + `CR0=PE|NE|ET`
（无分页）+ `esi = boot_params` + `eip = code32_start`，**其余全交给内核自己的
`startup_32`** —— 与 QEMU 裸跑同一条路。判定用 `g_cr3 == 0`（非零 = 老的 64 位直启）。
两个 VM-entry 细节（都实测踩过）：
- **32 位保护模式段的缓存 AR 必须带 accessed 位**：`0xC09B` / `0xC093`，
  写成 GDT 描述符里的 `0x9A`/`0x92` 会被 reason 33 拒掉；
- `ENT_LOAD_EFER` **不要清**：`EFER=0x801`（SCE|NXE、LME=0）对 32 位 guest 合法。

### 8.3 zlib 在这条链上的真实行为（读源码 + 实测读 guest 内存得到）
- 这个内核的 `struct inflate_state` **没有 `sane` 字段**（那是上游 zlib 的）；
- `__gunzip()` 在 preboot（`flush == NULL`）路径上**故意**把
  `state->window = NULL`、`wsize = 0`（`lib/decompress_inflate.c:140`）——
  因为 `workspace` 只 malloc 了 `sizeof(struct inflate_state)`，
  `working_window` 是越界指针。**所以 `whave == 0`，窗口路径根本进不去**；
- 于是 `inflate_fast` 里 `movzwl (%rbp,%rcx,2)` 这种 16 位带 scale-2 的访问
  只可能来自**直接拷贝块**（`from = out - dist`，按 `unsigned short` 拷）。
  实测 `%rbp = -2`、`%r11 = out` ⇒ **`dist = out + 2 ≈ 1950 万`**，而 deflate
  的回引上限是 32 KB ⇒ **解码从某点开始读错了位流**。
- `free_mem_ptr` 是堆**基址**、不随分配移动（真正 bump 的是 `malloc_ptr`），
  所以「它没动 ⇒ 没 malloc」的推断是错的：`__gunzip` 确实 malloc 了
  `strm`（在 `_bss`）和 `state`（紧随其后）。

### 8.4 当前状态：**非确定性**（最新、也是最关键的线索）
解压**确实在推进**（不是零产出 —— 之前看 `rdi=0x1000000` 是被寄存器复用误导）：
```
next_in/n ext_out 实测： 327KB→2.7MB、→3.4MB、→3.7MB（三次运行各不相同）
```
**解压是确定性的**（同输入同代码必同输出）⇒ 进度不同 ⇒ **有外部扰动在改 guest 的
架构状态**。已排除的扰动源：
- **宿主踩 guest 内存**：在 guest 不会碰的两处（GPA 0x4000 / 0x5000000）埋了哨兵，
  卡住时**完好无损** ⇒ 排除；
- **宿主 PMM 是否真的预留了 guest RAM**：`pmm_mark_allocated()` 确实被调用，
  且哨兵验证通过 ⇒ 预留有效。
⇒ **唯一剩下的时间相关输入是宿主 tick 的那次 VM-exit/entry 往返。**
下一步就是拿它做变量：把 EXTINT 分支里的宿主侧工作（`task_yield()`、采样打印）
暂时清空再跑两次，看进度是否变成确定性 / 是否能跑完。

### 8.6 ★ 根因定性：guest 自己的**缺页风暴**（不是 VMM 卡死）

实测在 guest 内部计数（`EXC_BITMAP=(1<<14)` 只数不注入）：
**35 秒 > 100 万次 #PF**（约 3 万次/秒），全部是
`rip=rva 0xAA9290`（`inflate_fast` 里 `movzwl 0x0(%rbp,%rcx,2)`）、`err=0x0`。

**机制**（这条把困扰了好几个 session 的 `cr2=0` 彻底解释清楚了）：
- 出错地址是 `from = out - dist`，实测 `%rbp = 0xFFFFFFFFFFFFFFFE` —— **非规范地址**；
- **SDM：非规范地址引起的 #PF 不更新 CR2** ⇒ CR2 保留**上一次**的旧值 `0`；
- guest 的 `do_boot_page_fault()` 读 `cr2` 去补身份映射 ⇒ **补的是错的那一页**；
- 重试仍缺页 ⇒ 无限循环 ⇒ **解压被拖慢约 2400 倍**（不是卡死，是每 33 µs 缺一次页）。

**所以「解压器卡在 inflate_fast」这个结论要改口径**：解压**一直在推进**
（实测 30 秒推进 2.7–3.7 MB，随宿主负载波动 —— 之前的「不确定性」是
「固定墙钟下取快照」的测量假象，不是真的非确定）。

**下一步（按优先级）**
1. **抓第一次缺页**（不是稳态）：`do_boot_page_fault` 的 `cr2` 在非规范故障下不可信，
   所以要看**故障 RIP + 该处寄存器**，对照 `inflate_fast` 的
   `dist <= op` 直接拷贝块，判断 `dist` 是从哪一步开始变野的。
2. 顺着 `state->mode` 走：实测采样到过 `mode=18`（`CODELENS`，正在建 Huffman 表），
   且 `lencode/distcode` 指向 `.bss` 里的 `codes[]`、`lenbits/distbits` 正常 ⇒
   建表阶段是健康的，**要往建表之后找**。
3. 若确认 `dist` 合法（`dist <= op`）却仍读到界外，则重点查
   **`strm->next_out` / `op`（32 位截断）与 `beg` 的一致性**。

### 8.7 ★★ 矛盾点已收窄到具体字段：`strm->next_out` 被写坏

在 **缺页现场**（不是 tick 采样）抓到的寄存器：
```
rip=0x341d290  cr2=0x0  err=0x0
r8=0x45e3(17891=dist)  rbp=0xffffffffffffba21  r11=0x4
rdx=0x5(len) rdi=0x2(len>>1) r12=0x3422e40(strm) r14=0x3422ea0(state)
```
对照反汇编 `aa9253: mov %r11,%rbp / aa9256: sub %rcx,%rbp` ⇒
`-0x45DF = out - 17891` ⇒ **`out = 4`**。

而 `%r11` 是 `inflate_fast` 从 **`strm->next_out`** 载入的局部量
（`aa8fe4: mov %rdi,%r12` 把 `strm` 存进 r12）。

**同一个 `strm->next_out` 字段，两个时刻读到的是：**
| 时刻 | 值 |
|---|---|
| tick 采样（解压中途） | `0x134a02a`（已产出 2.7 MB，合理）|
| 缺页现场 | **`4`** |

⇒ **这不是「算法读错位流」，而是 `z_stream` 结构体本身被写坏了**——
而 `strm` 恰好躺在 `_bss`（= `boot_heap`）的**开头**，也就是解压器所有
`malloc` 的起点；紧接着（+0x60）是 `struct inflate_state`（≈9.6 KB）。

**下一步（按优先级）**
1. **在缺页现场直接读 `strm` 的每个字段**（0x3422e40 起 0x60 字节），
   与 `__gunzip` 刚设好的值逐个对比，找出**先坏的是哪个字段**。
2. 判断是「一次性写坏」还是「反复写坏」：在**第一次**缺页现场与**第 100 万次**
   各读一次，若一致 ⇒ 早期一次性损坏；若不同 ⇒ 有东西在持续写。
3. 嫌疑最大的是**堆与 `.bss` 的重叠**：`boot_heap` 是 `.bss` 的**起始符号**，
   `malloc` 从它开始分配；若链接布局把 `.bss` 里其它变量排在 `boot_heap`
   之后而没有留出 `BOOT_HEAP_SIZE`（0x10000）的洞，`strm`/`state` 就会和
   它们互相覆盖。**核对 `.bss` 里 [boot_heap, boot_heap+0x10000) 区间内
   是否还有别的符号**（`nm` 一查便知）。

### 8.8 ★ 真正的定位：**距离 Huffman 表是坏的**（不是 `strm` 被写坏）

在同一次运行里同时读缺页现场与 tick 采样（§8.7 的"矛盾"是跨运行比较造成的测量错误）：
```
[PFSITE] strm->next_out = 0x13e6cb2   ← 有效（已产出 4.1 MB），不是 4
```
用反汇编的算术还原：
```
out  = 0x13E6CB2                 (20,865,202)
%rbp = out - dist = 0xFFFF…BA21  ⇒ dist = 20,867,729 ≈ **2087 万**
%rdx = 5 (len)、%rdi = 2 (len>>1)、奇数字节修正 + 16 位字循环 = GCC 内联的 memcpy
```
**deflate 的回引距离上限是 32 KB** ⇒ 2087 万在编码上不可能出现 ⇒
**距离 Huffman 表（`state->distcode` / `distbits`）本身是坏的**，
于是查表得到一个非法 `dist`；而 `if (dist > op)`（`op = out - beg = 0x3E6CB2`，正常）
没能拦住它 —— 说明查表时用到的表项/`dmask` 也是错的。

表由 `inflate()` 从**已验证正确的输入**建出来，存放在 `state->codes[]` 里，
而 `state` 是 `__gunzip` 从堆里 `malloc` 出来的（位于 `_bss` = `boot_heap` + 0x60，
堆区间已确认**无其它符号重叠**，`[boot_heap, +0x10000)` 是干净的）。

**下一步（就差这一刀）**
1. **dump `state->distcode` 指向的表**（`state+0x60` 处的指针，表项结构
   `struct code { unsigned char op; unsigned char bits; unsigned short val; }`，
   共 4 字节），看它是不是全 0 / 乱码 / 长度全 1 之类；
2. 同时 dump `state->lenbits` / `state->distbits` / `state->ncode`/`nlen`/`ndist`/`have`
   与 `state->mode`，判断表**建到哪一步**坏的（实测曾捕捉到 `mode=18` = `CODELENS`，
   即正在读码长码，那时 `lencode/distcode` 指针还是正常的）；
3. 表建坏而输入与代码都正确 ⇒ 嫌疑落在 **`state->codes[]` 这块内存**：
   它在 `malloc` 出来的 `state` 里，而 `state` 在堆上 —— 核对
   `sizeof(struct inflate_state)` 与 `malloc` 返回区间的边界是否被别的东西写过。

### 8.9 ★★★ 根因机制闭合：`out < beg` ⇒ `op` 32 位截断 ⇒ 边界检查被绕过

缺页现场从 `inflate_fast` **自己的栈帧**读到全部局部量（序言把 `end` 存在
`0x00(%rsp)`、`beg` 存在 `0x10(%rsp)`，见 `aa9017`/`aa9033`）：

```
end=0x33d78f7   beg=0x1000000   out=0x8   dist=0x112(=274)
%rbp = 0xfffffffffffffef6  (= out - dist = 8 - 274)
strm->next_out=0x124474a      avail_out=35205806
```

**除 `out` 外全部正常**：`beg`/`end` 正确，`dist=274` 是**完全合法的** deflate
回引距离（上限 32 KB），`next_out` 也已推进到 2.4 MB。

**机制**（`inffast.c` 的 `if (dist > op)` 守卫为什么没拦住）：
```c
op = (unsigned)(out - beg);   /* 两个 64 位指针之差 —— 截断成 32 位！*/
```
`out (8) < beg (0x1000000)` 时，`out - beg` 是**负数**，截断成 `unsigned` 后
变成一个**巨大的正数**（≈`0xFF000008`）⇒ `dist (274) > op` 判为**假** ⇒
走直接拷贝 `from = out - dist = -266` ⇒ **越界读 ⇒ 缺页**。

**而 `out` 在函数入口由 `strm->next_out` 载入**，同一时刻 `strm` 里读到的却是
`0x124474a` ⇒ **说明某些 `inflate_fast` 调用拿到的 `next_out` 是 8**
（另一次运行里是 4）—— **这个「间歇性被写成小值」的字段就是要找的写者。**

**下一步（唯一一条）**：找出谁把 `strm->next_out`（`0x3422e58`）写成小值。
- 在每次 `inflate_fast` **入口**打印 `strm->next_out`（而不是等到缺页），
  看它第一次变成小值是在哪一次调用、当时的 `%rip`/调用者是谁；
- `strm` 位于 `_bss`(= `boot_heap`) 起点、`next_out` 在 `+0x18`；
  `state` 紧随其后（`+0x60`）。核对 `.bss` 里是否有别的东西也落在这一小段
  （`nm -n vmlinux | awk '$1>="0000000000aaee40" && $1<="0000000000abee40"'`，
  此前查过是空的，但可再确认 `malloc_ptr`/`malloc_count` 等运行时地址）。

### 8.10 补充：`out` 在**入口是好的**，是执行中被改的

缺页现场再补一组（含窗口状态）：

```
window(0x20(%rsp))=0x0   state.wsize=0 whave=0 write=0   ← 窗口路径确实不可达
len(%rdx)=0x3   dist=0x1de(=478)   out=0x8
beg=0x1000000   end=0x33d78f7   strm->next_out=0x13e6cb2
```

**推论（重要）**：序言用它算过 `end = out + avail_out − 257 = 0x33d78f7`。
若入口时 `out` 真是 8，则 `avail_out` 会是 5400 万，而 `output_len` 只有
3758 万 —— **不可能**。所以 **`out` 在函数入口是好的，是在执行过程中
那个寄存器变成了 8**。

同时 `len=3`、`dist=478` 都是**合法**的 deflate 符号 ⇒ **解码本身没问题**，
坏的只有拷贝的目的指针。而 `op = (unsigned)(out - beg)` 的 32 位截断在
`out < beg` 时把「本该报错的越界」变成「静默放行」⇒ 越界读 ⇒ 缺页。

**下一步（需要 guest 侧单步）**：在缺页前对 guest 做少量指令级跟踪
（宿主的 `vmx` 退出计数 + 单步/`MTF`，或直接对比 QEMU 裸跑同段的反汇编执行流），
定位「目的指针寄存器被写成 8」的那条指令。

### 8.11 ★★★★ 根因找到：VM-entry 不恢复 guest 的 `%r11`（`vmx_run.S`）

**这是整个缺页风暴的根因，也是此前所有矛盾的总解释。**

`kernel/vmm/x86_64/vmx_run.S` 的**进入路径**逐一从 `vcpu->regs` 装载 guest 通用寄存器：

```
0x00→rax  0x08→rbx  0x10→rcx  0x18→rdx  0x20→rbp
0x28→rsi  0x30→rdi  0x38→r8   0x40→r9   0x48→r10
0x58→r12  0x60→r13  0x68→r14  0x70→r15
```

**`0x50`（= `vcpu->regs.r11`）被整段跳过。** 而退出路径**是**保存它的
（`movq %r11, 0x50(%r15)` ✓）。于是每次 VM-entry，guest 的 `%r11` 遗留的是
桩自己刚放进 `%r11d` 的 `vcpu->launched`（0/1）。

**后果**：guest 里任何把重要值放在 `%r11` 的代码，会在**每一次宿主 tick 触发的
VM-exit/entry 往返之后**拿到 0/1。zlib 的 `inflate_fast` 恰好把输出指针
`out` 放在 `%r11`（序言 `aa8ff4: mov 0x18(%rdi),%r11`）⇒ **输出指针被冲成 1/4/8**
⇒ `from = out - dist` 成为野地址 ⇒ 越界读 ⇒ 缺页 ⇒ guest 的处理器因非规范地址
拿不到正确 CR2 ⇒ **无限重试（缺页风暴）**。

**这解释了此前每一个对不上的现象**：观测到的 `%r11` 是 1/4/8 这类小值（就是
`launched`）；内存里的 `strm`/`next_out` 全程正常（**只有寄存器被冲掉**，
所以从内存侧怎么查都查不出）；风暴是间歇的（取决于 tick 是否落在该寄存器
活跃期）；裸跑完全正常（没有 VM-exit）。

**修法（尚未落地，注意有个坑）**：在进入路径补 `movq 0x50(%rdi), %r11`。
**但不能简单插在 `movq 0x48(%rdi), %r10` 之后** —— 因为 `%r11d` 还承担着
`test %r11d,%r11d / jnz .Ldo_resume`（选择 VMLAUNCH 还是 VMRESUME）。提前覆盖
会让第一次就错误地走 VMRESUME ⇒ **guest 一条指令都跑不到（零输出）**。
（改用 `%ebx` 暂存 flag 也不行：`test` 在装入 guest 的 `%rbx` 之后执行，测到的
又成了 guest 的值。）
**正确做法**：把 flag 在装入通用寄存器**之前**消费掉 —— 即在读取
`0x84(%rdi)`/`test`/分支之后，让**两条路径各自**（或用栈暂存 flag）完成
15 条装载序列，确保 `%r11` 的装载发生在 `test` **之后**、`vmlaunch/vmresume` **之前**。

### 8.12 ★★★★★ 已修复：VM-entry 漏装 guest 的 `%r11` —— 缺页风暴的真正根因

**症状**：解压器进 `inflate_fast` 后陷缺页风暴（3 万次/秒），串口停在
`Decompressing Linux...`，内存侧一切正常（`strm`/`next_out` 全程有效），
所有寄存器读数呈现 1/4/8 这类小值。

**根因**：`kernel/vmm/x86_64/vmx_run.S` 的进入路径逐条从 `vcpu->regs` 装载
通用寄存器：

```
0x00→rax 0x08→rbx 0x10→rcx 0x18→rdx 0x20→rbp 0x28→rsi 0x30→rdi
0x38→r8  0x40→r9  0x48→r10 0x58→r12 0x60→r13 0x68→r14 0x70→r15
```

**`0x50`（= `r11`）整段漏了。** 退出路径是保存它的（`movq %r11,0x50(%r15)`），
于是每次 VM-entry 后 guest 的 `%r11` 仍是桩用来装 `vcpu->launched` 的那个 0/1。

zlib 的 `inflate_fast` 把输出指针 `out` 放在 `%r11`（`aa8ff4: mov 0x18(%rdi),%r11`）
⇒ **每个宿主 tick 的 exit/entry 都把输出指针冲掉** ⇒ `from = out - dist` 成为野地址
⇒ 越界读 ⇒ 缺页；而 guest 自己的 `do_boot_page_fault` 因非规范地址拿不到正确 CR2
⇒ 无限重试。

**修复**（`vmx_run.S`，在 `vmlaunch`/`vmresume` **之前**各补一次；不能提前到
寄存器装载段，因为 `%r11d` 还承担 `test`/`jnz` 的 VMLAUNCH/VMRESUME 选择）：

```asm
movq  (%rsp), %r11        /* r11 = vcpu*（第 44 行 push，VMRESUME 前仍在栈顶）*/
movq  0x50(%r11), %r11    /* r11 = guest 的 r11 */
```

**修复后**：解压完成（`done.` `Booting the kernel.`），guest 内核完整引导，
打印 Linux version / e820 / earlycon，自建页表（`cr3=0x260c000`），
栈进入高半区（`rsp=0xffffffff82603eb0`）。

**新的卡点（完全不同的一类）**：内核后期 `Kernel panic - not syncing:
Attempted to kill the idle task!` —— guest 上这类 panic 最常见的原因是
**定时器/中断投递不通**（调度器拿不到 tick）。下一步查 vLAPIC 定时器那条链
（`vlapic_timer_poll` → `vmx_inject_pending`），与解压器无关。

> 教训：**VM-exit/entry 的寄存器保存必须逐条核对结构与汇编的偏移**。
> 这里的 `_Static_assert` 只钉住了 `regs@0`/`rflags@0x78`（总数对），
> 但**单个寄存器的偏移漏一项，编译期毫无提示**，而 guest 只会在
> 恰好使用该寄存器的代码路径上崩溃 —— 表现可以离 VMM 极远。

### 8.13 TEMP-DBG 现状（解压器定位期的那些已收掉）
解压期那一大批（`[SAMPLE]`/`[GSTACK]`/`[HEAP]`/`[ZSTATE]`/`[ZSTREAM]`/`[CODE+]`/
`[VGA-STR]`/`[VGA-DUMP]`/`[SENTINEL]`/`[NO-DEC]`）**已经全部删除** ——
它们不但刷屏，其中 `[OUTTAIL]` 那段还会拿垃圾 `next_out` 去 `phys_to_virt`
算出一个**非规范地址**，直接把宿主打进 `#GP`（看着像 guest 把 VMM 搞崩了）。

还剩的都是**有界**的：`[VMX-DBG]`（前 25 次退出）、`[CPUID-DBG]`（前 12 次
+ 每次 0x15/0x16）、`[EPT-DBG]`（前 8 次）、`[RIP-SAMPLE]`（每 2000 次退出一行，
INFO 级，`LOG=warn` 下不输出）。

**`EXC_BITMAP` 必须保持 0**（见 §7.3(a)）。

## 9. ★ 最后一公里：从「卡在 DMI」到 guest shell 的 6 个 bug

这一段是跨过最后一道坎的全部内容。**共同特征：症状全都在 guest 侧，
但根因全在 VMM 侧，而且每一个的表现都像另一个问题。**

### 9.1 MMIO 指令解码把 guest 的 **RIP 当物理地址** 用（最隐蔽）

`decode_mmio_access()` 要取指令字节，而它调的是 `guest_fetch_bytes(guest_rip)`
—— 后者按 **GPA** 查 EPT。解压器阶段 guest 是恒等映射（VA==PA），所以一直没事；
一旦进内核、RIP 变成 `0xffffffff810608c6` 这种内核虚拟地址，EPT 查不到 ⇒
解码失败 ⇒ **MMIO 读不回写目标寄存器**，guest 拿到的是上一条指令的遗留值。

实测症状（极具误导性，全都不像"解码坏了"）：
```
BIOS bug: APIC version mismatch, boot CPU: c0, CPU 0: version 14
APICID-TRACE: driver=flat read_id=129 isset(rid)=0
IOAPIC[0]: apic_id 0, version 0, address 0xfec00000, GSI 0-95
```
`0xc0` / `129` 都是别的寄存器残留，**vLAPIC 本身是好的**。

**修法**：`guest_va_to_gpa()` —— 走一遍 guest 自己的页表（PML4→PDPT→PD→PT，
处理 1 GiB / 2 MiB 大页），CR0.PG=0 或非 4 级时按恒等兜底。
指令取字节用 `guest_fetch_code()`，按页裁剪（x86 保证指令不跨页）。

### 9.2 CPUID 0x15/0x16 报 0 ⇒ 掉进 `quick_pit_calibrate()` 死循环

`native_calibrate_tsc()` 第一句就是 `if (cpuid_level < 0x15) return 0;`，
而我们把叶 0 的最大叶报成 **1**。于是内核去调 `quick_pit_calibrate()`：
它往通道 2 写 0xFFFF，然后**反复读端口 0x42 比高字节**。我们的 8254 桩
「读回最后写入的字节」，高字节永远是 0xFF ⇒ 50000 轮全部命中、每轮 5 个端口
VM-exit ⇒ 看着像死机。

**修法两件，都要做**：
1. **CPUID 合成**（实测 QEMU/KVM 会把宿主 0x15/0x16 抹成全 0，透传没用）：
   报 **1 GHz 晶体 + 比例 tsc/1e6**，即
   `0x15: eax=1000 ebx=<MHz> ecx=1000000000`，`0x16: eax=<MHz>`。
   这样 `crystal_khz*ebx/eax = tsc_khz` ✓，且顺带把 `cpu_khz_from_cpuid()`
   （`native_calibrate_cpu_early()` 的首选）也喂饱了。
2. **8254 写真**：按宿主挂钟递减 + 真实的 LSB/MSB 读翻转触发器。
   桩之所以危险，是因为它对内核撒的谎**恰好让它多跑 25 万次 VM-exit**。

### 9.3 ★ 晶体频率必须和 vLAPIC 模型对齐（否则 watchdog 判 TSC 不稳）

Linux 由晶体频率反推 LAPIC timer 频率：
`lapic_timer_period = crystal_khz * 1000 / HZ`（它假定 LAPIC 跑在晶体上）。
我们的 vLAPIC 模型是「1 count = 1 ns」= **1 GHz**。一开始报 2.417 GHz，
两边差 2.4 倍，每个 tick 都被拉长，于是：

```
clocksource: timekeeping watchdog on CPU0: Marking clocksource 'tsc-early' as unstable
  'refined-jiffies' wd_nsec: 503923392      ← 慢的一边
  'tsc-early'       cs_nsec: 4127262383
tsc: Marking TSC unstable due to clocksource watchdog
```
（实测比值 8.2，除频率外还叠加了丢 tick。）
**修法**：见 9.2 —— 晶体就报 1 GHz，让两边天然一致。

### 9.4 ★ 注入外部中断：只判 `RFLAGS.IF` 不够，还要判 interruptibility state

`sti; hlt` 是 guest 的 idle 序列，而 **`hlt` 落在 `sti` 的影子里** ——
VM-exit 时硬件把 interruptibility state 的 **bit0(STI 阻塞)=1** 一并存进 VMCS。
挂起的事件这时**不能硬投**，否则每次 VM-entry 都被硬件拒（reason 33）。

诊断靠把三个字段打进 VM-entry 失败现场（**`inst_error` 要最后读**，
任何其它 vmread/vmwrite 都可能覆盖它）：
```
[VMX] entry state: intr=0x800000ec err=0x0 ilen=0 msr_cnt=6 msr_adr=0x4b9400
[VMX]   cr0=0x80050033 cr3=0x260c000 cr4=0x26b0 efer=0xd01 rflags=0x202
[VMX]   actv=0x0 intr_state=0x1        ← 就是它
```
**修法**：和 `IF=0` 一样挂起 + 开 interrupt-window，
判据 `!(rflags & 0x200) || (istate & (0x1|0x2|0x8))`
（bit2 是 NMI 阻塞，只挡 NMI，**不能**一起判）。

### 9.5 ★ HLT 之后要**清掉**影子位，否则 guest 永远收不到 tick

9.4 只是"不硬投"，但**收不到 tick 的问题还在**：开 interrupt-window 也没用，
因为窗口条件要求「无阻塞」，而那个 bit 一直是 1 ——
实测退化成 **870 万次纯 HLT 空转**，guest 停在 idle 再也不前进。

**修法**：`hlt` 已经执行完了（我们正是因此才拿到 HLT 退出），
**影子已被消耗**，所以在 HLT 分支里清掉 bit0/1/3 是语义正确的。

### 9.6 ★★ `MSR_FS_BASE` 只存影子、没写 `GUEST_BASE_FS`（GS 修了 FS 没修）

同一段代码里 `MSR_GS_BASE` 有 `vmcs_write(GUEST_BASE_GS, val)`，
`MSR_FS_BASE` **只有** `vcpu->msr_fs_base = val;` —— 而上面那段注释
（"只存影子不写 VMCS 的话 guest 写的 base 永远不会生效"）**说的就是这个坑**。

症状的迷惑性拉满：**内核整个启动过程一个字都不错**，
一直到 userspace 才崩：
```
Run /init as init process
init[1]: segfault at 0 ip 00007febbb1fc30d sp ... error 4 in busybox
Code: ... c3 <64> 48 8b 04 25 00 00 00 00 ...      ← mov %fs:0x0,%rax
Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b
```
`64 48 8b 04 25 00 00 00 00` 是 `mov %fs:0x0,%rax`：FS base = 0 ⇒ 访问地址 0。
**内核态不用 FS，所以前 5 秒谁都没露馅。**

**修法**：照 GS 抄一遍 `vmcs_write(GUEST_BASE_FS, val)`。

### 9.7 IO-APIC 桩：不要漏了「两个寄存器按页内偏移区分」

`0xFEC00000` 是 IOREGSEL、`0xFEC00010` 是 IOWIN。曾经写成
`(gpa & ~0xFFF) == BASE && (gpa & 0xf) == 0` —— **`0x10 & 0xf == 0`**，
于是所有 IOWIN 访问都被当成 IOREGSEL，数据寄存器永远读不回真值，
Linux 拿它建 `irq_cfg` 直接空指针崩在 `setup_IO_APIC`。

**修法**：用**页内偏移** `off = gpa - BASE`，`off == 0x00` 才是 IOREGSEL。
表项初值全部置 `0x10000`（mask 位）—— 让内核探测完就认定没有外部中断。

### 9.8 这一轮真正管用的调试手法

1. **guest 虚拟地址必须符号化**：`addr2line -f -e ~/kbuild/linux-def/vmlinux <addr>`。
   `default_idle`、`quick_pit_calibrate`、`native_apic_mem_read` 三个函数名
   一出来，方向立刻就定了（比看寄存器猜快一个数量级）。
   > ⚠️ 反汇编要用 `vmlinux`（含符号），**不是** `vmlinux.bin`（那是解压后的镜像）。
2. **退出原因直方图 + RIP 采样**（每 2000 次退出一行）：
   `12:8680720` 这种一边倒的数字直接指出「在原地打转」，
   再配合那**一个** RIP 的符号名就定位了。
3. **VM-entry 失败现场要 dump 够**：`actv` / `intr_state` / `inst_error`
   三个字段是这一轮破案的关键，缺一不可。

## 10. 重建 guest 内核（`imgs/guests/x86_64/bzImage`）

guest 内核源码树在 **仓库外**：`~/kbuild/linux-def`（由 `~/kbuild/build2.sh` 从
`linux-6.2.15.tar.xz` 解出来，`defconfig` + 关掉 ACPI/模块/随机化）。

**必须用 musl 交叉编译器**，与 aarch64/riscv64 的 guest 保持一致（`build2.sh`
里原本是裸 `make`，会落到宿主 glibc 的 gcc，版本串一眼能看出来）：

```bash
export PATH=$PATH:~/Desktop/Software/compiler/x86_64-linux-musl-cross/bin
cd ~/kbuild/linux-def
make mrproper && cp /tmp/<存好的>.config .config && make olddefconfig   # 换编译器必须全量重来
make ARCH=x86_64 CROSS_COMPILE=x86_64-linux-musl- -j$(nproc) bzImage
strings vmlinux | grep -m1 "Linux version"     # 应为 x86_64-linux-musl-gcc (GCC) 11.2.1
cp arch/x86/boot/bzImage <repo>/imgs/guests/x86_64/bzImage
```

`HOSTCC` 不用动 —— 构建期工具（kconfig/objtool/…）仍由宿主 gcc 编，只有
内核本体走交叉编译器。

### 10.1 ⚠️ `CONFIG_SERIAL_8250_DETECT_IRQ` **必须保持 =y**

开机时串口上会出现**一个游离的 `0xFF`** 字节，位置固定在：

```
Serial: 8250/16550 driver, 4 ports, IRQ sharing enabled\r\r\n <0xFF> [    1.62] serial8250: ttyS0 at I/O 0x3f8 ...
```

**它不是 VMM 的 bug**：那是内核 `autoconfig_irq()` 在**主动探测串口用的是哪根
IRQ 线** —— 先 `IER=0x0f` 打开全部中断，再故意往 THR 写一个 `0xFF`，靠它触发的
"发送寄存器空"中断反推 IRQ（`8250_port.c` 里 `serial_out(up, UART_TX, 0xFF)`，
其上方 MCR 正好被设成 `0x0b`，与 VMM 侧抓到的 `mcr=0x0b lcr=0x13` 完全吻合）。
**QEMU 裸跑同一个 guest 也一样有这个字节**（两边都没给 guest ACPI，所以都落到
`arch/x86/include/asm/serial.h` 的 ISA 兜底路径，其 `STD_COMX_FLAGS` 带
`UPF_AUTO_IRQ`）。真机有 ACPI，端口由 `serial8250_pnp` 认领、flags 里没有
`UPF_AUTO_IRQ`，所以看不到。

**试过把它去掉，结论是去不掉**：关掉 `CONFIG_SERIAL_8250_DETECT_IRQ` 后探测不再
发生，但 `STD_COMX_FLAGS` 里那个 `UPF_AUTO_IRQ` 一没，驱动就会**保留**
`SERIAL_PORT_DFNS` 表里写死的 `irq = 4`，于是改为等 IRQ 4 的 RX 中断 ——
而我们的 PIC / IO-APIC 桩**永远不产生中断**，RX 就此死掉。
实测：`/init` 跑到 `busybox/mount/grep` 之后**卡死，键盘输入没有任何反应**。

反过来，探测**失败**（返回 0）反而是我们现在能用的原因：
```c
port->irq = (irq > 0) ? irq : 0;   /* 探不到 → 0 → 驱动走"无 IRQ"那条路 */
```
`irq=0` 时驱动不依赖中断，控制台输入正常。

**所以：这个 `0xFF` 是「串口输入能用」的代价，必须留着。**
真想去掉的话，正道是**让 vUART 真的会报中断**（RX 到达时按 guest 在 IO-APIC
重定向表里给 IRQ 4 编程的 vector 注入）—— 那是个正经功能，不是一行改动。

## 11. 相关

- 另两个架构的同类文档：`docs/vmm/RISCV64_GUEST_LINUX.md`、`docs/vmm/X86_VMX_GUIDE.md`（旧的玩具 guest 说明）
- 两种运行模式与 `/dev/vmm` 协议：`docs/vmm/GUEST_CONSOLE.md`
- 裸跑基准的做法：`docs/vmm/GUEST_NATIVE_QEMU.md`
