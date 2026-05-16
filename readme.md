# Avatar OS

![Avatar OS](https://img.shields.io/badge/OS-Avatar-blue)
![Architectures](https://img.shields.io/badge/arch-AArch64%20%7C%20RISC--V%2064%20%7C%20x86__64-green)
![License](https://img.shields.io/badge/license-MIT-orange)

**Avatar OS** 是一个 64 位操作系统内核项目，专注于跨架构支持和底层抽象设计。

## ✨ 核心特性

- **🎯 跨架构支持**：AArch64 (ARM 64-bit)、RISC-V 64-bit、x86_64 (AMD64/Intel 64)
- **⚡ Freestanding 环境**：不依赖标准库，完全自包含实现
- **🔧 零成本抽象**：编译时配置，运行时零开销
- **🛡️ 类型安全**：利用编译器扩展实现类型安全的宏
- **🔒 内存安全**：集成内存屏障的同步和 I/O 操作
- **📦 用户态支持**：支持运行 busybox 及用户程序
- **🗂️ 文件系统**：支持 ext4 rootfs 镜像
- **🖥️ 虚拟化（vCPU）**：RISC-V H 扩展 Hypervisor，支持 vCPU 创建、运行与陷入处理

## 🚀 快速开始

### 前置条件

#### x86_64
```bash
# Ubuntu/Debian
sudo apt install gcc make libc6-dev qemu-system-x86

# Fedora/RHEL
sudo dnf install gcc make glibc-devel qemu-system-x86
```

#### AArch64
```bash
# 安装 musl 交叉编译工具链
sudo apt install gcc-aarch64-linux-gnu make qemu-system-arm

# 或使用 musl-gcc
# 下载 https://musl.cc/
# 解压并添加到 PATH
```

#### RISC-V 64
```bash
# 安装 musl 交叉编译工具链
sudo apt install gcc-riscv64-linux-gnu make qemu-system-misc

# 或使用 musl-gcc
# 下载 https://musl.cc/
# 解压并添加到 PATH
```

> **注意**：rootfs 构建使用 `mkfs.ext4 -d`，需要 e2fsprogs ≥ 1.43（2016 年后的主流发行版均满足），
> **无需 sudo**，无需 loop mount。

### 编译和运行

#### RISC-V 64
```bash
# 首次：编译内核 + 创建 rootfs（仅需一次，镜像保存为 build/rootfs-riscv64.img）
make ARCH=riscv64 rootfs

# 启动 QEMU
make ARCH=riscv64 PLATFORM=qemu run-fs LOG=info -j4
```

#### AArch64
```bash
# 首次：编译内核 + 创建 rootfs（镜像保存为 build/rootfs-aarch64.img）
make ARCH=aarch64 rootfs

# 启动 QEMU
make ARCH=aarch64 PLATFORM=qemu run-fs LOG=info -j4
```

#### x86_64
```bash
# 首次：编译内核 + 创建 rootfs（镜像保存为 build/rootfs-x86_64.img）
make ARCH=x86_64 rootfs

# 启动 QEMU
make ARCH=x86_64 PLATFORM=qemu run-fs LOG=info -j4
```

> 每个架构的 rootfs 镜像独立存储，**切换架构无需 `make clean`**。  
> 仅当用户程序（`apps/`）发生变化时，Make 才会自动重建对应镜像。  
> 如需手动重建 rootfs（例如替换 busybox），可单独运行：
> ```bash
> # 方式一：通过 make（推荐）
> make ARCH=riscv64 rootfs
> # 方式二：通过脚本（需先 make ARCH=riscv64 编译内核和 apps）
> ./install-apps.sh riscv64
> ```

### 其他构建选项

```bash
# 调试版本（启用日志和断言）
make ARCH=aarch64 LOG=debug ASSERT=panic

# 发布版本（零开销）
make ARCH=aarch64 LOG=none ASSERT=off

# 仅编译内核
make ARCH=aarch64 kernel

# 清理构建
make ARCH=aarch64 clean

# 查看帮助
make help
```

## 📁 项目结构

```
avatar/
├── Makefile              # 构建系统
├── readme.md             # 本文件
├── CLAUDE.md             # AI 上下文配置
├── PROJECT_REFERENCE.md  # 详细参考文档
│
├── include/              # 头文件（基础设施层）
│   ├── aarch64/         # AArch64 架构实现
│   ├── riscv64/         # RISC-V 架构实现
│   ├── x86_64/          # x86_64 架构实现
│   ├── types.h          # 基础类型定义
│   ├── arg.h            # 可变参数支持
│   ├── arch.h           # 架构检测宏
│   ├── barrier.h        # 内存屏障
│   ├── cache.h          # 缓存操作
│   ├── spinlock.h       # 自旋锁
│   ├── string.h         # 字符串操作
│   ├── klog.h           # 内核日志
│   ├── list.h           # 双向链表
│   ├── mmio.h           # 内存映射 I/O
│   └── assert.h         # 断言系统
│
├── lib/                  # 库实现
│   ├── klog.c           # 日志系统
│   ├── string.c         # 字符串函数
│   ├── vsnprintf.c      # 格式化输出
│   └── bitmap.c         # 位图操作
│
├── kernel/               # 内核代码
│   ├── main.c           # 内核入口
│   ├── task/            # 任务管理
│   ├── mm/              # 内存管理
│   ├── syscall/         # 系统调用
│   └── loader/          # 程序加载器
│
├── boot/                 # 启动代码
│   ├── aarch64/         # AArch64 启动
│   ├── riscv64/         # RISC-V 启动
│   └── x86_64/          # x86_64 启动
│
├── driver/               # 设备驱动
│   ├── uart/            # 串口驱动
│   ├── timer/           # 定时器驱动
│   └── interrupt/       # 中断控制器
│
├── fs/                   # 文件系统
│   └── lwext4/          # ext4 支持
│
├── apps/                 # 用户程序
│   ├── busybox-aarch64  # Busybox (AArch64)
│   ├── busybox-riscv64  # Busybox (RISC-V)
│   └── busybox-x86_64   # Busybox (x86_64)
│
├── platforms/            # 平台配置
│   └── qemu/            # QEMU 平台
│
├── config/               # 配置文件
│   ├── mem_layout.table # 内存布局
│   └── device_profile.table # 设备配置
│
├── tests/                # 测试代码
├── tools/                # 构建工具
└── docs/                 # 详细文档
```

## 🎯 核心功能

### 已实现模块

#### 基础设施
- ✅ **类型系统** (types.h) - 整数、指针、对齐宏
- ✅ **可变参数** (arg.h) - va_list 支持
- ✅ **架构检测** (arch.h) - 编译时架构识别

#### 内存和同步
- ✅ **内存屏障** (barrier.h) - 编译器/CPU 屏障
- ✅ **缓存操作** (cache.h) - clean/invalidate
- ✅ **自旋锁** (spinlock.h) - 包含 IRQ 变体

#### 工具库
- ✅ **字符串操作** (string.h) - strlen, strcmp, memcpy 等
- ✅ **双向链表** (list.h) - Linux 内核风格
- ✅ **内存映射 I/O** (mmio.h) - 带屏障的设备访问

#### 调试支持
- ✅ **内核日志** (klog.h) - 级别和模块控制
- ✅ **断言系统** (assert.h) - 运行时和编译时

#### 内核功能
- ✅ **任务管理** - 进程/线程创建、切换、退出
- ✅ **内存管理** - 页表、虚拟内存、堆分配
- ✅ **系统调用** - syscall 接口实现
- ✅ **程序加载** - ELF 加载器
- ✅ **中断处理** - 异常和中断支持
- ✅ **设备驱动** - UART、定时器、中断控制器
- ✅ **文件系统** - ext4 支持（lwext4）
- ✅ **虚拟化** - vCPU（RISC-V H 扩展 Hypervisor）：vCPU 创建、运行、陷入分发

### 设计原则

#### 1. 内联优先
所有头文件函数必须是 `static inline`，避免链接错误。

```c
// ✅ 正确
static inline void operation(void) { }

// ❌ 错误
extern void operation(void);
```

#### 2. 架构抽象模式
统一接口 + 架构特化实现：

```c
/* module.h */
#include "arch.h"
static inline void operation(void);

#if ARCH_AARCH64
    #include "aarch64/module_impl.h"
#elif ARCH_X86_64
    #include "x86_64/module_impl.h"
#elif ARCH_RISCV64
    #include "riscv64/module_impl.h"
#endif
```

#### 3. 类型安全
使用 `__typeof__` 实现类型安全的宏：

```c
#define MIN(a, b) __extension__ ({            \
    __typeof__(a) _a = (a);                   \
    __typeof__(b) _b = (b);                   \
    _a < _b ? _a : _b;                        \
})
```

#### 4. 集成日志
使用 klog 进行错误和调试输出：

```c
KLOG_ERROR("Critical error: %s", msg);
KLOG_DEBUG("Value: %d", value);
KLOG_MODULE_DEBUG(LOG_MODULE_UART, "UART init");
```

## 📚 文档索引

### 核心系统
- [内存屏障](docs/BARRIER.md) - barrier.h API 和实现
- [缓存操作](docs/CACHE.md) - cache.h DMA 和 MMIO 缓存管理
- [自旋锁](docs/SPINLOCK.md) - spinlock.h 同步原语

### 调试和日志
- [内核日志](docs/KLOG_GUIDE.md) - klog.h 日志系统和模块控制
- [断言系统](docs/ASSERT_GUIDE.md) - assert.h 运行时和编译时断言

### 数据结构和工具
- [双向链表](docs/LIST_API.md) - list.h Linux 风格链表
- [字符串操作](docs/STRING.md) - string.h 字符串和内存操作
- [MMIO](docs/MMIO.md) - mmio.h 内存映射 I/O

### 架构和平台
- [架构平台配置](docs/ARCH_PLATFORM_PROFILE_GUIDE.md) - 多架构多平台支持
- [AArch64 NEON](docs/arch/aarch64/NEON_USAGE.md) - NEON 优化

### 开发指南
- [Busybox 编译](docs/BUILD_BUSYBOX.md) - Busybox 交叉编译指南
- [系统调用实现](docs/SYSCALL_IMPLEMENTATION.md) - syscall 接口详解

### 故障排查
- [中断上下文切换](docs/INTERRUPT_CONTEXT_SWITCH.md) - 中断处理和任务切换
- [RISC-V ecall bug](docs/RISCV64_ECALL_OPENSBI_BUG.md) - OpenSBI 兼容性问题
- [用户进程状态](docs/USER_PROCESS_STATUS.md) - 用户态进程管理

## 🔧 配置选项

### 日志级别
```bash
LOG=none      # 禁用所有日志
LOG=error     # 仅错误
LOG=warn      # 警告和错误
LOG=info      # 信息、警告和错误（默认）
LOG=debug     # 调试信息及以上
LOG=trace     # 所有日志包括跟踪
```

### 断言模式
```bash
ASSERT=panic  # 启用断言，失败时 panic（默认）
ASSERT=off    # 禁用所有断言（发布模式）
```

### 架构和平台
```bash
ARCH=x86_64       # x86_64 架构
ARCH=aarch64      # AArch64 架构
ARCH=riscv64      # RISC-V 64 架构

PLATFORM=qemu     # QEMU 平台（默认）
```

## 🛠️ 开发环境

### 推荐工具
- **编译器**: GCC 9+ 或 musl-gcc
- **调试器**: GDB + QEMU gdbserver
- **编辑器**: VSCode + C/C++ 扩展
- **版本控制**: Git

### VSCode 配置
项目包含 `.vscode/` 配置，支持：
- IntelliSense 代码补全
- 架构特定的编译命令
- 调试配置

### 调试
```bash
# 启动 QEMU with gdbserver
make ARCH=aarch64 run-fs QEMU_GDB=1234

# 连接 GDB
aarch64-linux-gnu-gdb build/kernel.elf
(gdb) target remote :1234
```

## 📖 参考资料

- [RISC-V Reader](https://riscv.org/technical/specifications/)
- [ARM Architecture Reference Manual](https://developer.arm.com/documentation/)
- [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [Linux Kernel Source](https://github.com/torvalds/linux)

## 🤝 贡献

欢迎提交 Issue 和 Pull Request！

## 📄 许可证

MIT License

---

**版本**: 1.0  
**更新**: 2026-05-16  
**项目**: Avatar OS
