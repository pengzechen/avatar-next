# AArch64 NEON 启用指南

> 本文只讲 **CPACR_EL1.FPEN 开关**与 NEON 工具函数。
> **FP 寄存器状态怎么保存**（陷阱帧 / 任务切换 / 向量表 128 字节预算）见
> [FP_SIMD_CONTEXT.md](FP_SIMD_CONTEXT.md) —— 动任何 FP 相关汇编前必读。

## 启用 NEON 工具函数

在 [include/aarch64/cpu.h](../../../include/aarch64/cpu.h) 中提供了 NEON 控制接口。

### API 说明

#### 1. `aarch64_enable_neon()` - 启用 NEON

```c
static inline void aarch64_enable_neon(void);
```

**功能**：设置 `CPACR_EL1.FPEN = 0b11`，EL0/EL1 执行 NEON/浮点指令不再陷入

**原理**：
```assembly
mrs x0, cpacr_el1          # 读取 CPACR_EL1
orr x0, x0, #(3 << 20)     # 设置 FPEN[21:20] = 0b11
msr cpacr_el1, x0          # 写回 CPACR_EL1
isb                        # 指令同步屏障
```

**CPACR_EL1.FPEN 字段**：
- `0b00`: 执行 NEON/FP 时陷入（**复位值**）
- `0b01`: 只在 EL0 陷入
- `0b10`: 保留
- `0b11`: ✅ EL0/EL1 都不陷入

#### 2. `aarch64_disable_neon()` - 禁用 NEON

```c
static inline void aarch64_disable_neon(void);
```

**功能**：将 FPEN 设为 `0b01`，EL0 执行 NEON/FP 会陷入

⚠️ 内核自己有 FP 状态（见 FP_SIMD_CONTEXT.md）之后，**不要**在内核执行期调用它。

#### 3. `aarch64_is_neon_enabled()` - 检查状态

#### 4. `aarch64_get_cpacr()` - 读取寄存器

## 什么时候打开 FPEN

**实际生效点在 [boot/aarch64/boot.S](../../../boot/aarch64/boot.S) 的 `init_el2_vhe`**：
BSP 与所有 AP 共用该宏，在 `msr vbar_el1` 之前就把 FPEN 置成 0b11。

原因：向量表一生效，异常入口的 `SAVE_REGS` 就会保存 q0-q31（`stp q`）。
FPEN 还是复位值 0b00 时，这些指令会陷入**同一个向量** → 无限递归。

```c
/* kernel/main.c 与 kernel/task/cpu.c 里的调用保留（幂等）*/
aarch64_enable_neon();
```

VHE（E2H=1）下 EL2 访问 `cpacr_el1` 落在 `CPACR_EL2`，正是同时门控 EL1 与 EL0 的
那一份。

## memcpy 的 NEON 优化

[include/aarch64/string_impl.h](../../../include/aarch64/string_impl.h)：

- `memcpy_neon()`：`ld1`/`st1` 128-bit 批量传输（每 16 字节）
- `memcpy()` 按阈值分派：**> 128 字节走 NEON**，否则走 `memcpy_generic()`

只把**目的地址**对齐到 16 字节即可：`ld1/st1` 的 `{v0.16b}` 变体是按字节访问的，
不要求 16 字节对齐（与 `ldr q`/`ldp q` 不同）。若按两边同时对齐来写，源和目的
奇偶不同时（如网络缓冲区拷进对齐堆块）对齐循环永不退出，整个拷贝退化成逐字节。

## 注意事项

1. **FPEN 必须先于向量表**（见上），否则是挂死而不是可诊断的 panic
2. **FP 寄存器状态有 4 个保存点**（陷阱帧 / 任务切换 / 三处伪帧 / fork 恢复），
   漏掉任一层都会静默破坏状态 —— 见 [FP_SIMD_CONTEXT.md](FP_SIMD_CONTEXT.md)
3. **向量表每个表项只有 128 字节**，超长代码必须挪到表外（如 `fp_save_common`），
   `VEC_ALIGN` 会在汇编期挡住
4. **性能**：每次陷阱约 512 B 的 FP 存取；这是无条件保存的代价，换来的是
   内核态与用户态 FP 状态都不会被异步事件破坏
5. **内核浮点只有 `float`/`double`**：未链接 libgcc，`long double` 会引出
   `__addtf3` 等帮助函数
