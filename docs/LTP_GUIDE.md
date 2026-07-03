# LTP 测例管理指南

如何为 Avatar OS 添加、编译和运行 LTP 测例。

## 目录结构

```
tests/ltp/
├── build.sh          # 交叉编译脚本
├── run_ltp.sh        # 目标板运行脚本（会被复制到 rootfs）
├── testcases.list    # 选定的测例清单
└── bin/<arch>/       # 编译产物（git ignored）

third_party/ltp/      # LTP 源码（release tarball 解压）
└── testcases/kernel/syscalls/
    ├── read/
    │   ├── read01.c
    │   ├── read02.c
    │   └── ...
    ├── write/
    ├── fork/
    └── ...           # ~300 个 syscall 目录
```

## 添加新测例

### 1. 选择测例

浏览 `third_party/ltp/testcases/kernel/syscalls/` 下的目录，
找到要测试的 syscall。每个目录下通常有多个编号的测例（01、02、03...），
难度和覆盖范围递增。

选择原则：
- 优先选 `01` 号（最基础，依赖最少）
- 检查源码确认不依赖未实现的 syscall（如 `epoll`、`inotify` 等）
- 避免需要 root 权限或特殊设备的测例

```bash
# 查看某个测例的源码
cat third_party/ltp/testcases/kernel/syscalls/readv/readv01.c

# 快速检查依赖的 syscall
grep -h 'syscall\|SYS_' third_party/ltp/testcases/kernel/syscalls/readv/readv01.c
```

### 2. 编辑 testcases.list

在 `tests/ltp/testcases.list` 中添加一行，格式为 `<目录名>/<二进制名>`：

```
# 添加 readv 测例
readv/readv01
```

二进制名通常与目录名加编号一致，但不总是。确认方法：

```bash
ls third_party/ltp/testcases/kernel/syscalls/readv/
# 看 Makefile 或 .c 文件名确认最终二进制名
```

### 3. 编译

```bash
# 编译指定架构（默认 riscv64）
bash tests/ltp/build.sh riscv64
bash tests/ltp/build.sh aarch64
bash tests/ltp/build.sh x86_64
```

脚本会：
1. 自动检测架构变更并重新 configure
2. 编译 libltp 库
3. 逐个编译 testcases.list 中的测例
4. 将二进制复制到 `tests/ltp/bin/<arch>/`

如果某个测例编译失败，脚本会报错但继续编译其他测例。

### 4. 运行

```bash
# 编译内核 + rootfs + 启动 QEMU
make ARCH=x86_64 test-ltp LOG=warn

# 在 QEMU shell 中执行
/ltp/run_ltp.sh
```

或者手动运行单个测例：

```bash
/ltp/readv01
```

## 编译失败排查

常见原因：

**缺少 config 宏**: 某些测例检查 `HAVE_xxx` 宏。
LTP configure 已处理大部分，但偶尔有遗漏。

```bash
# 查看 config.h 中已定义的宏
grep HAVE third_party/ltp/include/config.h
```

**依赖其他 LTP 库**: 部分测例依赖 `lib/` 之外的库
（如 `libs/libltpnuma`）。当前只编译核心 `lib/`，
需要时手动添加。

**内联汇编不兼容**: 极少数测例含 x86 专属内联汇编，
在交叉编译 aarch64/riscv64 时会失败。跳过即可。

## 测例运行失败排查

### TCONF (exit 32) — 跳过

测例检测到环境不满足条件（如缺少某功能），主动跳过。通常无需处理。

### TBROK (exit 2) — 环境异常

测例的 setup 阶段失败。常见原因：
- 依赖的 syscall 未实现（`Unknown syscall: N`）
- 目录创建失败（`mkdir` 未正确映射）
- 文件操作失败（`/proc` 不存在等）

**调试**:
```bash
# 加 LOG=debug 编译内核查看 syscall 日志
make ARCH=x86_64 kernel LOG=debug
```

### TFAIL (exit 1) — 测试失败

syscall 行为与 Linux 不一致。需要对照 man page 和 Linux 源码修复内核实现。

## x86_64 注意事项

x86_64 使用旧式 syscall 号（与 aarch64/riscv64 不同），
需要在 `kernel/syscall/syscall.c` 的 `x86_translate_syscall()` 中
添加映射。新增 syscall 时检查：

```bash
# 查看测例用了哪些 syscall
grep -E 'SYS_|__NR_' third_party/ltp/testcases/kernel/syscalls/xxx/xxx01.c

# x86_64 syscall 号查询
grep -w 'xxx' /usr/include/asm/unistd_64.h  # 或查 Linux 源码
```

参考已有映射表（`syscall.c` 的 `x86_translate_syscall` 函数）。

## 当前测例清单

| 分类 | 测例 | 说明 |
|------|------|------|
| 进程 | getpid02, getppid01, exit02, fork01, wait401, clone06 | 进程生命周期 |
| 文件 | write01, read01, close01, dup3_01, pipe01, lseek01 | 基础文件 I/O |
| 内存 | brk01, mmap01, munmap01 | 内存管理 |
| 信号 | kill03, rt_sigaction01 | 信号投递与处理 |
| 系统 | uname01, clock_gettime01 | 系统信息 |
| 调度 | sched_yield01 | CPU 让出 |

共 20 个测例，覆盖五大类。

## 推荐下一批测例

以下是已实现 syscall 中尚未覆盖的测例，可按需添加：

```
# 文件操作扩展
readv/readv01
writev/writev01
ftruncate/ftruncate01
fcntl/fcntl01
getcwd/getcwd01
chdir/chdir01

# 进程扩展
getuid/getuid01
getgid/getgid01
setpgid/setpgid01
setsid/setsid01

# 信号扩展
rt_sigprocmask/rt_sigprocmask01

# 内存扩展
mprotect/mprotect01
```
