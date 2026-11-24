# lib/libc — freestanding C 实现

| 文件 | 内容 |
|---|---|
| `string.c` | `memcpy`/`memset`/`memmove` 的外部符号版本（inline 版在 `include/libc/string.h`） |
| `vsnprintf.c` | `my_vsnprintf()` 格式化核心 |
| `stdlib.c` | `atoi()` / `qsort()` |

`stdio.h` 里声明的 `printf` / `snprintf` 目前还没有对应的外部实现；内核代码
用的是 `kprintf()` / `my_vsnprintf()`。这是下一步要收口的地方之一。
