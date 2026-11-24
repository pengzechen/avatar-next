# 从文件系统加载执行程序

> 状态：本文档部分内容早于当前实现。平坦二进制加载器 `bin_loader` 已删除；
> 现在 execve 走 `kernel/linux/exec/elf_loader.c`（文件 I/O 层）→
> `task_execve()`（`kernel/linux/exec/exec.c`）→ `kernel/mm/elf_image.c`（ELF 解析与段映射）。

## 已实现的功能

### 1. execve 系统调用
- **系统调用号**: SYS_EXECVE (4)
- **接口**: `int64_t execve(const char *pathname, char **argv, char **envp)`
- **功能**: 从文件系统加载并执行可执行程序

### 2. ELF 程序加载器
- **文件**: [kernel/linux/exec/elf_loader.c](../kernel/linux/exec/elf_loader.c)（文件 I/O、shebang、PT_INTERP）
- **解析/映射**: [kernel/mm/elf_image.c](../kernel/mm/elf_image.c)
- **支持格式**: ELF64（动态链接需提供 interpreter）
- **说明**: 旧的平坦二进制加载器 `bin_loader` 已删除

### 3. 用户程序
- **目录**: [apps/](../apps/)
- **示例**: rootfs 中的 `/busybox`（ELF，由 `linux/exec/elf_loader.c` 加载）

## 使用步骤

### 1. 编译内核和应用程序

```bash
make ARCH=aarch64 kernel
```

这将：
- 编译内核
- 编译 `apps/` 目录下的所有 `.S` 程序
- 生成二进制文件到 `build/` 目录

### 2. 创建 rootfs 镜像

```bash
# 创建 rootfs 镜像（已包含 /busybox）
make ARCH=aarch64 rootfs

# 需要额外程序时，挂载后拷入（需要 sudo 权限）
sudo mount -o loop build/rootfs.img /tmp/avatar_mnt
sudo cp build/your_app /tmp/avatar_mnt/your_app
sudo umount /tmp/avatar_mnt
```

### 3. 运行内核（带文件系统）

```bash
make ARCH=aarch64 run-fs
```

这将启动 QEMU 并加载 rootfs.img 到内存地址 0x60000000。

## 用户程序开发

### 编写用户程序

在 `apps/` 目录下创建汇编程序，例如 `myapp.S`：

```asm
/*
 * apps/myapp.S - 我的用户程序
 */

#define SYS_WRITE        20
#define SYS_EXIT         0

.section .text
.global _start

_start:
    /* 输出消息 */
    adr     x0, msg
    mov     x1, #20
    mov     x8, #SYS_WRITE
    svc     #0

    /* 退出 */
    mov     x0, #0
    mov     x8, #SYS_EXIT
    svc     #0

    /* 不应该到达这里 */
    b       .

msg:
    .asciz "Hello from myapp!\n"
    .align  3
```

### 编译说明

- **入口点**: 必须是 `_start`
- **代码段**: 使用 `.section .text`
- **数据**: 可以直接内嵌在代码之后（使用 `adr` 指令获取地址）
- **系统调用**: 使用 `svc #0` 触发，系统调用号放在 `x8`

### 系统调用列表

```c
#define SYS_EXIT          0
#define SYS_YIELD         1
#define SYS_GETPID        2
#define SYS_SLEEP         3
#define SYS_EXECVE        4
#define SYS_WRITE         20
```

## 在程序中使用 execve

### 汇编方式

```asm
/* 执行 /your_app 程序 */
adr     x0, pathname    /* x0 = 程序路径 */
mov     x1, #0          /* x1 = argv (暂未使用) */
mov     x2, #0          /* x2 = envp (暂未使用) */
mov     x8, #SYS_EXECVE
svc     #0

pathname:
    .asciz "/your_app"
```

### C 语言方式

```c
#include "unistd.h"

void load_program(void) {
    char *path = "/your_app";
    char *argv[] = {NULL};
    char *envp[] = {NULL};

    execve(path, argv, envp);

    /* 如果到达这里，说明 execve 失败了 */
}
```

## 当前限制

1. **格式支持**: ELF64；动态链接程序需要提供 interpreter（如 `/lib/ld-musl-*.so.1`）
2. **参数传递**: 支持 argv/envp（Linux ABI 初始栈）
3. **错误处理**: 加载失败时返回负 errno
4. **地址空间**: 每个用户进程有独立的用户页表

## 故障排除

### 文件系统挂载失败

```
[ERROR] [fs] ext4_mount failed: 95
```

**原因**: rootfs.img 未加载到 QEMU 内存

**解决**: 使用 `make ARCH=aarch64 run-fs` 而不是 `make ARCH=aarch64 run`

### 程序未找到

```
[ERROR] [loader] Failed to open '/your_app': -2
```

**原因**: 文件不存在于 rootfs 中

**解决**: 检查文件是否正确复制到 rootfs.img

### 内存分配失败

```
[ERROR] [loader] Failed to allocate memory for program
```

**原因**: 物理内存不足

**解决**: 检查程序大小是否超过 MAX_FILE_SIZE (1MB)

## 相关文档

- [系统调用实现](SYSCALL_IMPLEMENTATION.md)
- [任务与调度实现](PROJECT_REFERENCE.md)
- [异常/中断与上下文切换](INTERRUPT_CONTROL_COMPARISON.md)

---
**更新时间**: 2026-05-03
**版本**: 1.0
