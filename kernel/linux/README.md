# kernel/linux — Linux ABI personality（部分兼容）

这里放的是 **Linux ABI 的策略与入口**，不是通用内核机制：

- `syscall.c` / `syscall_trace.c`：Linux syscall 号空间、分发、strace 式追踪；
- `core/`：进程生命周期、信号、身份、rlimit 等 Linux/POSIX 语义；
- `fs/`、`io/`、`mm/`、`net/`：Linux 用户可见的 fd/path/pipe/tty/epoll/mmap/socket 接口；
- `exec/`：execve、ELF / shebang / PT_INTERP 加载。

通用机制不在这里：

- `kernel/task/`：TCB、调度、上下文切换、sleep/wake、completion、mutex；
- `kernel/mm/`：PMM、kmalloc、通用页表/地址空间；
- `kernel/time/`：tick、hrtimer、ITIMER 核心；timer/sleep 字段可被 RT 复用；
- `fs/vfs/`、`kernel/net/`：通用 VFS 与网络栈。

`include/syscall_abi.h` 和 `boot/<arch>/` 里的 syscall 入口是**架构机制**，
保留通用命名；它们只负责把寄存器现场交给本目录的 Linux 分发器。

本目录只实现当前需要的 Linux ABI 子集，不是完整 Linux；完整度分类见
`docs/SYSCALL_IMPLEMENTATION.md`。
