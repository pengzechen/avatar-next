# RISC-V64 内核启动重启问题排查记录

## 现象

执行 `make ARCH=riscv64 run-fs` 后，内核在输出 `[fs] Mounting 'ramblk0' at '/'...` 后立即重启，无限循环。

---

## 根本原因分析

问题由三个独立 Bug 叠加导致，需全部修复才能正常运行。

---

### Bug 1：`exception_init()` 调用顺序错误（重启根因）

**现象**：`ext4_mount` 触发 CPU 异常，hart 被 OpenSBI 重置。

**原因**：  
原始代码中，`exception_init()`（设置 `stvec`，启用 `sstatus.SIE`）在 `fs_init()` 之后才被调用。在 `stvec` 未设置的情况下，`ext4_mount` 过程中一旦发生任何 CPU 异常（如 Bug 2 导致的 Load Access Fault），异常无法被 S-mode 捕获，直接落到 M-mode。OpenSBI 检测到未处理的 M-mode 异常后将 hart 重置，表现为内核不停重启。

**修复**（`kernel/main.c`）：

```c
// 修复前：exception_init 在 fs_init 之后
ramblk_init();
fs_init();
...
exception_init();  // ← 太晚了

// 修复后：exception_init 必须在 fs_init 之前
exception_init();  // ← 先设置好异常向量
ramblk_init();
fs_init();
```

---

### Bug 2：`.data` 段函数指针运行时为 NULL（Load Access Fault 根因）

**现象**：`exception_init()` 修复后捕获到异常信息：

```
Unhandled exception: scause=0x5 sepc=0x8020b7c2 stval=0x48
```

`scause=0x5` 为 Load Access Fault，`sepc` 指向 `ext4_block_init`，`stval=0x48` 是对 NULL 指针加偏移量 0x48 的访问。

**原因**：  
riscv64 的 Makefile 缺少 `-fno-pie / -no-pie` 标志。GCC 默认以 PIE 模式链接，会在 `.rela.dyn` 段生成 `R_RISCV_RELATIVE` 类型的重定位条目，用于在动态加载时修正 `.data` 段中的绝对地址（如函数指针、结构体指针）。

裸机内核没有动态链接器，这些重定位条目从未被处理，导致 `EXT4_BLOCKDEV_STATIC_INSTANCE` 宏生成的 `g_ramblk.bdif` 等指针在运行时始终为 0。

通过以下命令确认：

```bash
riscv64-linux-musl-readelf -r build/kernel_riscv64.elf
# 修复前：输出 18 条 R_RISCV_RELATIVE 条目，g_ramblk.bdif 对应条目在其中
# 修复后：输出 "There are no relocations in this file."
```

**修复**（`Makefile`，riscv64 分支）：

```makefile
CFLAGS  += -fno-pic -fno-pie   # 禁止编译器生成 PIC/PIE 代码
LDFLAGS := -no-pie             # 链接器不生成 PIE 可执行文件，消除 R_RISCV_RELATIVE
```

---

### Bug 3：`gp`（Global Pointer）寄存器未初始化

**现象**：使用 GP-relative 寻址访问 `.sdata/.sbss` 中的全局变量时，可能访问到错误地址（因 `gp` 初始值不确定）。

**原因**：  
RISC-V 编译器会对 `.sdata` 中的小型全局变量使用 `gp`-relative 寻址优化（通过 `gp ± 2KB` 窗口直接寻址，省去地址加载指令）。这要求 `gp` 寄存器在进入 C 代码前被初始化为 `__global_pointer$`（链接器导出的符号，定义为 `.sdata` 段内 +0x800 偏移处）。原始 `boot.S` 未初始化 `gp`。

**修复**：

1. **`boot/riscv64/link.ld`**：添加 `.sdata` 节并定义 `__global_pointer$`：

```ld
.sdata : {
    __global_pointer$ = . + 0x800;
    *(.srodata*) *(.sdata .sdata.*)
}
```

2. **`boot/riscv64/boot.S`**：在进入 C 代码前初始化 `gp`（必须用 `.option norelax` 防止汇编器自身使用 GP-relative 访问该符号，造成循环依赖）：

```asm
.option push
.option norelax
la      gp, __global_pointer$
.option pop
```

---

### Bug 4：栈容量不足（潜在风险）

**原因**：原始栈大小为 16KB，lwext4 的 `ext4_mount` 调用链较深（`ext4_fs_init` → `ext4_sb_read` → `ext4_block_readbytes` → `ext4_bdif_bread` → `ramblk_bread`），加上 `klog` 每次调用消耗 512 字节栈空间，深层调用可能导致栈溢出。

**修复**（`boot/riscv64/boot.S`）：将栈从 16KB 扩大到 64KB：

```asm
stack_bottom:
    .space 0x10000   /* 64KB，原为 0x4000 (16KB) */
stack_top:
```

---

## 修改文件汇总

| 文件 | 修改内容 |
|---|---|
| `kernel/main.c` | RISC-V 路径中将 `exception_init()` 移至 `fs_init()` 之前 |
| `Makefile` | riscv64 分支添加 `CFLAGS += -fno-pie`、`LDFLAGS := -no-pie` |
| `boot/riscv64/boot.S` | 添加 `gp` 初始化；栈从 16KB 扩大到 64KB |
| `boot/riscv64/link.ld` | 添加 `.sdata` 节，定义 `__global_pointer$` |

---

## 验证

修复后运行 `make ARCH=riscv64 run-fs`，输出：

```
[INFO] Initializing exception handler...
[INFO] RISC-V exception init: stvec=0x80204c90, sstatus.SIE enabled
[INFO] [fs] Mounting 'ramblk0' at '/'...
[INFO] [fs] Root filesystem mounted successfully at '/'
[INFO] [fs] Root directory entries:
[INFO] Initializing timer...
```

内核不再重启，ext4 根文件系统挂载成功。

---

## 关键教训

1. **裸机内核必须以 `-no-pie` 链接**，否则 `.data` 中的指针初始化值不会被写入，在运行时全部表现为 NULL。这个问题在 x86_64 上不出现（x86_64 分支 Makefile 已有 `-no-pie`），是平台不一致导致的隐患。

2. **异常向量必须在任何可能触发异常的代码之前设置**。对 RISC-V 来说尤其重要，因为 OpenSBI 对未处理的 S-mode 异常的默认行为是重置 hart，而不是挂起，调试时容易误判为其他原因。

3. **RISC-V GP-relative 优化需要显式初始化 `gp`**，链接脚本中也需要导出 `__global_pointer$`，否则对 `.sdata` 中变量的访问结果不可预期。
