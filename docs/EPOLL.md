# epoll 子系统

## 概述

Avatar OS 的 epoll 实现提供 O(1) 事件驱动 I/O 通知，取代 poll/select 的忙轮询模型。

## 架构

```
┌─────────────┐     fd_notify_waiters()     ┌──────────────┐
│  pipe/socket │ ──────────────────────────▸ │  fd_waitqueue │
│  pty/...     │    状态变化时主动通知        │  (per fd_obj) │
└─────────────┘                             └──────┬───────┘
                                                   │ 唤醒
                                              ┌────▼────────┐
                                              │ epoll_instance│
                                              │  ready_count++│
                                              │  unblock task │
                                              └──────────────┘
```

### 三层设计

1. **fd_waitqueue** — 挂在每个 `fd_obj_t` 上的等待者链表，fd 状态变化时调用 `fd_notify_waiters()` 唤醒所有监听者
2. **epoll_instance** — 管理被监听 fd 集合、就绪列表、阻塞任务
3. **fd_poll** — 统一的 fd 状态查询接口，按 `fd_type_t` 分派到各子系统的 poll 函数

## 文件

| 文件 | 说明 |
|------|------|
| `kernel/syscall/io/epoll.h` | 数据结构、常量、API 声明 |
| `kernel/syscall/io/epoll.c` | 全部实现：实例管理 + syscall handler + fd_poll + fd_notify |

## Syscall 接口

| 功能 | aarch64/riscv64 | x86_64 |
|------|----------------|--------|
| `epoll_create1(flags)` | 20 | 291 |
| `epoll_ctl(epfd, op, fd, event)` | 21 | 233 |
| `epoll_pwait(epfd, events, max, timeout, sigmask, sigsetsize)` | 22 | 281 |

x86_64 额外兼容映射：
- 213 (`epoll_create`) → `epoll_create1(0)`
- 232 (`epoll_wait`) → `epoll_pwait(sigmask=NULL)`

## struct epoll_event ABI

```c
struct epoll_event {
    uint32_t events;
#if ARCH_X86_64
    uint64_t data;
} __attribute__((packed));   // x86_64: sizeof=12, 无 padding
#else
    uint64_t data;
};                           // aarch64/riscv64: sizeof=16, 自然对齐
#endif
```

Linux 内核只在 x86_64 上对 `epoll_event` 使用 `__attribute__((packed))`，其他架构使用自然对齐。这是一个已知的历史 ABI 差异。

## 事件通知注入点

| fd 类型 | 通知位置 | 事件 |
|---------|---------|------|
| pipe | `pipe_write` 写入后 | `EPOLLIN`（读端） |
| pipe | `pipe_read` 读出后 | `EPOLLOUT`（写端） |
| pipe | `pipe_close_write` | `EPOLLHUP`（读端） |
| pipe | `pipe_close_read` | `EPOLLERR`（写端） |
| socket | `ksock_recv_cb` / accept | `EPOLLIN` |
| socket | `ksock_sent_cb` | `EPOLLOUT` |
| pty | master/slave write | `EPOLLIN`（对端） |
| pty | close | `EPOLLHUP` |
| file/pseudo | 始终就绪 | — |

## poll/select 升级

`poll_handler` 和 `select_handler` 已改用 `fd_poll()` 统一接口 + `fd_waitqueue` 事件驱动唤醒，不再忙轮询。阻塞的 poll/select 通过临时注册到 fd_waitqueue 实现被动唤醒。

## fd_pool 改进

### FDT_ALLOCATED 状态

新增 `FDT_ALLOCATED` 中间态防止 TOCTOU 双重分配：

```
FDT_FREE → FDT_ALLOCATED → FDT_FILE / FDT_DIR / FDT_PIPE / ...
```

`fd_pool_alloc()` 立即将 slot 标记为 `FDT_ALLOCATED`，后续代码再设置具体类型。

### task_unblock 幂等性

`task_unblock()` 增加状态检查，非 `TASK_BLOCKED` 时直接返回，防止多次 unblock 导致调度队列损坏。

## 容量限制

| 参数 | 值 |
|------|-----|
| `EPOLL_MAX_INSTANCES` | 16 |
| `EPOLL_MAX_ITEMS` | 64 |
| `FD_WAIT_MAX`（每个 fd 最大监听者） | 4 |

## LTP 测试

通过的测试用例：
- `epoll_create1_01` / `epoll_create1_02`
- `epoll_ctl01`
- `epoll_wait01` / `epoll_wait04`

## 遗留问题

### openat(O_DIRECTORY) 对普通文件错误返回成功

**现象**: aarch64 上 mmap01 测试 cleanup 时报 TWARN：
```
tst_tmpdir.c:339: TWARN: tst_rmdir: rmobj(...) failed:
Cannot open directory stream for mmapfile (via fd 3); errno=20: ENOTDIR
```

**根因**: lwext4 的 `ext4_dir_open()` 不验证 inode 类型，对普通文件也会成功返回。当 `tst_rmdir` 对临时目录中的文件调用 `openat(fd, "mmapfile", O_DIRECTORY)` 时，内核错误地返回了成功。

**当前修复**: 在 `ext4_dir_open` 成功后，用 `ext4_mode_get()` 读取 inode mode 验证是否为目录类型（`S_IFDIR = 0040000`），非目录则返回 `-ENOTDIR`。

**影响范围**: 此 bug 存在于所有三个架构，但只有 aarch64 上的 mmap01 测试触发了该路径。x86_64 和 riscv64 上 mmap01 的 cleanup 代码路径未走到此分支。

**状态**: 修复已合入（`file_ops.c` 的 `openat_handler`），但尚未在三架构上全部验证编译和运行。
