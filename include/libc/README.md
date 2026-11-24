# include/libc — freestanding C 环境

这个目录是内核和第三方包的**无标准库头文件层**（`-nostdinc -Iinclude/libc`），
不是完整 libc。按来源分三类：

## 真正的 freestanding C 头

- `stdint.h` / `stddef.h` / `stdarg.h` / `stdbool.h` / `limits.h` / `inttypes.h`
- `string.h` + `string_internal.h`（inline API + 架构实现层）
- `stdio.h` / `stdlib.h`（`my_vsnprintf` / `atoi` / `qsort` 的声明）

## 内核/标准名 shim（历史遗留，后续应归位）

- `assert.h`：内核断言（`platform_panic`），但 lwext4 也会 `#include <assert.h>`；
- `signal.h`：内核信号常量/`signal_struct`，lwIP PPP 也会 `#include <signal.h>`。

这两个文件保留在 `libc/` 是为了让第三方继续用标准名；内核代码应逐步改为
`kernel/assert.h` / `linux/signal.h`，`libc/` 下只留薄转发。

## 已删除

- `unistd.h`：它是历史 `SYS_*` ABI 的重复声明，和
  `kernel/linux/syscall.h` 重复；当前内核/第三方没有实际使用。若将来启用
  lwIP PPP 等代码，应在对应 `_port/` 下提供包级 `unistd.h`，不要放回这里。

实现位置见 `lib/libc/`。
