# fs — C API

[← 返回总览](index.md) · 19 个函数 · 说明覆盖率 100.0%

<a id="mod-fs-fatfs_port-diskio.c"></a>

## fs/fatfs_port/diskio.c — FatFs 的块设备适配 + 挂载

`fs/fatfs_port/diskio.c` · 9 个函数 · 9 个有说明 · 185 行

FatFs 和 lwext4 共用同一个 struct ext4_blockdev（MBR 分区只是不同的
part_offset/part_size，bdif 是同一个）。FAT 这边只做 512 字节对齐的整扇区
I/O，直接走 bdif->bread/bwrite，不会碰 lwext4 的 ph_bbuf；两个文件系统的
所有 API 调用由 include/fs/fs_lock.h 的全局锁串行化。

编译要求：FATFS_CFLAGS（包含 lwext4 头 + FatFs 头 + fs/fatfs_port）。

<a id="fn-fs-fatfs_port-diskio.c-37"></a>

### `fat_errno`

```c
static int fat_errno(FRESULT fr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 switch 把 FatFs 的 FRESULT 映射为负的 Linux errno，未匹配的默认返回 -5（EIO）。

<details>
<summary>函数内备注（10 条）</summary>

**第 44 行（行尾）**

> ENOENT

**第 46 行（行尾）**

> EEXIST

**第 48 行（行尾）**

> EACCES

**第 50 行（行尾）**

> EROFS

**第 52 行（行尾）**

> EINVAL

**第 54 行（行尾）**

> ENOMEM

**第 56 行（行尾）**

> EMFILE

**第 58 行（行尾）**

> EBADF

**第 62 行（行尾）**

> ENODEV

**第 64 行（行尾）**

> EIO

</details>

**位置**：`fs/fatfs_port/diskio.c:37`

<a id="fn-fs-fatfs_port-diskio.c-68"></a>

### `disk_status`

```c
DSTATUS disk_status(BYTE pdrv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

仅当驱动号为 0 且全局 FAT 块设备指针非空时返回 0，否则返回 STA_NOINIT。

**位置**：`fs/fatfs_port/diskio.c:68`

<a id="fn-fs-fatfs_port-diskio.c-75"></a>

### `disk_initialize`

```c
DSTATUS disk_initialize(BYTE pdrv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验驱动号、块设备读写回调与 512 字节块大小，首次调用时再执行 ext4_block_init，任一检查失败返回 STA_NOINIT。

<details>
<summary>函数内备注（1 条）</summary>

**第 84 行**

> root ext4 已经先挂好，这里通常只是给共享 bdif 加一次引用。

</details>

**位置**：`fs/fatfs_port/diskio.c:75`

<a id="fn-fs-fatfs_port-diskio.c-93"></a>

### `fat_transfer`

```c
static DRESULT fat_transfer(BYTE pdrv, BYTE *buf, LBA_t sector, UINT count, bool write)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验驱动、缓冲区、扇区大小与分区偏移，换算分区内 LBA 后按 FAT_IO_CHUNK 分批调用 bread/bwrite，越界或任一批失败返回错误。

**位置**：`fs/fatfs_port/diskio.c:93`

<a id="fn-fs-fatfs_port-diskio.c-122"></a>

### `disk_read`

```c
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以非写入方式转调 fat_transfer 读取扇区。

**位置**：`fs/fatfs_port/diskio.c:122`

<a id="fn-fs-fatfs_port-diskio.c-127"></a>

### `disk_write`

```c
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把缓冲指针强转后以写入方式转调 fat_transfer 写扇区。

**位置**：`fs/fatfs_port/diskio.c:127`

<a id="fn-fs-fatfs_port-diskio.c-132"></a>

### `disk_ioctl`

```c
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

处理 CTRL_SYNC 及扇区数、扇区大小、块大小查询（结果写入 buff），其余命令返回 RES_PARERR。

**位置**：`fs/fatfs_port/diskio.c:132`

<a id="fn-fs-fatfs_port-diskio.c-154"></a>

### `fatfs_mount`

```c
int fatfs_mount(struct ext4_blockdev *part, const char *mount_point)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验分区参数后记录块设备，持 fs_global_lock 调用 f_mount；失败则清空设备并返回 errno，成功置挂载标志并打印日志。

<details>
<summary>函数内备注（2 条）</summary>

**第 157 行（行尾）**

> EINVAL

**第 160 行**

> FF_LBA64=0：LBA_t 是 32-bit，卷不能超过 2 TiB（512B 扇区）

</details>

**位置**：`fs/fatfs_port/diskio.c:154`

<a id="fn-fs-fatfs_port-diskio.c-182"></a>

### `fatfs_is_mounted`

```c
bool fatfs_is_mounted(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回全局 FAT 挂载标志。

**位置**：`fs/fatfs_port/diskio.c:182`

<a id="mod-fs-lwext4_port-fs_lock.c"></a>

## fs/lwext4_port/fs_lock.c — lwext4 的全局串行化

`fs/lwext4_port/fs_lock.c` · 5 个函数 · 5 个有说明 · 222 行

为什么需要：lwext4 是个单线程用户态库, 它的全局缓冲缓存(一棵红黑树)
没有任何内部同步。内核这边从前也没有在 fs 路径上加过锁, 于是 SMP>1 时
多个核并发 exec 就会把那棵树写坏：

#0  ext4_bcache_free / ext4_buf_lru_RB_REMOVE_COLOR   (两个不同 trace)
#4  ext4_fread <- read_file_fully <- sys_execve

实测：riscv64 / aarch64 纯 exec 压力下, SMP=1 连续多次干净,
SMP=4 连续两次都崩。

为什么用 --wrap 而不是在调用点加锁：
1. 内核里有 90+ 个 lwext4 调用点, 散在 8 个文件；22 个直接调用者里有
15 个是多出口的(提前 return), 手工配对 lock/unlock 必然漏。
2. 粒度要落在每次 API 调用上, 不能落在"整个内核函数"上 ——
read_file_fully 要循环读 5.6MB, 整函数持锁意味着关中断几十到几百
毫秒, 会把 timer tick 丢光。
`-Wl,--wrap=<sym>` 让链接器把所有 `sym` 引用改指向 `__wrap_sym`,
真实实现仍可经 `__real_sym` 调用 —— 正好是"每次调用"的粒度,
而且零调用点改动。

锁是可重入的：lwext4 内部会回调内核(ext4_user_calloc 等),
那些回调走的路径也可能再进来。按 CPU 记 owner + 深度计数。
持锁期间关本核中断：否则本核上的另一个任务抢占后再拿这把锁会自死锁
(owner 判定会把它误认成递归)。

强度说明：这是把 lwext4 当成一次一个核的临界区, 不是细粒度锁。
好处是绝对安全；代价是 fs 操作在 SMP 下串行。等真要抠性能时再谈
按 inode/挂载点分锁。

注意： 包装函数必须经下面的 FS_WRAP 宏生成, 不许手写。手抄签名的代价
实测过：漏掉 `__real_*` 的声明 -> 编译器按隐式声明当成返回 `int`
-> aarch64 上指针返回值被 `sxtw` 截成低 32 位
(`0xffff0000416d20c0` 变成 `0x416d20c0`), `ls` 第一次 getdents 就
死在 `ext4_dir_getdents+0x40`, FAR 每次都是同一个值 —— 一个"看起来
完全像竞态"的确定性崩溃。FS_WRAP 用 `__typeof__(fn)` 从头文件取真身
签名, 再用 _Static_assert 钉住, 签名对不上直接编译失败。

<a id="fn-fs-lwext4_port-fs_lock.c-74"></a>

### `fs_lock_enable_sleeping`

```c
void fs_lock_enable_sleeping(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

初始化全局文件系统互斥量并把锁模式切换为可睡眠的 mutex。

**位置**：`fs/lwext4_port/fs_lock.c:74`

<a id="fn-fs-lwext4_port-fs_lock.c-80"></a>

### `fs_lock`

```c
static inline void fs_lock(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

可睡眠模式下直接取互斥量；否则按 CPU 记录持有者实现递归，首次获取时保存中断状态、加自旋锁并设置深度与持有者。

<details>
<summary>函数内备注（1 条）</summary>

**第 91 行**

> 递归：本核已经持有

</details>

**位置**：`fs/lwext4_port/fs_lock.c:80`

<a id="fn-fs-lwext4_port-fs_lock.c-103"></a>

### `fs_unlock`

```c
static inline void fs_unlock(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

可睡眠模式下释放互斥量；否则递减本 CPU 递归深度，归零时清持有者、释放自旋锁并恢复中断状态。

**位置**：`fs/lwext4_port/fs_lock.c:103`

<a id="fn-fs-lwext4_port-fs_lock.c-136"></a>

### `fs_global_lock`

```c
void fs_global_lock(void)
```

上面两个是 --wrap 包装用的每次调用粒度。但有些操作是跨多次调用的,
单独锁每次调用不够：比如目录迭代 —— `ext4_dir_entry_next` 把条目拷进
`dir->de` 并返回它的地址, 调用方要拿着这个指针继续用(lwext4 的
`ext4_dir` 是内联在 `vfs_file_t` 里的, `de` 就在同一个对象里)。如果只锁
单次调用, 两次调用之间另一个核可以对同一个目录对象做别的事。

FatFs 也有同样的需求（f_stat+f_open、f_lseek+f_truncate、整个 f_readdir
循环），而且它和 lwext4 共用同一个块设备接口。所以这对函数已经不再只属于
lwext4，统一叫 fs_global_lock/fs_global_unlock（见 include/fs/fs_lock.h）。

这类地方要把整个序列圈进一个临界区。锁是可重入的, 所以外面加一层
粗锁, 里面每次调用那层细锁照常嵌套, 不会死锁。

**位置**：`fs/lwext4_port/fs_lock.c:136`

<a id="fn-fs-lwext4_port-fs_lock.c-141"></a>

### `fs_global_unlock`

```c
void fs_global_unlock(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

直接调用 fs_unlock 释放文件系统全局锁。

**位置**：`fs/lwext4_port/fs_lock.c:141`

<a id="mod-fs-lwext4_port-kmalloc.c"></a>

## fs/lwext4_port/kmalloc.c — lwext4 内存分配器

`fs/lwext4_port/kmalloc.c` · 5 个函数 · 5 个有说明 · 135 行

为 lwext4 提供 ext4_user_malloc / ext4_user_free / ext4_user_calloc /
ext4_user_realloc 四个接口, 通过一个静态堆上的 first-fit free-list
分配器实现, 不依赖任何操作系统服务。

堆大小默认 512KB, 可通过编译选项 -DLWEXT4_HEAP_SIZE=<bytes> 调整。

<a id="fn-fs-lwext4_port-kmalloc.c-40"></a>

### `heap_init`

```c
static void heap_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把静态堆内存初始化为唯一一个空闲块，设置其大小并清空后继指针。

**位置**：`fs/lwext4_port/kmalloc.c:40`

<a id="fn-fs-lwext4_port-kmalloc.c-49"></a>

### `ext4_user_malloc`

```c
void *ext4_user_malloc(size_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

首次使用时初始化堆，按对齐大小首次适配遍历块链表，剩余空间足够则分裂，返回块内负载，堆耗尽返回 NULL。

<details>
<summary>函数内备注（2 条）</summary>

**第 62 行**

> 若剩余空间足够容纳新的块头 + 最小分配单元, 则分裂

**第 77 行（行尾）**

> 堆耗尽

</details>

**位置**：`fs/lwext4_port/kmalloc.c:49`

<a id="fn-fs-lwext4_port-kmalloc.c-81"></a>

### `ext4_user_free`

```c
void ext4_user_free(void *ptr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把目标块标记为空闲后，遍历整条块链反复合并相邻空闲块。

<details>
<summary>函数内备注（2 条）</summary>

**第 89 行**

> 向后合并相邻空闲块

**第 95 行**

> 不前进 cur, 继续尝试合并

</details>

**位置**：`fs/lwext4_port/kmalloc.c:81`

<a id="fn-fs-lwext4_port-kmalloc.c-103"></a>

### `ext4_user_calloc`

```c
void *ext4_user_calloc(size_t nmemb, size_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

计算 nmemb 与 size 乘积后分配内存并清零，分配失败返回 NULL。

**位置**：`fs/lwext4_port/kmalloc.c:103`

<a id="fn-fs-lwext4_port-kmalloc.c-113"></a>

### `ext4_user_realloc`

```c
void *ext4_user_realloc(void *ptr, size_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空指针转分配、size 为 0 转释放；原块已够大直接复用，否则新分配并拷贝原有数据后释放旧块。

<details>
<summary>函数内备注（1 条）</summary>

**第 125 行**

> 原块已足够大, 直接返回

</details>

**位置**：`fs/lwext4_port/kmalloc.c:113`
