# 系统调用实现现状

**统计日期**: 2026-11-10
**统计口径**: `kernel/syscall/syscall.c` 主 `switch` 里的 106 个具名
`LINUX_SYS_*` 入口。裸号兼容项（statfs / setitimer / affinity 等）和历史的
`SYS_*` 自定义 ABI 单独统计，不与这 106 个混在一起。

> **“完整”是相对本项目定位而言**：核心路径可用、常见参数和错误路径基本正确、
> 有 LTP 或 busybox 实际使用验证；**不等于**与 Linux 逐条语义一致。
> 例如几乎所有实现都不支持全部 flag、权限模型、siginfo、审计/namespace 等。

## 一、结论

| 状态 | 数量 | 占比 |
|------|------|------|
| ✅ 相对完整 | **38** | 36% |
| ⚠️ 不完整（能用，但缺选项/边界/错误语义） | **63** | 59% |
| ❌ 桩（ENOSYS 或静默返回成功） | **5** | 5% |
| **合计** | **106** | 100% |

5 个桩里 4 个明确返回 `ENOSYS`（`waitid`、`sendmsg`、`recvmsg`、
`socketpair`）；只剩 `madvise` 一个是**静默成功型桩**，调用方会以为
操作生效了。2026-11 的第四轮已经把原先 10 个静默桩（`mprotect`、
`nanosleep`、`getrlimit`/`setrlimit`/`prlimit64`、`umask`、`prctl`、
`set_robust_list`、`getgroups`/`setgroups`）改成真实实现。

## 二、按子系统拆分

| 子系统 | 总数 | 完整 | 不完整 | 桩 |
|--------|------|------|--------|----|
| 进程 / 身份 | 30 | 18 | 11 | 1 |
| 文件 / I/O | 30 | 6 | 24 | 0 |
| 内存 | 5 | 1 | 3 | 1 |
| 信号 / 事件 | 9 | 4 | 5 | 0 |
| 系统信息 / 时间 | 16 | 9 | 7 | 0 |
| 网络 | 16 | 0 | 13 | 3 |

文件 I/O 没有桩，但 24/30 不完整：主路径都能用，缺的是错误语义和边界
（目录读写、重复 close、`O_CLOEXEC`、dirfd 校验等）。网络同理：
AF_INET TCP/UDP 基本可用，但没有一条达到“完整”。

## 三、明细

### ✅ 相对完整（38）

- **进程 / 身份**：`exit`, `execve`, `wait4`, `gettid`, `getpid`, `getppid`,
  `getuid`, `geteuid`, `getgid`, `getegid`, `getresuid`, `getresgid`,
  `getpgid`, `getsid`, `setsid`, `sched_yield`, `getgroups`, `setgroups`
- **文件 / I/O**：`pread64`, `pwrite64`, `lseek`, `dup`, `fstat`, `getcwd`
- **内存**：`brk`
- **信号 / 事件**：`rt_sigprocmask`, `rt_sigpending`, `rt_sigreturn`,
  `epoll_create1`
- **系统信息 / 时间**：`getcpu`, `getrlimit`, `setrlimit`, `prlimit64`,
  `umask`, `set_robust_list`, `nanosleep`, `times`, `personality`

### ⚠️ 不完整（61）

**进程 / 身份（11）**

| syscall | 主要缺口 |
|---|---|
| `clone` | fork 和 pthread 线程可用；vfork 退化成 fork，未实现的 flag 被忽略 |
| `exit_group` | 直接给兄弟线程置 DEAD；线程仍在别的核上跑时存在竞态（见下方“已知风险”） |
| `set_tid_addr` | 清 CTID 时裸写用户指针，坏地址可导致内核 #PF |
| `setuid` / `setgid` / `setresuid` / `setresgid` | 直接改 ID；无权限检查、无 saved-set 语义 |
| `setpgid` | 不校验 pgid 是否存在/同会话（`setpgid02` 失败） |
| `kill` / `tkill` / `tgkill` | 无权限检查；`kill(-1)` 只广播当前 pgid；信号队列满不返回 EAGAIN（`tgkill02` 失败） |

**文件 / I/O（24）**

| syscall | 主要缺口 |
|---|---|
| `openat` | dirfd 非目录已改返 `ENOTDIR`（`openat01` 通过）；`O_NOFOLLOW`/`O_SYNC`/`O_TMPFILE` 等未实现 |
| `close` | 重复 close 返回 `EBADF`（`close02` 通过）；fd 0/1/2 视为隐式控制台，无法真正关闭 |
| `read` / `write` / `readv` / `writev` | 参数校验和部分写/目录语义不全（`read02`/`write03` 失败） |
| `ftruncate` | ext4 只支持缩小，扩大是空操作；FAT 才补零扩展 |
| `fsync` / `fdatasync` | ext4 上是空操作；FAT 真正 `f_sync` |
| `dup3` | flags 只接受 `O_CLOEXEC`，其余 `EINVAL`；同 fd 返回 `EINVAL`（`dup3_02` 通过） |
| `fcntl` | 只支持 `F_GETFD/F_SETFD/F_GETFL/F_SETFL/F_DUPFD/F_DUPFD_CLOEXEC`；锁相关命令走 default 且返回 0 |
| `pipe2` | `O_NONBLOCK` 生效并反映到 `F_GETFL`，`O_CLOEXEC` 落到 fd 位图（`pipe2_01/02` 通过）；`O_DIRECT` 接受但不实现 packet 模式 |
| `getdents64` | 基本目录遍历可用；d_type/d_off、边界与错误语义未完全对齐 |
| `newfstatat` | 基本 stat 可用；dirfd/坏地址错误码不精确 |
| `readlinkat` | 仅 `/proc/self/fd/N` + 文件系统 readlink；无 `AT_SYMLINK_NOFOLLOW` 等语义 |
| `faccessat` / `faccessat2` | dirfd 非目录改返 `ENOTDIR`（`faccessat02` 通过）；仍忽略 mode/flags，无权限模型 |
| `mkdirat` | dirfd/umask 已生效；权限位仍未落地 |
| `unlinkat` | 仅支持 `AT_REMOVEDIR`；空路径/ENAMETOOLONG/权限等语义缺失 |
| `renameat` / `renameat2` | 同挂载内可用，跨挂载 `EXDEV`；`renameat2` 仅接受 flags=0 |
| `chdir` | 普通文件返回 `ENOTDIR`；路径改为流式规范化（`chdir04` 通过），cwd 缓冲仍限 `TASK_CWD_LEN` |
| `sendfile` | in_fd 必须是普通文件（否则 `EINVAL`）；out_fd 按 fd 对象分发到文件/管道/控制台；offset 经用户指针拷贝。文件位置语义未逐条对齐（未复测 `sendfile02`） |
| `ioctl` | 只覆盖 TTY（TCGETS/TCSETS/TIOCGWINSZ/TIOCGPGRP…）、FIONBIO、dma-buf；其余 `ENOTTY` |

**内存（3）**：`mmap`（`prot` 完全忽略；文件映射是立即拷贝，`MAP_SHARED` 不回写；
`MAP_FIXED` 不先 unmap 旧映射）；`munmap`（要求 addr 页对齐，边界语义简化）；
`mprotect`（PROT_NONE 只保留 VA、不建 PTE；升级为可读写时补零页，降为 PROT_NONE
时 unmap。但页表项没有真正的 RO/RX 权限位，`mmap` 的 `prot` 仍被忽略）。

**信号 / 事件（5）**：`rt_sigaction`（无 siginfo / `SA_SIGINFO`，且 struct 直接
裸写用户指针）、`ppoll`/`pselect6`（sigmask 忽略、事件位有限）、
`epoll_ctl`（重复 ADD/非法 fd 错误码不全）、`epoll_pwait`（同上，且依赖 socketpair
的用例跑不了）。

**系统信息 / 时间（7）**：`clock_gettime`（忽略 clockid，全部返回 realtime）、
`getrusage`（who 参数已校验、utime/stime 实时；但只填自身）、
`uname`（坏地址已返回 EFAULT，`uname02` 通过）、
`sysinfo`（多数字段为 0/估算）、`getrandom`（LCG，忽略 flags 与阻塞语义）、
`futex`（仅 `FUTEX_WAIT`/`FUTEX_WAKE`）、
`prctl`（PDEATHSIG/DUMPABLE/NAME/NO_NEW_PRIVS 已实现；未实现选项返回 EINVAL）。

**网络（13）**：`socket`, `bind`, `listen`, `accept`, `accept4`, `connect`,
`getsockname`, `getpeername`, `sendto`, `recvfrom`, `setsockopt`, `getsockopt`,
`shutdown`。全部只支持 AF_INET + TCP/UDP；sockaddr/缓冲区多数仍是裸用户指针；
`socket()` 不处理 `SOCK_CLOEXEC`；`accept4()` 只处理 `SOCK_NONBLOCK`。

### ❌ 桩（5）

| syscall | 当前行为 |
|---|---|
| `waitid` | `ENOSYS` |
| `sendmsg` / `recvmsg` / `socketpair` | `ENOSYS` |
| `madvise` | 直接返回 0（建议不生效） |

## 四、裸号兼容项（不计入上面的 106）

为了兼容 musl/busybox，`syscall.c` 还直接处理了一批原始 syscall 号。
按逻辑 syscall 计约 20 个（`times`、`personality` 第四轮已提升为正式入口，
不再算裸号兼容项）：

- **部分实现（2）**：`setitimer` / `getitimer`（仅 `ITIMER_REAL`，其余
  `EINVAL`；已能当 LTP 看门狗用）。
- **桩 / 伪实现（16）**：`sync`, `membarrier`, `chmod`, `fchmod`, `chown`,
  `fchown`, `fchmodat`, `fchownat`, `utimensat`, `msync`, `sched_setaffinity`,
  `sched_getaffinity`, `riscv_flush_icache`（以上返回 0）；`linkat`（返回
  `EPERM`）；`statfs` / `fstatfs`（硬编码 EXT4/1GB 假数据）。
- **明确未实现（2）**：`riscv_hwprobe`（`ENOSYS`）；`sigaltstack`
  （x86 映射到返回 0，其它架构落 `ENOSYS`）。

注意：`sched_setaffinity`/`sched_getaffinity` 的原始号分支把 **122/123 写反了**
（aarch64/riscv64 上 122=set、123=get），当前行为是
“set 去填 mask、get 直接返回 0”，属于待修 bug。

## 五、历史自定义 ABI（`SYS_*`，12 个）

Linux 用户程序不走这里，仅保留给早期裸机测试：

- 可用：`exit`, `yield`, `getpid`, `write`, `brk`, `execve`, `gettimeofday`
  （`sleep` 是忙等）。
- 玩具实现：`sbrk`（只改一个静态变量，不映射内存）。
- **未实现**：`open`, `close`, `read`（`read` 恒返回 0，open/close 返回 -1）。

新用户程序请直接使用 Linux ABI，不要基于这套号继续加接口。

## 六、已知风险（比“缺功能”更值得先处理）

1. **用户指针裸解引用**：`rt_sigaction`、网络 sockaddr/缓冲区、
   `set_tid_addr` 的 CTID 回写、`execve` 的路径复制等仍直接解引用来自
   `regs[]` 的地址。用户传未映射地址即可让内核态 #PF → 停机。
   规则见 `CLAUDE.md`「用户指针」一节。
2. **静默成功**：`chmod/chown/utimensat/msync/sync/...` 返回 0 但不做事，
   比 `ENOSYS` 更难排查。新代码不要模仿这种桩。
3. **`exit_group` 的兄弟线程**：和刚修掉的 `task_exit` 栈复用问题同源 ——
   它给别的线程置 DEAD 时，对方可能还在 CPU 上运行。多线程程序调用
   `exit_group` 才有风险，LTP 当前未覆盖。
4. **`mmap` 的 `prot` 仍被忽略**：`mprotect` 现在能处理 PROT_NONE 保留区和
   升级补页，但页表项没有 RO/RX 权限位，等于仍没有完整的页权限模型。

## 七、和 LTP 的关系

`tests/ltp/testcases.list` 当前 103 个用例、覆盖 66 个 syscall 目录，
三架构 SMP=1 全绿（SMP=4 只在上一轮 79 个用例时跑过）。覆盖的 66 个目录
绝大多数落在“完整/不完整”两档；桩基本没有专门用例（`waitid` 曾导致内核
崩溃，现在只保留“不崩”的记录）。清单里逐条记录了“试过但过不了”的原因，见
`tests/ltp/testcases.list` 和 `docs/tests/LTP_GUIDE.md`。

## 八、建议的优先级

1. **安全类**：把剩余用户指针全部改成 `copy_to/from_user_bytes`（网络、
   sigaction、CTID、execve path）。这类问题能让普通用户程序打挂整机。
2. **剩余的静默成功**：`madvise` 至少改成 `ENOSYS` 或真实现；裸号兼容项里的
   `chmod/chown/utimensat/msync/sync` 同理。
3. **文件错误语义**：目录 `read/write` 的 EISDIR、`faccessat` 的 mode、
   `sendfile` 的 offset/文件位置逐条对齐 —— 都是 LTP 现成能测的。
4. **时间**：`clock_gettime` 支持 MONOTONIC；`nanosleep` 已真睡眠但精度
   受 100Hz tick 限制。
5. **网络**：要再往上做，先补 `sendmsg/recvmsg/socketpair` 和用户指针拷贝；
   否则保持“仅 AF_INET”并在文档中明确。

---
**更新时间**: 2026-11-10
