# LTP 测例管理指南

> **自动化入口**（推荐）：`make -C tests ltp-smoke` / `ltp-mm` / `ltp-full`。
> `run_ltp.sh` 现在每个测例输出一行 `LTP-CASE: <name> PASS|FAIL|SKIP|BROK`，
> 框架据此做逐例判定，并用 `tests/ltp/expected_failures.txt` 管理已知失败。
> 子集通过 `LTP_TESTS="..."`（或 `/tmp/ltp.list`）传给脚本。
>
> **全新 clone 先跑 `make env`**：LTP 源码（434 MB）不入库，会拉到
> `tests/ltp/src/`。`make -C tests ltp-build` 会自动先跑 env。

如何为 Avatar OS 添加、编译和运行 LTP 测例。

## 目录结构

```
tests/ltp/
├── build.sh          # 交叉编译脚本
├── fetch.sh          # make env：拉取 LTP 源码到 src/
├── run_ltp.sh        # 目标板运行脚本（会被复制到 rootfs）
├── testcases.list    # 选定的测例清单
├── bin/<arch>/       # 编译产物（git ignored）
└── src/              # LTP 源码（make env 拉取；git ignored）
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

浏览 `tests/ltp/src/testcases/kernel/syscalls/` 下的目录，
找到要测试的 syscall。每个目录下通常有多个编号的测例（01、02、03...），
难度和覆盖范围递增。

选择原则：
- 优先选 `01` 号（最基础，依赖最少）
- 检查源码确认不依赖未实现的 syscall（如 `epoll`、`inotify` 等）
- 避免需要 root 权限或特殊设备的测例

```bash
# 查看某个测例的源码
cat tests/ltp/src/testcases/kernel/syscalls/readv/readv01.c

# 快速检查依赖的 syscall
grep -h 'syscall\|SYS_' tests/ltp/src/testcases/kernel/syscalls/readv/readv01.c
```

### 2. 编辑 testcases.list

在 `tests/ltp/testcases.list` 中添加一行，格式为 `<目录名>/<二进制名>`：

```
# 添加 readv 测例
readv/readv01
```

二进制名通常与目录名加编号一致，但不总是。确认方法：

```bash
ls tests/ltp/src/testcases/kernel/syscalls/readv/
# 看 Makefile 或 .c 文件名确认最终二进制名
```

### 3. 拉源码并编译

```bash
# 首次：拉 LTP 源码到 tests/ltp/src（默认固定 20260529）
make env

# 编译指定架构（默认 riscv64）
bash tests/ltp/build.sh riscv64
bash tests/ltp/build.sh aarch64
bash tests/ltp/build.sh x86_64

# 或一条命令（ltp-build 会自动先跑 env）：
make -C tests ltp-build ARCHES='aarch64 riscv64 x86_64'
```

脚本会：
1. 自动检测架构变更并重新 configure
2. 编译 libltp 库
3. 逐个编译 testcases.list 中的测例
4. 将二进制复制到 `tests/ltp/bin/<arch>/`

如果某个测例编译失败，脚本会报错但继续编译其他测例。

> ⚠️ **每个架构要单独编，且产物不入库。**
> `tests/ltp/bin/<arch>/` 是 gitignore 的构建产物，各架构互不相干。
> 改过 `testcases.list` 之后，**三个架构都要重新跑一遍 `build.sh`**，
> 否则没重编的那个架构会**静默地只装旧的那批测例** —— 表现为
> `run_ltp.sh` 汇总出的测例数少于 `testcases.list` 的行数
> （曾出现 aarch64 有 35 个、riscv64/x86_64 只有 25 个）。
>
> 另外 `build.sh` 开头会 `rm -f "$OUT_DIR"/*`：中途失败会把这个架构的产物
> 清空或留半套。编完顺手核对一下数量：
>
> ```bash
> for a in aarch64 riscv64 x86_64; do
>     echo "$a: $(ls tests/ltp/bin/$a/ | wc -l)  # 应为 testcases.list 行数 + 1(run_ltp.sh)"
> done
> ```

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
grep HAVE tests/ltp/src/include/config.h
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
grep -E 'SYS_|__NR_' tests/ltp/src/testcases/kernel/syscalls/xxx/xxx01.c

# x86_64 syscall 号查询
grep -w 'xxx' /usr/include/asm/unistd_64.h  # 或查 Linux 源码
```

参考已有映射表（`syscall.c` 的 `x86_translate_syscall` 函数）。

## 当前测例清单（79 个，2026-10）

三架构（x86_64 / aarch64 / riscv64）均实测通过；x86_64 SMP=1 与 SMP=4 也各跑过一轮。

| 分类 | 数量 | 测例 |
|------|------|------|
| 进程生命周期 / 身份 | 23 | getpid01/02, getppid01/02, getuid01, getgid01, geteuid01, getegid02, getpgid01/02, getsid01/02, setsid01, set_tid_address01, gettid02, setpgid01, exit01/02, exit_group01, fork01, wait401/402, clone06 |
| 文件 I/O | 29 | open01/03, close01, read01, write01/02, readv01, writev01/02, pread01/02, pwrite01/02, lseek01, ftruncate03, fsync02, fdatasync01, dup01/02, dup3_01, fcntl01~04, pipe01, getcwd01, chdir02, fstat03, faccessat01 |
| 信号 | 7 | kill03/06/08, tkill01, rt_sigaction01, sigaction02, rt_sigprocmask01 |
| 内存 | 6 | brk01/02, mmap01/02, munmap01/03 |
| I/O 多路复用 | 9 | poll01/02, pselect01, epoll_create1_01/02, epoll_ctl01/03, epoll_wait01/04 |
| 系统信息 | 4 | uname01, clock_gettime01, getrusage01, getrandom02 |
| 调度 | 1 | sched_yield01 |

## 覆盖率（2026-10 统计）

- **79 个用例覆盖 55 个 syscall 目录**（按 LTP 目录计）。
- `kernel/syscall/syscall.c` 分发 **104 个 Linux syscall 入口**；扣除明确返回
  `ENOSYS` / 空实现的 13 个（waitid、mprotect、madvise、umask、getrlimit、
  setrlimit、prlimit64、set_robust_list、prctl、getgroups、setgroups、
  nanosleep、socketpair），约 **91 个有实际实现**。
- 因此已有 syscall 的**直接专测覆盖率约 55/91 ≈ 60%**。几乎所有用例还会
  间接调用 openat/read/write/getpid/mmap 等，实际触达面高于这个数字。
- 尚未直接覆盖的大块：socket 簇 15 个（ksocket 已实现，但没有 LTP 用例）、
  ioctl/pty、futex、sendfile、readlinkat、getdents64、renameat/unlinkat 的
  完整语义、set*id 权限模型、prctl、nanosleep 精度等。

**挑选原则与"试过但过不了"的逐条记录**（含 ioctl/waitid 两处内核崩溃）
全部在 `tests/ltp/testcases.list` 的文件头注释里 —— 往清单里加测例前**先看一眼**，
避免重复试那些已知需要补功能才能过的。

> 注意：LTP 对错误路径与边界语义要求很高，而本项目多数 syscall 实现是
> "够 busybox 用"。因此 LTP 失败**未必**代表功能缺失，很多是兼容性问题；
> 而项目定位不是完全兼容 Linux，所以失败项要按"值不值得修"逐个判断，
> 不要默认全修。

当前剩余阻塞项（详见 `testcases.list`）：

- **环境依赖**：rename01/03/04、getdents02、fsync01 需要 LTP 块设备 + mkfs；
  unlink05 需要 mkfifo；getcpu01 需要 sched_getaffinity；
  sysinfo01/gettid01 需要 `/proc/loadavg`、`/proc/self/status`。
- **依赖未实现 syscall**：open02 依赖 seteuid；mkdir02/04 依赖 setregid/setreuid；
  execve01 依赖 MAP_SHARED 文件映射一致性（LTP_IPC_PATH bad magic）。
- **内核语义缺口**：PROT_NONE 坏地址（`tst_get_bad_addr`）未返回 EFAULT；
  信号队列满时的 EAGAIN（tgkill02）；unlink07/08 的空路径 / 权限 /
  ENAMETOOLONG 语义。

## 本轮（2026-10）修掉的既有 bug

这些都不是"为了过测例而加功能"，而是既有实现的错误；测例留作回归网：

- x86_64 raw syscall 映射：17/18（pread64/pwrite64）、32（dup）、
  200（tkill，原先与内部 BIND 撞号）、280（utimensat）。
- `clock_gettime` 裸写用户指针：x86 上未映射的 setreuid(113) 会落到内部
  clock_gettime(113)，把 uid 当 timespec 指针写 → CR2=0x2 内核 #PF。
- pread/pwrite：用户缓冲未校验、NULL buf 语义、fd 类型/访问模式错误码、
  ext4 越过 EOF 写入（lwext4 的 fseek 拒绝 offset > fsize）。
- `open(O_CREAT)` 忽略 mode（open01 的 sticky bit）；`ftruncate` 错误码；
  `execve` 完全忽略 envp。
- **aarch64 open 标志 ABI**：aarch64 的 `O_DIRECTORY` 是 `040000`（x86/riscv
  是 `0200000`），内核原先不翻译，导致 `open(file, O_DIRECTORY)` 被当成
  普通 open 成功返回 —— LTP 清理 tmpdir 时 `fdopendir` 才报 ENOTDIR，
  整个测例以 TWARN(exit=4) 计为失败。现在 syscall 入口做双向翻译。
- `fstat` / `newfstatat` / `getresuid` / `getresgid` / `getrandom` 仍在裸写
  用户指针；已改为先填内核缓冲再 `copy_to_user_bytes`，未映射地址返回
  EFAULT 而不是内核态 #PF。
- **SMP 下任务退出栈被提前回收**：`task_exit()` 在切出 CPU 之前就唤醒父进程，
  父进程 `wait4` 立即回收槽位、下一次 `fork` 把仍在使用的内核栈整块刷成
  `TASK_STACK_MAGIC(0xA5)` → `sched_dequeue` 里 call 到野地址、整机挂死。
  LTP `getpid01`（连续 fork/wait 100 次）在 x86_64 SMP=4 下必中，SMP=1 正常。
  修复：新增 `task_t::switch_done`，由 `arch_task_switch` 在真正切到 next
  的栈/页表后置位；wait4 的回收路径等待它再销毁 pgd/复用栈槽/TCB。
  详见 `docs/bugfix/SMP_TASK_EXIT_STACK_REUSE.md`。

## 回归命令

```sh
# 三架构 SMP=1
make -C tests ltp-full ARCHES='aarch64 riscv64 x86_64'

# x86_64 SMP=4（helper/多核路径另见 docs/vmm/X86_GUEST_LINUX.md §12）
make -C tests ltp-full ARCHES=x86_64 SMP=4
```

`tests/ltp/expected_failures.txt` 目前为空：79 个用例全部 PASS。
