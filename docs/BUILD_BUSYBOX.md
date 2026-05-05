# Busybox 1.37.0 编译指南

本文档详细说明 Avatar OS 项目中三个架构（x86_64、AArch64、RISC-V 64）的 busybox 二进制文件的编译过程。

## 目录

- [编译结果](#编译结果)
- [前置条件](#前置条件)
- [编译配置](#编译配置)
- [x86_64 编译](#x86_64-编译)
- [AArch64 编译](#aarch64-编译)
- [RISC-V 64 编译](#risc-v-64-编译)
- [配置说明](#配置说明)
- [验证方法](#验证方法)

---

## 编译结果

三个架构的 busybox 二进制文件位于 `apps/` 目录：

| 架构 | 文件路径 | 大小 | 链接方式 | Debuginfo |
|------|----------|------|----------|-----------|
| x86_64 | `apps/busybox-x86_64` | 7.4M | 静态链接 | ✅ 包含 |
| AArch64 | `apps/busybox-aarch64` | 5.8M | static-pie | ✅ 包含 |
| RISC-V 64 | `apps/busybox-riscv64` | 5.1M | static-pie | ✅ 包含 |

**共同特性：**
- ✅ 静态链接（不依赖动态库）
- ✅ 包含完整调试信息（`-g` 编译选项）
- ✅ 已禁用 SHA1SUM 和 SHA256SUM 工具

---

## 前置条件

### 1. 源码准备

```bash
# 下载 busybox 1.37.0 源码
cd apps/
wget https://busybox.net/downloads/busybox-1.37.0.tar.bz2
tar xjf busybox-1.37.0.tar.bz2
```

### 2. 工具链要求

**x86_64（本地编译）：**
```bash
# 需要 gcc 和相关开发库
sudo apt install gcc make libc6-dev
```

**AArch64（交叉编译）：**
```bash
# musl-libc 交叉编译工具链
aarch64-linux-musl-gcc

# 检查工具链
which aarch64-linux-musl-gcc
# 输出: /home/ajax/SoftWare/compiler/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc
```

**RISC-V 64（交叉编译）：**
```bash
# musl-libc 交叉编译工具链
riscv64-linux-musl-gcc

# 检查工具链
which riscv64-linux-musl-gcc
```

---

## 编译配置

### 关键配置选项

所有架构统一使用以下配置：

1. **静态链接**：`CONFIG_STATIC=y`
   - 不依赖动态链接库
   - 适合嵌入式和裸机环境

2. **禁用 SHA1/SHA256 工具**：
   - `# CONFIG_SHA1SUM is not set`
   - `# CONFIG_SHA256SUM is not set`

3. **调试信息**：`EXTRA_CFLAGS="-g"`
   - 包含完整符号表
   - 支持 GDB 调试

4. **AArch64 特定**：禁用 SHA1 硬件加速
   - `# CONFIG_SHA1_HWACCEL is not set`
   - 避免编译错误

---

## x86_64 编译

### 编译步骤

```bash
# 1. 进入源码目录
cd /home/ajax/Proj/OS/Thread-Process-Lock/avatar/apps/busybox-1.37.0

# 2. 生成默认配置
make defconfig

# 3. 修改配置文件
sed -i 's/^# CONFIG_STATIC is not set$/CONFIG_STATIC=y/' .config
sed -i 's/^CONFIG_SHA1SUM=y$/# CONFIG_SHA1SUM is not set/' .config
sed -i 's/^CONFIG_SHA256SUM=y$/# CONFIG_SHA256SUM is not set/' .config

# 4. 禁用 tc 工具（避免编译错误）
sed -i 's/^CONFIG_TC=y$/# CONFIG_TC is not set/' .config

# 5. 编译（带调试信息）
make -j$(nproc) EXTRA_CFLAGS="-g"

# 6. 复制到目标位置
cp busybox_unstripped /home/ajax/Proj/OS/Thread-Process-Lock/avatar/apps/busybox-x86_64
```

### 验证

```bash
file apps/busybox-x86_64
# 输出: ELF 64-bit LSB executable, x86-64, statically linked,
#       with debug_info, not stripped
```

---

## AArch64 编译

### 编译步骤

```bash
# 1. 准备独立的构建目录
mkdir -p /tmp/busybox-aarch64-build
cd /tmp/busybox-aarch64-build
tar xjf /home/ajax/Proj/OS/Thread-Process-Lock/avatar/apps/busybox-1.37.0.tar.bz2
cd busybox-1.37.0

# 2. 生成 AArch64 配置
make defconfig CROSS_COMPILE=aarch64-linux-musl- ARCH=arm64

# 3. 修改配置文件
sed -i 's/^# CONFIG_STATIC is not set$/CONFIG_STATIC=y/' .config
sed -i 's/^CONFIG_SHA1SUM=y$/# CONFIG_SHA1SUM is not set/' .config
sed -i 's/^CONFIG_SHA256SUM=y$/# CONFIG_SHA256SUM is not set/' .config

# 4. 禁用 SHA1 硬件加速（避免编译错误）
sed -i 's/^CONFIG_SHA1_HWACCEL=y$/# CONFIG_SHA1_HWACCEL is not set/' .config

# 5. 编译（带调试信息）
make -j$(nproc) CROSS_COMPILE=aarch64-linux-musl- ARCH=arm64 EXTRA_CFLAGS="-g"

# 6. 复制到目标位置
cp busybox_unstripped /home/ajax/Proj/OS/Thread-Process-Lock/avatar/apps/busybox-aarch64

# 7. 清理临时文件
rm -rf /tmp/busybox-aarch64-build
```

### 验证

```bash
file apps/busybox-aarch64
# 输出: ELF 64-bit LSB pie executable, ARM aarch64, static-pie linked,
#       with debug_info, not stripped
```

---

## RISC-V 64 编译

### 编译步骤

```bash
# 1. 准备独立的构建目录
mkdir -p /tmp/busybox-riscv64-build
cd /tmp/busybox-riscv64-build
tar xjf /home/ajax/Proj/OS/Thread-Process-Lock/avatar/apps/busybox-1.37.0.tar.bz2
cd busybox-1.37.0

# 2. 生成 RISC-V 配置
make defconfig CROSS_COMPILE=riscv64-linux-musl- ARCH=riscv

# 3. 修改配置文件
sed -i 's/^# CONFIG_STATIC is not set$/CONFIG_STATIC=y/' .config
sed -i 's/^CONFIG_SHA1SUM=y$/# CONFIG_SHA1SUM is not set/' .config
sed -i 's/^CONFIG_SHA256SUM=y$/# CONFIG_SHA256SUM is not set/' .config

# 4. 编译（带调试信息）
make -j$(nproc) CROSS_COMPILE=riscv64-linux-musl- ARCH=riscv EXTRA_CFLAGS="-g"

# 5. 复制到目标位置
cp busybox_unstripped /home/ajax/Proj/OS/Thread-Process-Lock/avatar/apps/busybox-riscv64

# 6. 清理临时文件
rm -rf /tmp/busybox-riscv64-build
```

### 验证

```bash
file apps/busybox-riscv64
# 输出: ELF 64-bit LSB pie executable, UCB RISC-V, static-pie linked,
#       with debug_info, not stripped
```

---

## 配置说明

### 为什么禁用 SHA1SUM/SHA256SUM？

1. **减少代码体积**：SHA1/SHA256 算法实现占用一定空间
2. **避免依赖**：某些架构的 SHA 硬件加速可能导致编译问题
3. **简化调试**：对于操作系统开发，这些工具不是必需的

### 为什么使用静态链接？

1. **独立性**：不依赖动态链接库，适合嵌入式环境
2. **简化**：避免动态链接器相关的复杂性
3. **一致性**：确保在不同环境中的行为一致

### 为什么包含调试信息？

1. **调试支持**：支持 GDB 调试用户态程序
2. **开发便利**：在操作系统开发过程中，符号信息非常重要
3. **问题定位**：崩溃时可以获取更详细的堆栈信息

### x86_64 与其他架构的区别

- **x86_64**: 生成传统静态可执行文件
- **AArch64/RISC-V**: 生成 static-pie 可执行文件
  - PIE (Position Independent Executable) 支持 ASLR
  - 更现代的链接方式
  - musl-libc 工具链的默认行为

---

## 验证方法

### 1. 检查文件类型

```bash
cd /home/ajax/Proj/OS/Thread-Process-Lock/avatar/apps

# 检查所有三个架构
for arch in x86_64 aarch64 riscv64; do
    echo "=== $arch ==="
    file busybox-$arch
done
```

### 2. 检查调试符号

```bash
# 使用 readelf 检查调试信息
readelf -S busybox-x86_64 | grep debug

# 使用 nm 检查符号表
nm busybox-aarch64 | head -20

# 使用 size 查看段大小
size busybox-riscv64
```

### 3. 检查依赖库

```bash
# 应该显示 "not a dynamic executable"
ldd busybox-x86_64
```

### 4. 验证 SHA 工具已禁用

```bash
# 查看 busybox 支持的命令列表
./busybox-x86_64 --list | grep -E 'sha1sum|sha256sum'

# 应该没有输出（表示已禁用）
```

---

## 常见问题

### Q1: 编译时出现 "undefined reference to sha1_process_block64_shaNI"

**原因**：SHA1 硬件加速在交叉编译环境可能不可用。

**解决**：
```bash
sed -i 's/^CONFIG_SHA1_HWACCEL=y$/# CONFIG_SHA1_HWACCEL is not set/' .config
```

### Q2: 编译时出现 networking/tc.c 错误

**原因**：某些内核头文件在交叉编译时可能不兼容。

**解决**：
```bash
sed -i 's/^CONFIG_TC=y$/# CONFIG_TC is not set/' .config
```

### Q3: 静态链接失败，提示 "cannot find -lc"

**原因**：静态库可能未安装。

**解决**：
```bash
# Ubuntu/Debian
sudo apt install libc6-dev

# 或检查 musl-libc 工具链是否正确安装
which aarch64-linux-musl-gcc
```

---

## 参考资源

- **Busybox 官方文档**: https://busybox.net/FAQ.html
- **Busybox 配置选项**: https://busybox.net/BusyBox.html
- **交叉编译工具链**: https://musl.cc/
- **Avatar OS 项目**: /home/ajax/Proj/OS/Thread-Process-Lock/avatar/

---

**文档版本**: 1.0
**更新日期**: 2026-05-05
**维护者**: Avatar OS Team
