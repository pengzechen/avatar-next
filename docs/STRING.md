# 字符串函数跨架构实现

本项目实现了跨架构的字符串函数抽象层，支持架构特定优化。

## 架构实现

### x86_64
- [include/x86_64/string_impl.h](include/x86_64/string_impl.h)
- 当前使用通用实现
- TODO: 可添加 SSE2/AVX2 优化

### AArch64 (ARM 64-bit)
- [include/aarch64/string_impl.h](include/aarch64/string_impl.h)
- **NEON 优化**: 使用 NEON 128-bit 寄存器
- 对于大于 128 字节的拷贝自动使用 NEON 优化
- 小块数据使用通用实现

### RISC-V64
- [include/riscv64/string_impl.h](include/riscv64/string_impl.h)
- 当前使用通用实现
- TODO: 可添加 RISC-V V 扩展优化

## 架构抽象设计

### 通用函数 (include/string.h)
所有架构共享的纯 C 实现：
- `strlen`, `strcpy`, `strncpy`
- `strcmp`, `strncmp`, `strcat`
- `strchr`, `strstr`
- `memset`, `memcmp`, `memmove`, `memchr`
- `atol`

### 架构优化函数
根据架构自动选择最优实现：
- **x86_64**: 通用实现（预留 SSE/AVX 接口）
- **AArch64**: NEON 优化的 `memcpy`
- **RISC-V64**: 通用实现（预留 V 扩展接口）

## 使用方式

```c
#include "string.h"

/* 自动使用当前架构的最优实现 */
memcpy(dest, src, n);  /* AArch64 上使用 NEON 优化 */
```

## 编译

```bash
make ARCH=x86_64   # 使用通用实现
make ARCH=aarch64  # 使用 NEON 优化
make ARCH=riscv64  # 使用通用实现
```

## 性能优化策略

### AArch64 NEON 优化
- 阈值：> 128 字节使用 NEON
- 对齐：16 字节边界对齐
- 指令：`ld1`/`st1` 128-bit 批量传输

### 未来优化方向
- **x86_64**: SSE2/AVX2/AVX-512 向量化
- **RISC-V**: V 扩展向量指令
- **AArch64**: 更激进的 NEON 使用策略
