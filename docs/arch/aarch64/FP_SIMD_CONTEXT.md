# AArch64 FP/SIMD（NEON）：内核启用与上下文保存

**日期**: 2026-09-18
**影响架构**: AArch64（`ARCH_AARCH64`）
**严重性**: 高（漏掉任一层都会静默破坏 FP 寄存器状态）
**触发条件**: 内核代码或用户态执行任何 FP/SIMD 指令

> **改 `trap_frame_t` 布局、`boot/aarch64/exception.S`、`kernel/task/aarch64/switch.S`
> 或 `kernel/task/switch.h` 里三处伪帧构造之前，先读完本文。**

---

## 一、背景：为什么要动这件事

`Makefile` 里 aarch64 原本写着 `-mgeneral-regs-only`（禁用 SIMD/FP），而 riscv64
一直是开着浮点的（`-march=rv64gc -mabi=lp64d`）。本次把 aarch64 也打开。

**打开这个开关不是删一行 CFLAGS 就完事**——riscv64 之所以安全，是因为它在**两层**
都保存了 FP 状态，而 aarch64 这两层**一层都没有**：

| 层 | riscv64 | aarch64（改之前） |
|---|---|---|
| 陷阱帧（EL0→内核） | `f[32] + fcsr` | 无 |
| 任务切换 | `fs0-fs11 + fcsr` | 无 |

改之前之所以"看起来能跑"，靠的是一个巧合：内核编译时禁了 FP，所以内核代码
**从不碰**浮点寄存器。真实用户态早就在用 NEON（`apps/busybox-aarch64` 里有 743 条
FP/SIMD 指令），这些寄存器在两个用户任务之间、以及被内核抢占时没有任何人保存——
已经是一个潜伏 bug。删掉 `-mgeneral-regs-only` 后，内核自己开始用 FP 寄存器
（实测：内核镜像里出现 126 条 FP/SIMD 指令，`kernel_syscall_fs_file_ops.o` /
`kernel_syscall_core_proc_lifecycle.o` 里是成片的 `ldp q0,q1` / `stp q0,q1`
结构体拷贝），潜伏 bug 立刻变成必现 bug。

---

## 二、踩到的坑：SAVE_REGS 撑爆 128 字节向量槽位

第一版实现把所有保存代码就地展开，结果 busybox 一启动就 panic：

```
[INFO][C0] boot/aarch64/exception.c:148: This is invalid_exception: kind: 2, source: 0
```

**key: 报的是 FIQ 表项（kind=2/source=0），但 QEMU `-d int` 显示当时发生的是一次
普普通通的定时器 IRQ：**

```
Taking exception 5 [IRQ] on CPU 0
...from EL2 to EL2
...with ESR 0x0/0x3950000
```

根因：AArch64 异常向量表是 16 个**固定 128 字节**的槽位，硬件按
`VBAR + group*0x200 + slot*0x80` 跳转（不看你代码有多长）。原来的 `SAVE_REGS`
约 92 字节，加进 34 条 FP 存储后涨到 230+ 字节，于是后面所有表项都被挤到错误
偏移——IRQ 跳进了 FIQ 表项的代码里。

`.p2align 7` **不会**报错，它只是把后面的表项继续往后推，所以这个问题在汇编期
完全静默，只在运行期表现为"中断跳错表项"的灵异 bug。

**修复**：FP 保存移到向量表**外面**的 `fp_save_common`，由 `SAVE_REGS` 用
`bl` 调用；并在向量表里用 `VEC_ALIGN`（底层是 `.org`）把每个表项的偏移钉死，
超长时汇编期直接失败：

```
Error: attempt to move .org backwards
```

（用 `.if` 做不了这件事：`.p2align` 产生的 fragment 边界会让 GAS 无法在汇编期
求出 `.` 的常量差值，报 `non-constant expression`。）

---

## 三、现在的四层保存点

### 1. 早期打开 FPEN —— `boot/aarch64/boot.S` 的 `init_el2_vhe`

```asm
mrs     x1, cpacr_el1
orr     x1, x1, #(3 << 20)      /* FPEN = 0b11：EL0/EL1 执行 FP/SIMD 不陷入 */
msr     cpacr_el1, x1
isb
```

必须在 `msr vbar_el1` **之前**：向量表一生效，第一次异常的 `SAVE_REGS` 就会执行
`bl fp_save_common`（里面有 `stp q`）。FPEN 复位值是 0b00（陷入），那会陷入
**同一个向量** → 再进 SAVE_REGS → 无限递归（每轮 `sub sp, #816`），
表现为挂死而不是可诊断的 panic。

VHE（E2H=1）下 EL2 访问 `cpacr_el1` 落在 `CPACR_EL2`，正是同时门控 EL1 与 EL0
的那一份；`kernel_main()` 里的 `aarch64_enable_neon()` 保留（幂等），AP 走同一份
`init_el2_vhe`。

### 2. 陷阱帧 —— `boot/aarch64/exception.S` + `include/aarch64/exception.h`

| 偏移 | 字段 |
|---|---|
| 0..247 | `r[0..30]`（x0..x30） |
| 248 | `usp`（SP_EL0） |
| 256 | `elr` |
| 264 | `spsr` |
| 272 | `tpidr_el0` |
| 280 | `fp_pad`（填充，让 q[] 落在 16 字节边界） |
| 288 + 16*i | `q[0..31]`（q31 @ 784） |
| 800 | `fpcr` |
| 808 | `fpsr` |

**`TRAP_FRAME_SIZE = 816`（102 × 8）。**

- **无条件保存**，不能按 `spsr & 0xf` 条件跳过：内核态（EL1）陷入可能打断一段
  **没有函数调用**的内核代码，此时 v0-v7/v16-v31 仍是活跃值——异步陷阱不是编译器
  意义上的 call，AAPCS64 的 caller-saved 规则保护不了它们。
- 恢复到 `q0-q31` 必须在 `RESTORE_REGS` 标号 `1:` **之后**：那个 `spsr` 判断只管
  SP_EL0/tpidr_el0 要不要恢复。
- **帧大小必须是 16 的倍数**：硬件进入 EL1 时不动 SP，16 字节栈对齐是纯软件不变量
  （AAPCS64），`stp q`/`ldp q` 也要求 16 字节对齐。改之前的 280 字节是
  `8 (mod 16)`，也就是说**整个陷阱处理期间 C 函数一直在错位栈上运行**——这次
  顺带修好了。
- 代价：每次陷阱约 34 条存储 + 34 条加载（约 512 B 双向）。对照 riscv64 无条件
  保存 f0-f31 的做法。

### 3. 任务切换 —— `kernel/task/aarch64/switch.S` 的 `arch_task_switch`

帧 96 → **176 字节**，新增 `d8-d15 + FPCR + FPSR`（AAPCS64 的被调用者保存部分）：

```
0 x19  8 x20  16 x21 24 x22 32 x23 40 x24
48 x25 56 x26 64 x27 72 x28 80 x29 88 x30(LR)
96 d8  104 d9   112 d10 120 d11
128 d12 136 d13 144 d14 152 d15
160 fpcr  168 fpsr
```

x19@0 / x30@88 的偏移刻意保持不变，这样 `switch.h` 的 `sp[0]=&child_frame`、
`sp[11]=返回地址` 约定不用改。

**这里只保存 d8-d15，不保存 q0-q31**：切换点是一次普通 C 函数调用，caller-saved
的 v0-v7/v16-v31 在调用点本来就是死的。分工是：
**陷阱帧负责用户/guest 的完整 q0-q31，切换帧只负责内核 C 代码跨切换不丢值。**

### 4. 三处伪帧构造 —— `kernel/task/switch.h`

`arch_init_task_stack` / `arch_init_user_stack` / `arch_init_fork_child_stack`
都要从 `sp -= 12` 改成 `sp -= 22`，并把新增的 10 个字（d8-d15/fpcr/fpsr）清零。

⚠️ **这四处（switch.S + switch.h 三处）必须同时改**。只改一半的症状是：
`arch_task_switch` 按 176 字节弹栈，而伪帧只有 96 字节 → 读到栈顶**之外**的内存
（`g_task_stacks[TASK_MAX][TASK_STACK_SIZE]` 里那就是下一个任务的栈），
收尾 `add sp, #176` 还会把 SP 留在栈顶之上。

`arch_fork_resume_user` 从子进程陷阱帧恢复 q0-q31/fpcr/fpsr（fork 语义：子进程
继承父进程 FP 状态）。顺带修了一个老 bug：它恢复了 x1-x8、x10-x30、x0、tpidr_el0，
**唯独漏了 r[9]**，fork 子进程会带着内核地址（`&trap_frame`）回到 EL0。

---

## 四、验证方法

```bash
make PLATFORM=qemu-virt-aarch64 clean     # 改过 CFLAGS/头文件必须 clean 重编
make PLATFORM=qemu-virt-aarch64 kernel

# 1) 向量表 16 个表项的偏移（每个槽位应恰好以 SAVE_REGS 的 sub sp 开头）
aarch64-linux-musl-objdump -d build/qemu-virt-aarch64/exception_asm.o \
  | grep -E '^\s+[0-9a-f]+:' | python3 -c "..."   # 见 git log / MR 里的片段
# 2) FP 保存/恢复确实在
aarch64-linux-musl-objdump -d build/qemu-virt-aarch64/exception_asm.o | grep -c 'bl.*<fp_save_common>'   # 16
aarch64-linux-musl-objdump -d build/qemu-virt-aarch64/kernel_task_aarch64_switch.o | grep 'stp\s+d8'
# 3) 没有引入 libgcc 依赖（未链接 libgcc，见下）
aarch64-linux-musl-nm build/qemu-virt-aarch64/kernel_aarch64.elf | grep -E '^ +U '

make PLATFORM=qemu-virt-aarch64 test-pthread   # busybox/musl 全家桶 + clone 路径
make PLATFORM=qemu-virt-aarch64 test-ltp       # fork/clone/signal 路径
```

**负向验证（推荐做一次）**：把 `VEC_ALIGN` 里的某个偏移改小 0x80，重新汇编应立刻
失败并报 `attempt to move .org backwards` —— 说明 128 字节预算真的被强制住了。

**没有覆盖的**：本次没有新增专门的 FP 上下文测试（如"装载 32 个图案 → 制造
陷阱/抢占 → 逐字节比对"）。现有目标只能证明"没炸"，证不了"寄存器值确实没被破坏"。

---

## 五、残留风险与限制

1. **VMM guest 的 v0-v31 没有保存点**。guest 陷入走 `guest_vec_table`
   （`kernel/vmm/aarch64/el2_vmcs.S`，TGE=0 时 `vbar_el2` 被改指），不经过
   `SAVE_REGS`；宿主内核现在可能用 FP 寄存器，会破坏 guest 的 NEON 状态。
   guest 的 FPCR/FPSR 是 bank 的（EL1 那份），宿主在 EL2 读写的是 EL2 那份，
   不会被串扰；受害的只有 V0-V31 这组共享物理寄存器。
   → **`test-guest-linux` 在 VMM 侧补上保存之前不应再作为通过标准。**
2. **二级核 idle 栈只有 4096 字节**（`kernel/task/cpu.c`）。陷阱帧 816 + 切换帧
   176 + 处理函数调用链，单层嵌套下约 1.2-2KB，余量偏紧。全树没有栈 canary，
   溢出是静默的。
3. **内核浮点只能用 `float`/`double`**。没有链接 libgcc，`long double`（aarch64 上
   是 128 位）的运算会引出 `__addtf3`/`__divtf3` 等帮助函数，链接期报未定义符号。
4. **`sigreturn`/信号投递的 sigframe 拷贝**已从裸 `memcpy` 改成
   `copy_to_user_bytes`/`copy_from_user_bytes`（帧涨到 816 字节后跨页概率大增，
   裸 memcpy 踩到坏地址 = 内核态数据中止 = 整机挂死）。注意这两个接口内部只做
   `user_range_ok()`（地址范围 + 回绕）校验，**不查页表是否映射**：传一个落在
   用户地址范围内但未映射的地址仍会中止。
5. FPCR/FPSR 的 bank 语义依赖 `TGE=1`（宿主常态）。宿主在 EL2 用 `mrs fpcr` 读到的
   正是 EL0 用户态生效的那一份；guest 进出时 TGE 会被清零，那时 EL1 侧的那份
   属于 guest，不在本次范围内。
