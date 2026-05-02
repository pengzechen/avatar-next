# lwext4 集成文档

## 概述

[lwext4](https://github.com/gkostka/lwext4) 是一个轻量级 ext2/3/4 文件系统库，专为裸机和嵌入式环境设计。本文档描述如何在 Avatar OS 中使用它。

---

## 目录结构

```
fs/
├── compat/                   # 标准库兼容头（-nostdinc 裸机环境）
│   ├── stdint.h              # → include/types.h
│   ├── stddef.h              # → include/types.h + __builtin_offsetof
│   ├── stdbool.h             # → include/types.h
│   ├── stdarg.h              # → include/arg.h
│   ├── stdlib.h              # 空存根（malloc 由配置重定向）
│   └── inttypes.h            # PRIu64/PRId64 等格式化宏
├── lwext4/                   # 原始 lwext4 源码（不做任何修改）
└── lwext4_port/              # 裸机移植层（本项目代码）
    ├── generated/
    │   └── ext4_config.h     # 裸机配置文件
    ├── kmalloc.c             # 静态堆内存分配器
    └── libc_stub.c           # 缺失 libc 函数的实现（qsort）
```

---

## 移植配置（`lwext4_port/generated/ext4_config.h`）

| 配置项 | 值 | 说明 |
|---|---|---|
| `CONFIG_HAVE_OWN_ERRNO` | 1 | 使用 lwext4 内置错误码，不依赖 `<errno.h>` |
| `CONFIG_HAVE_OWN_OFLAGS` | 1 | 使用内置文件标志（`O_RDONLY` 等），不依赖 `<fcntl.h>` |
| `CONFIG_DEBUG_PRINTF` | 0 | 禁用调试输出，不依赖 `<stdio.h>` |
| `CONFIG_DEBUG_ASSERT` | 0 | 禁用 assert 输出，不依赖 `<stdio.h>` |
| `CONFIG_HAVE_OWN_ASSERT` | 1 | 使用空断言，不依赖 `<assert.h>` |
| `CONFIG_USE_USER_MALLOC` | 1 | `ext4_malloc/free` 重定向到 `ext4_user_*` 函数 |
| `CONFIG_BLOCK_DEV_ENABLE_STATS` | 0 | 关闭块设备统计（减少内存占用）|
| `CONFIG_JOURNALING_ENABLE` | 1 | 启用 journaling（ext3/4）|
| `CONFIG_EXTENTS_ENABLE` | 1 | 启用 extent 树（ext4）|

---

## 内存分配器（`lwext4_port/kmalloc.c`）

使用一个 **静态堆 + first-fit free-list** 分配器，不依赖操作系统的任何内存服务。

- **堆大小**：默认 512 KB，通过编译选项调整：
  ```makefile
  LWEXT4_CFLAGS += -DLWEXT4_HEAP_SIZE=<字节数>
  ```
- **接口**：`ext4_user_malloc` / `ext4_user_calloc` / `ext4_user_realloc` / `ext4_user_free`
- **线程安全**：当前实现不含锁，若需多线程访问文件系统，应在外层加锁。

---

## Makefile 集成

`make ARCH=<arch> kernel` 会自动编译并链接 lwext4，无需额外步骤。

相关变量（在 Makefile 中定义）：

```makefile
LWEXT4_DIR      := fs/lwext4         # lwext4 源码目录
LWEXT4_PORT_DIR := fs/lwext4_port    # 移植层目录
LWEXT4_COMPAT   := fs/compat         # 标准库兼容头目录

LWEXT4_OBJS     # 所有 lwext4/src/*.c 的目标文件
LWEXT4_PORT_OBJS # kmalloc.o + libc_stub.o
```

lwext4 使用专用的 `LWEXT4_CFLAGS`（在通用 `CFLAGS` 基础上追加 `-I` 路径和配置宏，并用 `-w` 屏蔽第三方代码警告）。

---

## 使用 lwext4

### 1. 实现块设备接口

lwext4 通过 `ext4_blockdev_iface` 结构体抽象底层存储，需要实现以下四个函数：

```c
#include <ext4.h>
#include <ext4_blockdev.h>

static int my_open(struct ext4_blockdev *bdev)
{
    /* 初始化设备，填写块大小和块数 */
    bdev->bdif->ph_bsize  = 512;          /* 物理块大小（字节）*/
    bdev->bdif->ph_bcnt   = DISK_BLOCKS;  /* 总块数 */
    bdev->bdif->ph_bbuf   = bdev->bdif->ph_bbuf; /* 已由宏分配 */
    return EOK;
}

static int my_bread(struct ext4_blockdev *bdev, void *buf,
                    uint64_t blk_id, uint32_t blk_cnt)
{
    /* 从块 blk_id 开始读取 blk_cnt 个块到 buf */
    memcpy(buf, disk + blk_id * 512, blk_cnt * 512);
    return EOK;
}

static int my_bwrite(struct ext4_blockdev *bdev, const void *buf,
                     uint64_t blk_id, uint32_t blk_cnt)
{
    memcpy(disk + blk_id * 512, buf, blk_cnt * 512);
    return EOK;
}

static int my_close(struct ext4_blockdev *bdev)
{
    return EOK;
}
```

### 2. 注册块设备并挂载

```c
#include <ext4.h>

/* 声明块设备（宏自动分配内部缓冲区）*/
EXT4_BLOCKDEV_STATIC_INSTANCE(my_bdev, 512, DISK_BLOCKS,
                               my_open, my_bread, my_bwrite, my_close,
                               0, 0);

void fs_init(void)
{
    int rc;

    /* 注册块设备 */
    rc = ext4_device_register(&my_bdev, "sda");
    if (rc != EOK) { /* 处理错误 */ return; }

    /* 挂载到 /mnt/ext4/ */
    rc = ext4_mount("sda", "/mnt/ext4/", false);
    if (rc != EOK) { /* 处理错误 */ return; }
}
```

### 3. 文件操作示例

```c
#include <ext4.h>

void fs_demo(void)
{
    ext4_file f;
    int rc;
    size_t bw;

    /* 写文件 */
    rc = ext4_fopen(&f, "/mnt/ext4/hello.txt", "w+");
    if (rc == EOK) {
        ext4_fwrite(&f, "Hello, Avatar OS!\n", 18, &bw);
        ext4_fclose(&f);
    }

    /* 读文件 */
    char buf[64] = {0};
    size_t br;
    rc = ext4_fopen(&f, "/mnt/ext4/hello.txt", "r");
    if (rc == EOK) {
        ext4_fread(&f, buf, sizeof(buf) - 1, &br);
        ext4_fclose(&f);
    }

    /* 目录操作 */
    ext4_dir dir;
    ext4_direntry *de;
    rc = ext4_dir_open(&dir, "/mnt/ext4/");
    if (rc == EOK) {
        while ((de = ext4_dir_entry_next(&dir)) != NULL)
            /* 遍历目录项 */;
        ext4_dir_close(&dir);
    }
}
```

### 4. 卸载

```c
ext4_umount("/mnt/ext4/");
ext4_device_unregister("sda");
```

---

## 测试镜像制作（Host 侧）

```bash
# 创建 8MB ext4 镜像
dd if=/dev/zero of=disk.img bs=1M count=8
mkfs.ext4 -b 1024 disk.img

# 挂载并放入测试文件
sudo mount -o loop disk.img /mnt/test
echo "hello" | sudo tee /mnt/test/hello.txt
sudo umount /mnt/test

# 将镜像嵌入内核二进制（objcopy 方式）
aarch64-linux-musl-objcopy -I binary -O elf64-littleaarch64 \
    -B aarch64 disk.img build/disk.o
# 链接时加入 build/disk.o，在代码中用 _binary_disk_img_start 访问
```

---

## 错误码参考

lwext4 函数成功返回 `EOK (0)`，失败返回正数 errno 值（见 `include/ext4_errno.h`）：

| 值 | 含义 |
|---|---|
| `EOK` (0) | 成功 |
| `EIO` (5) | I/O 错误 |
| `ENOMEM` (12) | 内存不足 |
| `ENOENT` (2) | 文件不存在 |
| `EEXIST` (17) | 文件已存在 |
| `ENOSPC` (28) | 磁盘空间不足 |
| `EROFS` (30) | 只读文件系统 |
