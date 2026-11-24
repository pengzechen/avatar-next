# lib — freestanding 库

```
lib/
├── libc/       freestanding C 库实现（string / vsnprintf / atoi / qsort）
├── klog.c      内核日志输出，不是 libc
└── bitmap.c    内核位图，不是 libc
```

`libc/` 里的对象由 `mk/sources.mk` 的 `LIBC_SOURCES` / `STRING_SOURCES` /
`VSNPRINTF_SOURCES` 引用；`klog.c`、`bitmap.c` 是内核工具，不要混进 libc。
