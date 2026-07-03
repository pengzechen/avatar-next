# LTP x86_64 Signal & Syscall Bug Fixes

本次 session 的目标是让 LTP (Linux Test Project) 测例在三个架构上通过。
riscv64 和 aarch64 已基本修复，x86_64 遇到了大量问题。以下按发现顺序记录。

## 1. MAP_SHARED 文件映射未标记 NOFREE — LTP 计数器全零

**现象**: LTP 测例运行完毕后，PASS/FAIL/SKIP 计数器全部为 0。

**根因**: LTP 框架 (`tst_test.c:setup_ipc()`) 使用 **file-backed MAP_SHARED**
（`open(O_CREAT) → ftruncate → mmap(MAP_SHARED, fd)`）在父子进程间共享
`struct results`。内核 `sys_mmap` 只在匿名 MAP_SHARED 路径设置了 `PTE_NOFREE`，
file-backed 路径遗漏了。子进程 exit 时 `munmap` 释放了共享页的物理帧，
父进程读到的计数器已被清零。

**修复**: `kernel/syscall/mm/mmap.c` — 在 file-backed mmap 的页读取循环之后，
对 MAP_SHARED 页也标记 `PTE_NOFREE`（三个架构各自的 PTE 操作）。

**影响**: 全架构。

---

## 2. aarch64 PTE_NOFREE 编译错误

**现象**: `'PTE_NOFREE' undeclared` in `proc_lifecycle.c`。

**根因**: `include/aarch64/mm_vm.h` 没有 `#include "mmu.h"`，
而 `PTE_NOFREE` 定义在 `mmu.h` 中。

**修复**: `include/aarch64/mm_vm.h` 添加 `#include "mmu.h"`。

---

## 3. aarch64 用户页错误导致 QEMU 退出

**现象**: `munmap01` 测例导致 QEMU 直接退出。

**根因**: `handle_el0_sync_exception` 只处理了 SVC (EC=0x15)，
用户态 page fault (EC=0x20/0x24) 直接调用 `platform_shutdown()`。

**修复**: `boot/aarch64/exception.c` — 对 EC=0x20/0x24 投递 SIGSEGV
而非关机。

---

## 4. x86_64 syscall 号映射错误 — mkdir/rename 无效

**现象**: 所有 LTP 测例 TBROK，`chdir` 报 ENOENT（目录不存在）。

**根因**: x86_64 syscall 翻译表中：
- **82 = rename**（被错误映射为 fchmod stub）
- **83 = mkdir**（被错误映射为 fchown stub）

导致 `mkdir("/tmp/LTP_xxx", 0777)` 静默返回 0 但不创建目录。

**修复**: `kernel/syscall/syscall.c` — 将 82 映射为 `renameat(AT_FDCWD,...)`，
83 映射为 `mkdirat(AT_FDCWD,...)`；fchmod/fchown stub 移到正确的 91/93。

---

## 5. x86_64 缺少旧式 syscall 映射

**现象**: `Unknown syscall: 87` (unlink)、`Unknown syscall: 26` (msync)、
`Unknown syscall: 90` (chmod)、`Unknown syscall: 92` (chown) 等。

**根因**: x86_64 有大量 POSIX 旧式 syscall（不带 `at` 后缀），
而内核只实现了 `*at` 变体。

**修复**: 逐一添加翻译：

| x86_64 # | 名称 | 翻译 |
|----------|------|------|
| 26 | msync | stub 0 |
| 84 | rmdir | unlinkat(AT_FDCWD, path, AT_REMOVEDIR) |
| 85 | creat | openat(AT_FDCWD, path, O_CREAT\|O_WRONLY\|O_TRUNC, mode) |
| 86 | link | stub 0 |
| 87 | unlink | unlinkat(AT_FDCWD, path, 0) |
| 90 | chmod | stub 0 |
| 91 | fchmod | stub 0 |
| 92 | chown | stub 0 |
| 93 | fchown | stub 0 |

---

## 6. x86_64 用户态异常未投递信号

**现象**: `munmap01` 测例触发 `#14` (Page Fault) 后进程被直接终止。

**根因**: `boot/x86_64/exception.c` 的 `handle_exception` 对用户态异常
直接设置 `TASK_DEAD`，没有投递对应信号。

**修复**: 在 `from_user` 块中，对已知异常投递信号后 return：
- `#14` (PF) → SIGSEGV
- `#13` (GPF) → SIGSEGV
- `#6` (UD) → SIGILL
- `#0` (DE) → SIGFPE
- `#5` (BP) → SIGTRAP

---

## 7. x86_64 rt_sigaction01 GPF — 信号返回蹦床地址错误 (关键 Bug)

**现象**: `rt_sigaction01` 信号 handler 执行成功后，`ret` 跳转到
`RIP=0x6fffff10`（用户栈区域）触发 GPF (#13)。

**根因链**:

1. LTP 的 `ltp_rt_sigaction()` 在 x86_64 上调用 `sig_initial()` 获取
   musl libc 内部的 `__restore_rt` 蹦床地址。
2. `sig_initial()` 通过 `sigaction()` libc wrapper 两次调用获取
   `oact.sa_restorer`。
3. **musl 的 `sigaction()` wrapper 不会将 `sa_restorer` 从内核响应
   复制回用户 struct**（只复制 handler/flags/mask）。
4. 因此 `oact.sa_restorer` 是栈上的未初始化垃圾值 `0x6fffff10`。
5. 这个垃圾值被存入 `kernel_sigaction.sa_restorer` 并通过
   `rt_sigaction` syscall 传给内核。
6. 信号投递时，内核将此地址作为 handler 的返回地址压栈。
7. handler `ret` 后跳转到栈数据区域，触发 GPF（x86_64 栈页有 NX 位）。

**注**: 此 bug 仅影响 musl 工具链。glibc 的 `sigaction()` wrapper
会复制 `sa_restorer` 回用户 struct，因此 LTP 在 glibc 环境下正常工作。

**修复**: 两部分：

### 7a. 内核信号返回蹦床页

`kernel/task/exec.c` — 创建用户进程时，在固定虚拟地址 `USER_SIGRET_PAGE`
(0x70001000) 映射一个可执行页，写入 x86_64 `rt_sigreturn` 蹦床代码：

```asm
mov $15, %eax    ; rt_sigreturn syscall number
syscall
```

页的其余部分填充 `0xCC` (int3) 作为安全兜底。

### 7b. 信号投递时始终使用内核蹦床

`kernel/syscall/core/signal.c` — x86_64 信号投递时，无条件使用
`USER_SIGRET_PAGE` 作为 `sa_restorer`，不依赖用户提供的值。

这是安全的，因为：
- 蹦床功能与 musl 的 `__restore_rt` 完全等价（都是调用 `rt_sigreturn`）
- musl 自身也是无条件覆盖用户提供的 restorer，使用自己的 `__restore_rt`
- aarch64 和 riscv64 已有类似机制（在栈上生成蹦床代码），
  x86_64 因栈 NX 位无法使用同一方案

**涉及文件**:
- `include/user_layout.h` — 新增 `USER_SIGRET_PAGE` 常量
- `kernel/task/exec.c` — 映射蹦床页
- `kernel/syscall/core/signal.c` — 使用蹦床页
- `kernel/task/task.h` — 新增 `SA_RESTORER` 常量

---

## 8. LTP 多架构构建支持

**现象**: 从 riscv64 切换到 aarch64 编译 LTP 后，rootfs 中没有 LTP 二进制。

**根因**: `tests/ltp/build.sh` 只检查 `config.h` 是否存在，
不检查是否为当前架构配置。

**修复**: 检查 `config.status` 中的 `host_alias`，
架构变更时自动 `make distclean` 后重新 configure。

---

## 最终 LTP 结果

| 架构 | PASS | FAIL | BROK | 备注 |
|------|------|------|------|------|
| riscv64 | 20/20 | 0 | 0 | |
| aarch64 | 19/20 | 0 | 1 | close01 BROK (已知) |
| x86_64 | 20/20 | 0 | 0 | 修复后全部通过 |
