# 字符串 / 内存函数：三层结构与架构优化

## 三层结构

| 文件 | 角色 | 内容 |
|---|---|---|
| `include/string_internal.h` | **实现层** | `memcpy_generic` / `memset_generic` / `memmove_generic`，包含架构头，定义 `memcpy_arch` / `memset_arch` |
| `include/string.h` | **公开 API** | `strlen`/`strcmp`/… 全套 `static inline`；`memcpy`/`memset`/`memmove` 转发到实现层 |
| `lib/string.c` | **外部符号** | 把 `memcpy`/`memset`/`memmove` 定义成外部符号，同样转发到实现层 |
| `include/{aarch64,riscv64,x86_64}/string_impl.h` | **架构实现** | 各自的 `memcpy_arch` / `memset_arch`（aarch64 是 NEON + 阈值分派） |

### 为什么要拆成三层

`memcpy`/`memset` 有两个来源的调用者：

1. **源码里写的调用** —— 由 `string.h` 的 `static inline` 处理（内联，或退化成调用点自己的副本）；
2. **编译器自己生成的调用** —— 大结构体整体赋值、GCC 的"循环 → memcpy"变换等，会直接发一个
   对外部符号的 `bl memcpy`：
   ```asm
   0xc0c: bl memcpy        # 例如 fork 路径里 816 字节 trap_frame_t 的整体赋值
   ```

第 2 类**不会**走 `static inline` 版本：GCC 生成的是库函数调用，绑定到外部符号。
如果外部符号是另一份（更差的）实现，就会出现"源码里写的是 NEON、编译器生成的是逐字节循环"
这种分裂 —— 实际踩过：`lib/string.c` 里原本那个 0x20 字节的逐字节 `memcpy` 就是这么被
fork/clone 的 trap frame 拷贝用上的。

所以实现和"公开名字"必须分家：公开名字留给 `string.h` 的 `static inline`，实现在
`string_internal.h` 里用**具名**函数（`memcpy_arch` 等）提供，`lib/string.c` 的外部符号
也转发到同一份实现。三者写同一份代码，谁被调到都一样快。

> 顺带解释 `lib/string.c` 里那句"不包含 string.h"的注释：同一个翻译单元里，同名标识符
> 既有内部链接（`static inline memcpy`）又有外部链接（`void *memcpy(...)`）是未定义行为
> （C11 6.2.2p7），所以它只能包含实现层头。

## 各架构状态

### AArch64
- `memcpy_neon()`：`ld1 {v0.16b}` / `st1 {v0.16b}`，阈值 `MEMCPY_NEON_THRESHOLD = 128` 字节
- `memset_neon()`：`dup v0.16b, w` + `st1`，阈值 `MEMSET_NEON_THRESHOLD = 128` 字节
- 两者都**只对齐目的地址**：`ld1/st1` 的 `{v0.16b}` 变体是按字节访问的，架构上不要求
  16 字节对齐（与 `ldr q`/`ldp q` 不同）。早先 `memcpy_neon` 要求 src/dest 两边同时
  16 字节对齐，源和目的奇偶不同时（如网络缓冲区拷进对齐堆块）对齐循环永不退出，
  整段退化成逐字节。
- 前置条件：`CPACR.FPEN` 已打开（`boot/aarch64/boot.S` 在装向量表前就开了），
  且 FP 状态有陷阱帧 / 切换帧两层保存 —— 见 [FP_SIMD_CONTEXT.md](arch/aarch64/FP_SIMD_CONTEXT.md)

### x86_64
- 目前转发到通用实现。
- 要做 SSE2/AVX 版本，得先给相关翻译单元单独开向量开关（内核现在带
  `-mno-mmx -mno-sse`），并按 aarch64 的做法把 FP 状态纳入上下文保存。

### RISC-V 64
- 目前转发到通用实现，预留 V 扩展。

## 通用实现的对齐约束（重要）

`memcpy_generic` 用**普通 GPR** 做 8/4/2 字节访问，所以要求 src 和 dest **同时**对齐才敢加宽；
只有一边对齐（更准确说：两者的**相对偏移是奇数**）时，无论推进多少字节都不可能同时对齐 ——
这种情况会退化成逐字节拷贝。这是该约束下的必然结果，不是缺陷：

- 想再快，要么做"两次对齐读取 + 移位拼合"（纯 GPR 也能做，成本是复杂度），
- 要么确认目标平台允许非对齐访问 —— **RISC-V 不能想当然**：SG2002/C906 这类真实硬件
  不保证支持非对齐访问，而本仓库有 `PLATFORM_SG2002`。

AArch64 的 NEON 版本没有这个问题，因为 `ld1/st1` 是按字节访问的。

## 自检

`tests/string_test.c` 覆盖 `memcpy`/`memset`/`memmove` 的**长度 × 源对齐 × 目的对齐**全组合
（长度矩阵跨过 128 字节阈值两侧），用逐字节参考实现比对，并检查拷贝区间之外的守卫字节没被踩：

```bash
make PLATFORM=qemu-virt-aarch64 test-string
# 期望：=== STRING TEST: PASS (4210 cases) ===
```

其中一条用例是大结构体整体赋值（`blob_b = blob_a;`），专门让编译器生成对外部符号
`memcpy` 的调用 —— 就是上面说的第 2 类路径。

开关是 `STRING_TEST=1`（见 Makefile §7），只影响 `kernel_main.o`，自检跑完继续正常启动。

## 使用方式

```c
#include "string.h"

memcpy(dest, src, n);   /* 自动走当前架构的最优实现 */
memset(p, 0, n);
```
