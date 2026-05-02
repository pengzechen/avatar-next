# 从文件系统加载执行程序

## 已实现的功能

### 1. execve 系统调用
- **系统调用号**: SYS_EXECVE (4)
- **接口**: `int64_t execve(const char *pathname, char **argv, char **envp)`
- **功能**: 从文件系统加载并执行可执行程序

### 2. 二进制程序加载器
- **文件**: [kernel/syscall/bin_loader.c](../kernel/syscall/bin_loader.c)
- **支持格式**: 平面二进制（原始二进制代码）
- **加载地址**: 0x10000（虚拟地址）
- **用户栈地址**: 0x100000（1MB）

### 3. 用户程序编译
- **目录**: [apps/](../apps/)
- **示例程序**: [apps/test_exec.S](../apps/test_exec.S)
- **链接脚本**: [apps/app.ld](../apps/app.ld)

## 使用步骤

### 1. 编译内核和应用程序

```bash
make ARCH=aarch64 kernel
```

这将：
- 编译内核
- 编译 `apps/` 目录下的所有 `.S` 程序
- 生成二进制文件到 `build/` 目录

### 2. 创建 rootfs 镜像并添加程序

```bash
# 创建空的 rootfs 镜像
make ARCH=aarch64 rootfs

# 手动挂载并添加程序（需要 sudo 权限）
mkdir -p /tmp/avatar_mnt
sudo mount -o loop build/rootfs.img /tmp/avatar_mnt
sudo cp build/test_exec.bin /tmp/avatar_mnt/test_exec
sudo chmod +x /tmp/avatar_mnt/test_exec
sudo umount /tmp/avatar_mnt
rmdir /tmp/avatar_mnt
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
/* 执行 /test_exec 程序 */
adr     x0, pathname    /* x0 = 程序路径 */
mov     x1, #0          /* x1 = argv (暂未使用) */
mov     x2, #0          /* x2 = envp (暂未使用) */
mov     x8, #SYS_EXECVE
svc     #0

pathname:
    .asciz "/test_exec"
```

### C 语言方式

```c
#include "unistd.h"

void load_program(void) {
    char *path = "/test_exec";
    char *argv[] = {NULL};
    char *envp[] = {NULL};

    execve(path, argv, envp);

    /* 如果到达这里，说明 execve 失败了 */
}
```

## 当前限制

1. **格式支持**: 仅支持平面二进制格式，不支持 ELF
2. **参数传递**: argv 和 envp 暂未实现
3. **错误处理**: 加载失败时返回负错误码
4. **内存管理**: 程序加载后不会自动释放内存
5. **地址空间**: 所有程序共享相同的虚拟地址空间

## 测试程序

### test_exec

位置: [apps/test_exec.S](../apps/test_exec.S)

功能：
- 输出欢迎消息
- 进入循环，定期输出状态
- 演示从文件系统加载的程序可以正常运行

## 故障排除

### 文件系统挂载失败

```
[ERROR] [fs] ext4_mount failed: 95
```

**原因**: rootfs.img 未加载到 QEMU 内存

**解决**: 使用 `make ARCH=aarch64 run-fs` 而不是 `make ARCH=aarch64 run`

### 程序未找到

```
[ERROR] [loader] Failed to open '/test_exec': -2
```

**原因**: 文件不存在于 rootfs 中

**解决**: 检查文件是否正确复制到 rootfs.img

### 内存分配失败

```
[ERROR] [loader] Failed to allocate memory for program
```

**原因**: 物理内存不足

**解决**: 检查程序大小是否超过 MAX_FILE_SIZE (1MB)

## 下一步工作

1. **ELF 支持**: 实现 ELF 文件格式加载器
2. **参数传递**: 实现 argv 和 envp 传递
3. **独立地址空间**: 为每个进程创建独立的地址空间
4. **进程替换**: 实现真正的进程替换语义
5. **资源清理**: 进程退出时释放加载的程序内存

## 相关文档

- [系统调用实现](SYSCALL_IMPLEMENTATION.md)
- [任务管理](../docs/TASK_MANAGEMENT.md)
- [内存管理](../docs/MEMORY_MANAGEMENT.md)

---
**更新时间**: 2026-05-03
**版本**: 1.0
