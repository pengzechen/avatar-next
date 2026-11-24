# kernel/linux/fs — 文件 / 设备 syscall 入口

这里只放 SysV/Linux 文件类 syscall 的入口、参数翻译和它们直接依赖的内核对象。
路径 + 用户指针校验、fd 表、字符设备实现都按职责分在下列子目录：

| 目录 | 内容 | 主要导出 |
|---|---|---|
| `fd/` | 全局 fd 对象池（`fd_obj_t`、`FDT_*` 类型、fd 分配/回收） | `linux/fs/fd/fd_pool.h` |
| `path/` | 路径解析、`copy_*_user_*`、`user_range_accessible` | `linux/fs/path/path.h` |
| `file/` | `openat/read/write/lseek/stat/getdents64/...` 的 handler | `*_handler()`（声明在 `syscall_internal.h`） |
| `char/` | pipe、tty、pty 三个字符设备实现 | `linux/fs/char/{pipe,pty,tty}.h` |
| `process/` | 进程级 `files_struct` / `fs_struct` 静态池与引用计数 | `include/fs/process_fs.h` |
| `ioctl.c` | 跨 file / pseudo / dma-buf / tty 的 ioctl 派发 | `ioctl_handler()` |

约定：

- 只有 `ioctl.c` 留在本层顶层，因为它同时要处理 VFS 设备、dma-buf fd 和全局
  tty 三条路径，放进 `char/` 会误导读者。
- `file/` 下的 handler 签名统一是 `(uint64_t regs[6], task_t *)`，结果写回
  `regs[0]`；`path/` 和 `fd/` 是它们共享的底层设施。
- 新增文件类 syscall 时：handler 放 `file/`（目录类放 `file/dir.c`），需要
  用户指针时只走 `path/path.h` 的 `copy_*`，不要裸解引用。
- 新增字符设备时放 `char/`，并同步 `fs/vfs/vfs.c` 的设备创建路径。
