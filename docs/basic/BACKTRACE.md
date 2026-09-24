# 调用栈回溯 (BACKTRACE)

## 概述

panic、断言失败、内核态异常发生时，打印带函数名的调用栈，回答"**是谁调过来的**"。

在此之前，崩溃现场只有一份寄存器快照：能看到 `RIP=0x…`，但看不出这个地址
是被哪条路径调过来的，排查偶发缺页基本靠猜。

实现：`kernel/debug/backtrace.c` + `kernel/debug/backtrace.h`。
符号表由 `tools/gen_kallsyms.py` 在构建时生成并嵌进镜像。

```
=== BACKTRACE (fault) [C0 '/bin/cat' id=5] ===
  #0  0xffff800000206583  backtrace_read+0x33
  #1  0xffff800000206bce  pseudo_read+0x5e
  #2  0xffff8000002197de  sendfile_handler+0x1de
  #3  0xffff8000002269c4  syscall_handler+0x1744
```

## 快速使用

崩溃时**不需要任何开关**：`assert` 失败、内核态异常、以及任何走
`platform_panic()` 的路径都会自动带上调用栈。

按需看当前任务的栈：

```sh
cat /proc/backtrace
```

### 想亲眼看一次长什么样

有一条一键命令，**故意在 ELF 解析路径上 panic**：

```sh
make PLATFORM=qemu-virt-x86_64 test-panic
```

输出（三个架构都一样，函数名按各自镜像解析）：

```
[ERROR][C0] kernel/loader/elf_image.c:155: [elf] PANIC_TEST=1: 故意在 ELF 解析路径上触发一次 panic
=== BACKTRACE [C0 'busybox' id=2] ===
  #0  0xffff800000241e58  platform_panic+0x28
  #1  0xffff800000209e71  elf_image_load_impl+0x231
  #2  0xffff80000022695f  task_execve+0x11f
  #3  0xffff80000020aedb  load_from_file_depth+0xc6b
  #4  0xffff80000020b74f  demo_load_busybox+0xcf
  #5  0xffff80000022a1ff  task_trampoline+0x4f
```

触发点在 `kernel/loader/elf_image.c`：ELF 头部**刚校验通过、程序头还没开始解析**
的地方。选这里是因为它离 boot 足够远，能打出一条真实的加载器调用链，而不是只有
`kernel_main` 两层 —— 正好用来看展开和符号化对不对。

它由 `PANIC_TEST=1` 控制（`RUN_PANIC_TEST`，见 `Makefile` §7），**不传就是普通
内核，那段代码根本不参与编译** —— 不需要事后手动删代码。触发点直接调
`platform_panic()`，走路和真实断言失败完全一样，所以看到的东西不会因为是
"演示"而和真崩溃不一样。

> ⚠️ 这个变体编出来的内核**跑不完启动**（每次加载可执行文件都会 panic），
> 它不是用来跑功能的。看完 `make PLATFORM=... clean` 或者直接不带
> `PANIC_TEST` 重编即可 —— 切回来时 `elf_image.o` 会自动重编
> （`Makefile` 的 `_VARIANT_OBJS`），不会留下带 panic 的旧对象。
>
> 需要 `imgs/rootfs-<arch>.img`：触发点是"解析 /busybox 的 ELF 头"，
> 没有文件系统会先在 `open` 那一步失败退出，走不到这里。

想看展开过程的细节（走的哪条路、每一帧的原始地址）：`LOG=debug`，
自检会在开机时打印一次完整的调用栈。

## 两个概念：帧指针链 vs 栈扫描

展开有两条路，**先试帧指针链，走不通才退化成栈扫描**：

| | 帧指针链 | 栈扫描 |
|---|---|---|
| 原理 | 顺着 `rbp`/`x29`/`s0` 串起来的帧记录往上走 | 从 sp 往栈顶逐字找"像代码地址"的值 |
| 准确性 | 准 | **会误报**（局部变量、旧 PC 都会被当成帧） |
| 前提 | 编译时开了帧指针（`FP=1`，默认） | 无 |
| 用于 | 主力路径 | 汇编存根、`FP=0` 构建、栈被踩烂 |

用到栈扫描时输出里会明确标出来，别把那种结果当准的：

```
=== BACKTRACE (fault) [C0 'nginx' id=7] ===
  [bt] 帧指针链不可用，以下是栈扫描结果（可能有误报）
  #0  ...
```

## 构建开关：`FP=`

```bash
make PLATFORM=qemu-virt-x86_64 kernel          # FP=1，默认
make PLATFORM=qemu-virt-x86_64 FP=0 kernel     # 不加帧指针
```

`FP=1` 时给三个架构都加 `-fno-omit-frame-pointer`。

**代价**：占掉一个通用寄存器（x86_64 `rbp` / aarch64 `x29` / riscv64 `s0`），
全内核大约 1-3% 性能损失。

**默认开**：默认关的话新功能一上来只能给栈扫描的噪声，看着像坏了。
要压性能时 `FP=0` 一行关掉。

切换 `FP=` 会改变 CFLAGS，但**不需要手动 clean** —— 它已经加进
`Makefile` §7 的 `_CFG_CHECK` 签名，配置一变会自动清掉旧目标文件。

## 符号表是怎么来的

内核是 `objcopy -O binary` 出 `.bin` 再交给 QEMU `-kernel` 的，**镜像里没有
符号表**。要打出 `func+0x12` 就必须把符号表编进去，而表的内容（地址）又依赖
链接结果 —— 鸡生蛋。所以链接分两趟：

```
stage-1.elf ──gen_kallsyms.py──> kallsyms_<arch>.S ──> .o
     │                                                   │
     └──────────────────┬────────────────────────────────┘
                        ▼
                 kernel_<arch>.elf（带 .kallsyms 段）
                        │
                        └─ gen_kallsyms.py --verify 比对两趟地址是否一致
```

两趟地址为什么对得上：`link.ld` 把 `.kallsyms` 放在 `.text` **之后**，新增
这一段不移动任何函数；表里也只收 `STT_FUNC`。

### ⚠️ 改这块时要小心的三件事

1. **`.kallsyms` 的段序不能挪。** 放到 `.text` 之前会让所有函数地址整体后移，
   镜像本身完全正常、但 panic 打出来的函数名全错位 —— 假的调用栈比打不出来
   更危险。`--verify` 就是专门挡这个的，它失败时**别绕过**，去查段序。

2. **stage-1 必须链一个"空表桩"**（`gen_kallsyms.py --stub`），不能靠"不链接"
   或 C 侧弱符号。实测踩过：弱符号在 stage-1 里是未定义的（地址按 0 解析），
   `backtrace_lookup()` 里对它的取地址比较会被编成"构造常量 0"，指令序列比
   stage-2 的 PC 相对寻址长几个字节 → `.text` 两趟不一样长 → 从该函数往后
   所有地址偏移 16 字节 → 符号表全错位。桩让两趟的符号解析完全一致。

   桩里**对齐必须和真表逐项一致**：aarch64 用 `ldr x, [x, #:lo12:sym]` 取
   数组基址，符号没 8 字节对齐会直接报 `R_AARCH64_LDST64_ABS_LO12_NC` 截断。

3. **riscv64 的 `.kallsyms` 位置和另两个架构不同**：x86_64/aarch64 放在
   `.rodata` 之后，riscv64 放在 `.sdata` 之后、`.bss` 之前。原因是 RISC-V
   链接器默认开松弛（relaxation），代码长度依赖布局；放在 `.sdata` 之后能让
   两趟之间 `.text/.rodata/.data/.sdata` 一个都不动。

   x86_64 那边不能照抄 riscv64 的位置 —— 它必须排在 `_edata` 之前，否则
   multiboot 的 `load_end_addr` 会把这一段漏掉。

## 三个架构的帧布局不一样

实测 GCC `-O2 -fno-omit-frame-pointer`：

```
x86_64 rbp            aarch64 x29            riscv64 s0(x8)
push %rbp             stp x29,x30,[sp,-16]!  addi sp,sp,-N
mov  %rsp,%rbp        mov x29,sp             sd   ra,(N-8)(sp)
                                             sd   s0,(N-16)(sp)
                                             addi s0,sp,N
[fp+0] = 上帧 fp      [x29+0] = 上帧 x29     [s0-16] = 上帧 s0
[fp+8] = 返回地址     [x29+8] = 返回地址     [s0-8]  = 返回地址
```

RISC-V 的偏移是**负的**（`s0` 指向帧顶而不是帧底）。照抄 x86 的 `[fp]/[fp+8]`
会读到帧内的局部变量，症状是调用栈里冒出一堆随机函数。

### RISC-V 的叶子函数坑

RISC-V 上还有个额外的坑：**叶子函数不保存 `ra`**（压根没调用过谁，`ra` 一直
活着），于是 GCC 把它省下来的槽位给了 `s0`：

```
bt_temp_b（非叶子）: addi sp,sp,-16; sd s0,0(sp); sd ra,8(sp); addi s0,sp,16
bt_temp_c（叶子）  : addi sp,sp,-16; sd s0,8(sp);               addi s0,sp,16
                                         ^^^^^^^ 只有 s0，没有 ra
```

也就是 `[s0-8]` 里装的到底是 `ra` 还是 `s0` 得看这个函数是不是叶子，判据是
"它是不是代码地址"。更麻烦的是：**叶子帧的返回地址栈上根本没有**，只活在 `ra`
寄存器里。异常路径可以从 trap frame 的 `x[1]` 把它捞回来 —— 这就是
`backtrace_dump_fault()` / `backtrace_collect_from()` 那个 `ra` 参数的唯一用途
（x86_64/aarch64 传 0）。

## 已知局限

- **最内层可能少一帧**：如果出错的那个函数编译后**没有帧记录**
  （x86_64 上无局部变量的叶子连 `push rbp` 都省了），`rbp` 还停在上一个
  函数的帧上，往上一跳就直接到了它的调用者。**不会有错位的标签**，只是少了
  中间那一层。RISC-V 由 `ra` 提示补上，x86_64/aarch64 目前没有来源。
  这是帧指针展开的固有局限（Linux 在 x86 上改用 ORC/DWARF 正是为此）。

- **汇编写的入口不会出现在栈里**：异常存根、`task_trampoline` 这些不建帧
  指针，帧链走到它们就断了。所以 `/proc/backtrace` 的栈底通常是
  `syscall_handler`，再往下没有了。

- **`FP=0` 时只有栈扫描**：会误报，输出里有明确标注。

## 相关文档

- `docs/basic/ASSERT.md` — 断言系统（panic 的主要来源）
- `docs/basic/KLOG.md` — 日志级别、`/proc/syscalls`
- `docs/INTERRUPT_CONTEXT_SWITCH.md` — 异常返回路径与任务切换
