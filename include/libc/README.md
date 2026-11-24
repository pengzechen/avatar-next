# include/libc — freestanding C 环境

这个目录是内核和第三方包的**无标准库头文件层**（`-nostdinc -Iinclude/libc`），
不是完整 libc。按来源分三类：

## 真正的 freestanding C 头

- `stdint.h` / `stddef.h` / `stdarg.h` / `stdbool.h` / `limits.h` / `inttypes.h`
- `string.h` + `string_internal.h`（inline API + 架构实现层）
- `stdio.h` / `stdlib.h`（`my_vsnprintf` / `atoi` / `qsort` 的声明）

## 内核/标准名 shim（历史遗留，后续应归位）

- `assert.h`：内核断言（`platform_panic`），但 lwext4 也会 `#include <assert.h>`；
- `signal.h`：现在只是转发到 `include/linux/signal.h`，给 lwIP PPP 等仍写
  `<signal.h>` 的第三方代码用；内核代码直接 include `"linux/signal.h"`。

`assert.h` 下一步也应拆成 `include/kernel/assert.h` + 薄转发。

## 已删除

- `unistd.h`：它是历史 `SYS_*` ABI 的重复声明，和
  `kernel/linux/syscall.h` 重复；当前内核/第三方没有实际使用。若将来启用
  lwIP PPP 等代码，应在对应 `_port/` 下提供包级 `unistd.h`，不要放回这里。

实现位置见 `lib/libc/`。
