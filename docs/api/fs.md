# fs — C API

[← 返回总览](index.md) · 226 个函数 · 说明覆盖率 15.5%

<a id="mod-fs-ext4-port-fs_lock.c"></a>

## fs/ext4/port/fs_lock.c — lwext4 的全局串行化

`fs/ext4/port/fs_lock.c` · 5 个函数 · 1 个有说明 · 222 行

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

<a id="fn-fs-ext4-port-fs_lock.c-74"></a>

### `fs_lock_enable_sleeping`

```c
void fs_lock_enable_sleeping(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/ext4/port/fs_lock.c:74`

<a id="fn-fs-ext4-port-fs_lock.c-80"></a>

### `fs_lock`

```c
static inline void fs_lock(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 91 行**

> 递归：本核已经持有

</details>

**位置**：`fs/ext4/port/fs_lock.c:80`

<a id="fn-fs-ext4-port-fs_lock.c-103"></a>

### `fs_unlock`

```c
static inline void fs_unlock(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/ext4/port/fs_lock.c:103`

<a id="fn-fs-ext4-port-fs_lock.c-136"></a>

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

**位置**：`fs/ext4/port/fs_lock.c:136`

<a id="fn-fs-ext4-port-fs_lock.c-141"></a>

### `fs_global_unlock`

```c
void fs_global_unlock(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/ext4/port/fs_lock.c:141`

<a id="mod-fs-ext4-port-kmalloc.c"></a>

## fs/ext4/port/kmalloc.c — lwext4 内存分配器

`fs/ext4/port/kmalloc.c` · 5 个函数 · 0 个有说明 · 135 行

为 lwext4 提供 ext4_user_malloc / ext4_user_free / ext4_user_calloc /
ext4_user_realloc 四个接口, 通过一个静态堆上的 first-fit free-list
分配器实现, 不依赖任何操作系统服务。

堆大小默认 512KB, 可通过编译选项 -DLWEXT4_HEAP_SIZE=<bytes> 调整。

<a id="fn-fs-ext4-port-kmalloc.c-40"></a>

### `heap_init`

```c
static void heap_init(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/ext4/port/kmalloc.c:40`

<a id="fn-fs-ext4-port-kmalloc.c-49"></a>

### `ext4_user_malloc`

```c
void *ext4_user_malloc(size_t size)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（2 条）</summary>

**第 62 行**

> 若剩余空间足够容纳新的块头 + 最小分配单元, 则分裂

**第 77 行（行尾）**

> 堆耗尽

</details>

**位置**：`fs/ext4/port/kmalloc.c:49`

<a id="fn-fs-ext4-port-kmalloc.c-81"></a>

### `ext4_user_free`

```c
void ext4_user_free(void *ptr)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（2 条）</summary>

**第 89 行**

> 向后合并相邻空闲块

**第 95 行**

> 不前进 cur, 继续尝试合并

</details>

**位置**：`fs/ext4/port/kmalloc.c:81`

<a id="fn-fs-ext4-port-kmalloc.c-103"></a>

### `ext4_user_calloc`

```c
void *ext4_user_calloc(size_t nmemb, size_t size)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/ext4/port/kmalloc.c:103`

<a id="fn-fs-ext4-port-kmalloc.c-113"></a>

### `ext4_user_realloc`

```c
void *ext4_user_realloc(void *ptr, size_t size)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 125 行**

> 原块已足够大, 直接返回

</details>

**位置**：`fs/ext4/port/kmalloc.c:113`

<a id="mod-fs-fat-port-diskio.c"></a>

## fs/fat/port/diskio.c — FatFs 的块设备适配 + 挂载

`fs/fat/port/diskio.c` · 9 个函数 · 0 个有说明 · 185 行

FatFs 和 lwext4 共用同一个 struct ext4_blockdev（MBR 分区只是不同的
part_offset/part_size，bdif 是同一个）。FAT 这边只做 512 字节对齐的整扇区
I/O，直接走 bdif->bread/bwrite，不会碰 lwext4 的 ph_bbuf；两个文件系统的
所有 API 调用由 include/fs/fs_lock.h 的全局锁串行化。

编译要求：FATFS_CFLAGS（包含 lwext4 头 + FatFs 头 + fs/fat/port）。

<a id="fn-fs-fat-port-diskio.c-37"></a>

### `fat_errno`

```c
static int fat_errno(FRESULT fr)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

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

**位置**：`fs/fat/port/diskio.c:37`

<a id="fn-fs-fat-port-diskio.c-68"></a>

### `disk_status`

```c
DSTATUS disk_status(BYTE pdrv)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/fat/port/diskio.c:68`

<a id="fn-fs-fat-port-diskio.c-75"></a>

### `disk_initialize`

```c
DSTATUS disk_initialize(BYTE pdrv)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 84 行**

> root ext4 已经先挂好，这里通常只是给共享 bdif 加一次引用。

</details>

**位置**：`fs/fat/port/diskio.c:75`

<a id="fn-fs-fat-port-diskio.c-93"></a>

### `fat_transfer`

```c
static DRESULT fat_transfer(BYTE pdrv, BYTE *buf, LBA_t sector, UINT count, bool write)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/fat/port/diskio.c:93`

<a id="fn-fs-fat-port-diskio.c-122"></a>

### `disk_read`

```c
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/fat/port/diskio.c:122`

<a id="fn-fs-fat-port-diskio.c-127"></a>

### `disk_write`

```c
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/fat/port/diskio.c:127`

<a id="fn-fs-fat-port-diskio.c-132"></a>

### `disk_ioctl`

```c
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/fat/port/diskio.c:132`

<a id="fn-fs-fat-port-diskio.c-154"></a>

### `fatfs_mount`

```c
int fatfs_mount(struct ext4_blockdev *part, const char *mount_point)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（2 条）</summary>

**第 157 行（行尾）**

> EINVAL

**第 160 行**

> FF_LBA64=0：LBA_t 是 32-bit，卷不能超过 2 TiB（512B 扇区）

</details>

**位置**：`fs/fat/port/diskio.c:154`

<a id="fn-fs-fat-port-diskio.c-182"></a>

### `fatfs_is_mounted`

```c
bool fatfs_is_mounted(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/fat/port/diskio.c:182`

<a id="mod-fs-init.c"></a>

## fs/init.c — 根文件系统与 MBR 分区初始化

`fs/init.c` · 10 个函数 · 1 个有说明 · 357 行

这是**文件系统总入口**，不属于 lwext4 移植层：它把块设备注册到 lwext4，
把 ext4 分区挂到 "/"；如果磁盘是 MBR 分区表，再把第一个 FAT 分区挂到
FAT_MOUNT_POINT（默认 /boot）。

根块设备由驱动框架提供：ramblk/sdblk 在 DRV_PHASE_STORAGE 注册自己，
fs_init() 只问 block_root_device()，不再按平台/驱动 #if 选择。

MBR 解析必须自己做，不能复用 lwext4 的 ext4_mbr_scan()：它只保留
0x83（Linux native），FAT 分区会被直接丢弃（part_size 留 0）。

编译要求：FATFS_CFLAGS（同时包含 lwext4 与 FatFs 头文件）。

<a id="fn-fs-init.c-70"></a>

### `mbr_le32`

```c
static uint32_t mbr_le32(const uint8_t *p)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/init.c:70`

<a id="fn-fs-init.c-75"></a>

### `mbr_type_is_ext4`

```c
static bool mbr_type_is_ext4(uint8_t type)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/init.c:75`

<a id="fn-fs-init.c-80"></a>

### `mbr_type_is_fat`

```c
static bool mbr_type_is_fat(uint8_t type)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（7 条）</summary>

**第 83 行（行尾）**

> FAT12

**第 84 行（行尾）**

> FAT16 &lt; 32M

**第 85 行（行尾）**

> FAT16

**第 86 行（行尾）**

> FAT32

**第 87 行（行尾）**

> FAT32 LBA

**第 88 行（行尾）**

> FAT16 LBA

**第 89 行（行尾）**

> EFI system partition (FAT)

</details>

**位置**：`fs/init.c:80`

<a id="fn-fs-init.c-106"></a>

### `mbr_read`

```c
static int mbr_read(struct ext4_blockdev *disk, struct mbr_table *out)
```

mbr_read - 读扇区 0 并解析 4 个主分区

返回 0 表示读取成功（out->valid 表示是否找到了可用的 MBR 分区）；
负值表示设备错误。没有 MBR 签名时 out->valid = false，调用方按整盘
ext4 处理（QEMU/ramblk 的旧镜像就是这种）。

判据刻意收紧：只有至少一条“类型已知 + LBA/长度在设备范围内”的分区
才算 MBR。裸 FAT 镜像的引导扇区同样有 0x55AA，不能只看签名。

<details>
<summary>函数内备注（2 条）</summary>

**第 117 行**

> 确保 sdblk 的 ph_bcnt/part_size 已经由 open 回调填好

**第 134 行（行尾）**

> 不是 MBR

</details>

**位置**：`fs/init.c:106`

<a id="fn-fs-init.c-179"></a>

### `mbr_make_bdev`

```c
static void mbr_make_bdev(struct ext4_blockdev *dst, struct ext4_blockdev *parent, const struct mbr_part *part)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/init.c:179`

<a id="fn-fs-init.c-187"></a>

### `fs_log_ext4_features`

```c
static void fs_log_ext4_features(struct ext4_blockdev *bdev)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（2 条）</summary>

**第 206 行**

> 成功读到了超级块 —— 这是&quot;生效配置&quot;, 不是错误。

**第 209 行**

> 只在确实存在不支持的位时才 WARN：非零意味着我们在静默忽略磁盘上的
> 语义, 那是真该被看见的；全零是正常情况, 不该占 WARN 的注意力。

</details>

**位置**：`fs/init.c:187`

<a id="fn-fs-init.c-224"></a>

### `fs_dump_root_dir`

```c
static void fs_dump_root_dir(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 232 行**

> 目录项数量由镜像内容决定 —— 逐项一行, 只能采样, 不能留在 INFO

</details>

**位置**：`fs/init.c:224`

<a id="fn-fs-init.c-239"></a>

### `fs_mount_bdev`

```c
static int fs_mount_bdev(struct ext4_blockdev *bdev, const char *name, int partitioned)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/init.c:239`

<a id="fn-fs-init.c-269"></a>

### `fs_init_mbr`

```c
static int fs_init_mbr(struct ext4_blockdev *raw, const struct mbr_table *tbl)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 326 行（行尾）**

> root 仍然可用

</details>

**位置**：`fs/init.c:269`

<a id="fn-fs-init.c-333"></a>

### `fs_init`

```c
int fs_init(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 350 行**

> 没有 MBR：ramblk 平台保持“整盘 ext4”的旧行为，sdblk 则视为错误

</details>

**位置**：`fs/init.c:333`

<a id="mod-fs-pseudofs-dev.c"></a>

## fs/pseudofs/dev.c — 出错时把刚刚分配给用户的 fd 收回来。

`fs/pseudofs/dev.c` · 12 个函数 · 3 个有说明 · 381 行

为什么需要：ALLOC 是「分配缓冲区 → 拿 fd → 把 fd 写回用户态」三步,
最后一步会失败(用户传个可读不可写, 或已 munmap 的地址)。失败时不收回,
缓冲区内存和 fd 池槽位就一起泄漏 —— 而且每次调用漏一个。

必须走完整的 close 序列(take → close → free)：只调 fd_pool_free 会让
dma_buf 的引用计数少还一份, 内存永不释放。

用 extern 局部声明而不是 include fd_pool.h：后者会拉进 vfs.h → ext4.h,
而 pseudofs 的编译规则没有 lwext4 的 -I(本文件上面 ion_alloc_fd_for_current
也是同样的写法)。越界检查由 fd_obj_close / fd_pool_free 自己兜。

<a id="fn-fs-pseudofs-dev.c-32"></a>

### `dmabuf_close_fresh_fd`

```c
static void dmabuf_close_fresh_fd(int fd)
```

出错时把刚刚分配给用户的 fd 收回来。

为什么需要：ALLOC 是「分配缓冲区 → 拿 fd → 把 fd 写回用户态」三步,
最后一步会失败(用户传个可读不可写, 或已 munmap 的地址)。失败时不收回,
缓冲区内存和 fd 池槽位就一起泄漏 —— 而且每次调用漏一个。

必须走完整的 close 序列(take → close → free)：只调 fd_pool_free 会让
dma_buf 的引用计数少还一份, 内存永不释放。

用 extern 局部声明而不是 include fd_pool.h：后者会拉进 vfs.h → ext4.h,
而 pseudofs 的编译规则没有 lwext4 的 -I(本文件上面 ion_alloc_fd_for_current
也是同样的写法)。越界检查由 fd_obj_close / fd_pool_free 自己兜。

<details>
<summary>函数内备注（1 条）</summary>

**第 44 行（行尾）**

> 内部会把 dma_buf 那份引用还掉

</details>

**位置**：`fs/pseudofs/dev.c:32`

<a id="fn-fs-pseudofs-dev.c-56"></a>

### `dma_heap_dev_ioctl`

```c
int dma_heap_dev_ioctl(int nid, uint64_t req, void *argp)
```

/dev/dma_heap/<name> —— 官方 librknnrt 的每一块缓冲区都从这里分配,
见 docs/npu/RK3588_NPU_DRIVER.md §2。

注意： 用户指针一律走 copy_*_user(CLAUDE.md 铁律)。本文件里既有的
ion_dev_ioctl 是裸解引用(`r->size = ...` 直接写用户内存),
用户传非法地址就是内核态写坏地址 → #PF → 整机挂死。别照抄那一段。

<details>
<summary>函数内备注（1 条）</summary>

**第 78 行**

> 引用计数走法(对齐 ion 的写法)：
> dma_buf_alloc            → 1(&quot;创建&quot;那份)
> dma_buf_alloc_fd_current → 2(fd 拿走一份)
> dma_buf_put(下面那句)  → 1 —— 只剩 fd 那份
> fd 关闭时 fd_obj_close    → 0 → 内存还给堆

</details>

**位置**：`fs/pseudofs/dev.c:56`

<a id="fn-fs-pseudofs-dev.c-108"></a>

### `dma_buf_fd_ioctl`

```c
int dma_buf_fd_ioctl(uint32_t idx, uint64_t req, void *argp)
```

打在 dmabuf fd 自身 上的 ioctl。librknnrt 的 DMA_BUF_IOCTL_SYNC 就是这么
发的 —— 它不走 /dev/ 节点, 所以必须由 ioctl_handler 按 fd 类型分派过来
(见 kernel/syscall/fs/ioctl.c 里的 FDT_DMABUF 分支)。

**位置**：`fs/pseudofs/dev.c:108`

<a id="fn-fs-pseudofs-dev.c-122"></a>

### `ion_alloc_fd_for_current`

```c
static int ion_alloc_fd_for_current(ion_handle_t handle)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/dev.c:122`

<a id="fn-fs-pseudofs-dev.c-134"></a>

### `ion_handle_from_user_fd`

```c
static ion_handle_t ion_handle_from_user_fd(int fd)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/dev.c:134`

<a id="fn-fs-pseudofs-dev.c-145"></a>

### `tpu_cache_fd_op`

```c
static int tpu_cache_fd_op(int32_t fd, int invalidate)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/dev.c:145`

<a id="fn-fs-pseudofs-dev.c-165"></a>

### `tpu_cache_range_op`

```c
static int tpu_cache_range_op(const struct cvitpu_legacy_cache_op_arg *op, int invalidate)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/dev.c:165`

<a id="fn-fs-pseudofs-dev.c-199"></a>

### `ion_dev_ioctl`

```c
int ion_dev_ioctl(int nid, uint64_t req, void *argp)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/dev.c:199`

<a id="fn-fs-pseudofs-dev.c-297"></a>

### `tpu_dev_ioctl`

```c
int tpu_dev_ioctl(int nid, uint64_t req, void *argp)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/dev.c:297`

<a id="fn-fs-pseudofs-dev.c-357"></a>

### `npu_dev_ioctl`

```c
int npu_dev_ioctl(int nid, uint64_t req, void *argp)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/dev.c:357`

<a id="fn-fs-pseudofs-dev.c-366"></a>

### `video0_read`

```c
int video0_read(int nid, uint64_t off, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/dev.c:366`

<a id="fn-fs-pseudofs-dev.c-375"></a>

### `video0_ioctl`

```c
int video0_ioctl(int nid, uint64_t req, void *argp)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/dev.c:375`

<a id="mod-fs-pseudofs-dir.c"></a>

## fs/pseudofs/dir.c

`fs/pseudofs/dir.c` · 5 个函数 · 0 个有说明 · 147 行

<a id="fn-fs-pseudofs-dir.c-12"></a>

### `dirent_type`

```c
static uint8_t dirent_type(pseudo_type_t type)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/dir.c:12`

<a id="fn-fs-pseudofs-dir.c-28"></a>

### `emit_dirent`

```c
static size_t emit_dirent(void *buf, size_t written, size_t bufsz, uint64_t ino, uint64_t seq, uint8_t dtype, const char *name)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/dir.c:28`

<a id="fn-fs-pseudofs-dir.c-46"></a>

### `is_direct_child`

```c
static bool is_direct_child(const char *dir_path, const char *child_path)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/dir.c:46`

<a id="fn-fs-pseudofs-dir.c-77"></a>

### `emit_proc_pid_dirs`

```c
static int emit_proc_pid_dirs(uint64_t *off, void *buf, size_t bufsz, size_t *written, uint64_t idx)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/dir.c:77`

<a id="fn-fs-pseudofs-dir.c-100"></a>

### `pfs_getdents_node`

```c
int pfs_getdents_node(int nid, uint64_t *off, void *buf, size_t bufsz)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/dir.c:100`

<a id="mod-fs-pseudofs-proc.c"></a>

## fs/pseudofs/proc.c

`fs/pseudofs/proc.c` · 11 个函数 · 0 个有说明 · 258 行

<a id="fn-fs-pseudofs-proc.c-15"></a>

### `pfs_task_alive`

```c
bool pfs_task_alive(uint32_t slot)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/proc.c:15`

<a id="fn-fs-pseudofs-proc.c-21"></a>

### `pfs_task_pid`

```c
uint32_t pfs_task_pid(uint32_t slot)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/proc.c:21`

<a id="fn-fs-pseudofs-proc.c-26"></a>

### `pfs_parse_proc_pid`

```c
bool pfs_parse_proc_pid(const char *path, uint32_t *pid_out, const char **rest_out)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/proc.c:26`

<a id="fn-fs-pseudofs-proc.c-48"></a>

### `find_task`

```c
static task_t *find_task(uint32_t pid)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/proc.c:48`

<a id="fn-fs-pseudofs-proc.c-57"></a>

### `format_status`

```c
static size_t format_status(char *tmp, size_t bufsz, task_t *t)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 113 行**

> 调度策略/RT 优先级。数值与 Linux 一致：0=SCHED_NORMAL,
> 1=SCHED_FIFO, 2=SCHED_RR，方便用户态工具按 Linux 习惯解析。
> 非 RT 任务的 RtPrio 恒为 0。

</details>

**位置**：`fs/pseudofs/proc.c:57`

<a id="fn-fs-pseudofs-proc.c-163"></a>

### `pfs_pid_status_read`

```c
int pfs_pid_status_read(uint32_t pid, uint64_t off, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/proc.c:163`

<a id="fn-fs-pseudofs-proc.c-174"></a>

### `stat_read_task`

```c
static int stat_read_task(task_t *t, uint64_t off, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/proc.c:174`

<a id="fn-fs-pseudofs-proc.c-215"></a>

### `pfs_pid_stat_read`

```c
int pfs_pid_stat_read(uint32_t pid, uint64_t off, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/proc.c:215`

<a id="fn-fs-pseudofs-proc.c-221"></a>

### `pfs_self_status_read`

```c
int pfs_self_status_read(uint64_t off, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/proc.c:221`

<a id="fn-fs-pseudofs-proc.c-227"></a>

### `pfs_self_stat_read`

```c
int pfs_self_stat_read(uint64_t off, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/proc.c:227`

<a id="fn-fs-pseudofs-proc.c-232"></a>

### `pfs_stat_pid_path`

```c
int pfs_stat_pid_path(uint32_t pid, const char *rest, struct kernel_stat *st)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/proc.c:232`

<a id="mod-fs-pseudofs-pseudofs.c"></a>

## fs/pseudofs/pseudofs.c — 虚拟文件系统实现

`fs/pseudofs/pseudofs.c` · 38 个函数 · 17 个有说明 · 761 行

节点表(g_pseudo_nodes[])将路径映射到内容生成器回调。
syscall.c 通过 API 函数在各系统调用处集成此模块。

依赖(均已在全局 CFLAGS 中)：
-Iinclude  -Idriver  -Ikernel  -Ikernel/mm

<a id="fn-fs-pseudofs-pseudofs.c-40"></a>

### `lfsr_byte`

```c
static uint8_t lfsr_byte(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/pseudofs.c:40`

<a id="fn-fs-pseudofs-pseudofs.c-51"></a>

### `null_read`

```c
static int null_read(int nid, uint64_t off, void *buf, size_t len)
```

/dev/null

**位置**：`fs/pseudofs/pseudofs.c:51`

<a id="fn-fs-pseudofs-pseudofs.c-59"></a>

### `null_write`

```c
static int null_write(int nid, const void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/pseudofs.c:59`

<a id="fn-fs-pseudofs-pseudofs.c-67"></a>

### `zero_read`

```c
static int zero_read(int nid, uint64_t off, void *buf, size_t len)
```

/dev/zero

**位置**：`fs/pseudofs/pseudofs.c:67`

<a id="fn-fs-pseudofs-pseudofs.c-80"></a>

### `tty_read`

```c
static int tty_read(int nid, uint64_t off, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 95 行（行尾）**

> raw: 单字符; canonical: 换行截止

</details>

**位置**：`fs/pseudofs/pseudofs.c:80`

<a id="fn-fs-pseudofs-pseudofs.c-102"></a>

### `tty_write`

```c
static int tty_write(int nid, const void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 106 行**

> 逐字符走 klog_write, 而不是整段 —— 因为 src 可能是用户指针
> (vfs_write 的 buf 一路来自 syscall 的 regs[]), 整段输出会把裸访问
> 的爆炸半径从 1 字节放大到 len 字节。逐字符至少保证单字符不被撕裂,
> 且不绕过那把锁。用户指针校验在 syscall 入口那一层补。

</details>

**位置**：`fs/pseudofs/pseudofs.c:102`

<a id="fn-fs-pseudofs-pseudofs.c-118"></a>

### `rand_read`

```c
static int rand_read(int nid, uint64_t off, void *buf, size_t len)
```

/dev/urandom, /dev/random

**位置**：`fs/pseudofs/pseudofs.c:118`

<a id="fn-fs-pseudofs-pseudofs.c-129"></a>

### `maps_read`

```c
static int maps_read(int nid, uint64_t off, void *buf, size_t len)
```

/proc/self/maps — musl 启动时读取, 空文件即可

**位置**：`fs/pseudofs/pseudofs.c:129`

<a id="fn-fs-pseudofs-pseudofs.c-138"></a>

### `status_read`

```c
static int status_read(int nid, uint64_t off, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/pseudofs.c:138`

<a id="fn-fs-pseudofs-pseudofs.c-144"></a>

### `stat_read`

```c
static int stat_read(int nid, uint64_t off, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/pseudofs.c:144`

<a id="fn-fs-pseudofs-pseudofs.c-151"></a>

### `version_read`

```c
static int version_read(int nid, uint64_t off, void *buf, size_t len)
```

/proc/version

**位置**：`fs/pseudofs/pseudofs.c:151`

<a id="fn-fs-pseudofs-pseudofs.c-170"></a>

### `dri_name_read`

```c
static int dri_name_read(int nid, uint64_t off, void *buf, size_t len)
```

/proc/dri/N/name —— DRM 的传统 /proc 接口。

librknnrt 实测会依次读 /proc/dri/0/name … /proc/dri/7/name 来认设备
(内核 trace 抓到的, 见 docs/npu/RK3588_NPU_DRIVER.md §8)。
格式与 Linux 一致：
<驱动名> <major>.<minor>.<patch> <date> <desc>

注意： 驱动名必须是 rknpu —— 它按名字匹配, 写别的(比如我们 DRM 节点上
用的 "rknpu-avatar")会被当成"不是 RKNPU 设备"。

**位置**：`fs/pseudofs/pseudofs.c:170`

<a id="fn-fs-pseudofs-pseudofs.c-180"></a>

### `uptime_put_secs`

```c
static size_t uptime_put_secs(char *dst, size_t pos, size_t cap, uint64_t cs)
```

把"百分之一秒"写成 "S.CC", 返回写入字节数

<details>
<summary>函数内备注（1 条）</summary>

**第 183 行**

> u64_to_dec 是&quot;填 buf, 返回长度&quot;, 不是返回指针 —— 别拿它的返回值当字符串

</details>

**位置**：`fs/pseudofs/pseudofs.c:180`

<a id="fn-fs-pseudofs-pseudofs.c-201"></a>

### `uptime_read`

```c
static int uptime_read(int nid, uint64_t off, void *buf, size_t len)
```

格式与 Linux 一致："<uptime秒> <idle秒>", 各两位小数。

这里以前是写死的静态字符串 "0.00 0.00" —— 跑多久都纹丝不动(用户实测
`cat /proc/uptime` 恒为 0.00 0.00)。硬编码本身不算错, 错在它看起来
像"系统刚起来", 会把人往错误方向带。

<details>
<summary>函数内备注（3 条）</summary>

**第 207 行**

> 上电以来的单调时间。用 timer_get_ns() 而不是 g_system_ticks：
> 后者是 100 Hz 的 tick 计数, 先除成秒再乘回百分秒会丢精度,
> 而且 tick 在中断关闭的窗口里不一定在走。

**第 211 行（行尾）**

> 纳秒 → 百分之一秒

**第 213 行**

> idle：所有核在 idle 任务上累计的 tick 数 ÷ tick 频率。
> 求和而不是只看本核 —— Linux 的 /proc/uptime 第二列就是各 CPU 之和。
> 记账在 sched_tick() 里(见 cpu.h 的 idle_ticks)。

</details>

**位置**：`fs/pseudofs/pseudofs.c:201`

<a id="fn-fs-pseudofs-pseudofs.c-233"></a>

### `rtstat_read`

```c
static int rtstat_read(int nid, uint64_t off, void *buf, size_t len)
```

/proc/rtstat —— R0 延迟统计（wake→run / syscall / tick gap）

**位置**：`fs/pseudofs/pseudofs.c:233`

<a id="fn-fs-pseudofs-pseudofs.c-244"></a>

### `mounts_read`

```c
static int mounts_read(int nid, uint64_t off, void *buf, size_t len)
```

/proc/mounts

**位置**：`fs/pseudofs/pseudofs.c:244`

<a id="fn-fs-pseudofs-pseudofs.c-255"></a>

### `meminfo_read`

```c
static int meminfo_read(int nid, uint64_t off, void *buf, size_t len)
```

/proc/meminfo — 从 PMM 动态生成

<details>
<summary>函数内备注（4 条）</summary>

**第 268 行**

> MemTotal

**第 273 行**

> MemFree

**第 278 行**

> MemAvailable

**第 282 行**

> Stubs

</details>

**位置**：`fs/pseudofs/pseudofs.c:255`

<a id="fn-fs-pseudofs-pseudofs.c-293"></a>

### `cpuinfo_read`

```c
static int cpuinfo_read(int nid, uint64_t off, void *buf, size_t len)
```

/proc/cpuinfo

**位置**：`fs/pseudofs/pseudofs.c:293`

<a id="fn-fs-pseudofs-pseudofs.c-312"></a>

### `pid_max_read`

```c
static int pid_max_read(int nid, uint64_t off, void *buf, size_t len)
```

/proc/sys/kernel/pid_max

**位置**：`fs/pseudofs/pseudofs.c:312`

<a id="fn-fs-pseudofs-pseudofs.c-342"></a>

### `syscalls_read`

```c
static int syscalls_read(int nid, uint64_t off, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/pseudofs.c:342`

<a id="fn-fs-pseudofs-pseudofs.c-361"></a>

### `backtrace_read`

```c
static int backtrace_read(int nid, uint64_t off, void *buf, size_t len)
```

/proc/backtrace — 当前任务的调用栈(kernel/debug/backtrace.c)。

和 /proc/syscalls 一样不受 LOG= 等级影响, 也不需要重编：怀疑哪里
卡住了, cat 一下就能看到这条路径是怎么调下来的。

最上面几帧必然是 backtrace_read → vfs → syscall 入口那一串 ——
那条路径就是这个任务此刻的内核栈, 不是噪声。往下才是调用者。

**位置**：`fs/pseudofs/pseudofs.c:361`

<a id="fn-fs-pseudofs-pseudofs.c-472"></a>

### `pfs_node_count`

```c
int pfs_node_count(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/pseudofs.c:472`

<a id="fn-fs-pseudofs-pseudofs.c-477"></a>

### `pfs_node_at`

```c
const pseudo_node_t *pfs_node_at(int nid)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/pseudofs.c:477`

<a id="fn-fs-pseudofs-pseudofs.c-489"></a>

### `pfs_node_available`

```c
bool pfs_node_available(const pseudo_node_t *node)
```

可选服务节点是否可见。节点表本身保持编译期固定(nid 就是表下标,
open/read/ioctl 全都直接用它), 只在查找和目录枚举时过滤 —— 这样
移除 #if DRIVER_* 不会改变 nid 的含义。

**位置**：`fs/pseudofs/pseudofs.c:489`

<a id="fn-fs-pseudofs-pseudofs.c-510"></a>

### `find_node`

```c
static int find_node(const char *path)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/pseudofs.c:510`

<a id="fn-fs-pseudofs-pseudofs.c-522"></a>

### `pfs_node_name`

```c
const char *pfs_node_name(const pseudo_node_t *n)
```

节点路径的最后一个分量(即名称)

**位置**：`fs/pseudofs/pseudofs.c:522`

<a id="fn-fs-pseudofs-pseudofs.c-535"></a>

### `pseudo_open`

```c
int pseudo_open(const char *abspath)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（5 条）</summary>

**第 539 行**

> /proc/self/fd/&lt;N&gt; 归并到 /proc/self/fd 目录节点

**第 543 行**

> /proc/&lt;pid&gt;  或  /proc/&lt;pid&gt;/status

**第 547 行（行尾）**

> /proc/&lt;pid&gt;

**第 549 行（行尾）**

> /proc/&lt;pid&gt;/status

**第 551 行（行尾）**

> /proc/&lt;pid&gt;/stat

</details>

**位置**：`fs/pseudofs/pseudofs.c:535`

<a id="fn-fs-pseudofs-pseudofs.c-558"></a>

### `pseudo_read`

```c
int pseudo_read(int nid, uint64_t *off, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（4 条）</summary>

**第 563 行**

> 动态 /proc/&lt;pid&gt;/stat

**第 572 行**

> 动态 /proc/&lt;pid&gt;/status

**第 580 行**

> 动态 /proc/&lt;pid&gt; 目录：无内容可读

**第 588 行（行尾）**

> 空文件

</details>

**位置**：`fs/pseudofs/pseudofs.c:558`

<a id="fn-fs-pseudofs-pseudofs.c-595"></a>

### `pseudo_write`

```c
int pseudo_write(int nid, const void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/pseudofs.c:595`

<a id="fn-fs-pseudofs-pseudofs.c-605"></a>

### `pseudo_ioctl`

```c
int pseudo_ioctl(int nid, uint64_t req, void *argp)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/pseudofs.c:605`

<a id="fn-fs-pseudofs-pseudofs.c-615"></a>

### `pseudo_poll`

```c
uint32_t pseudo_poll(int nid)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 617 行**

> 无 poll_fn 的节点保持老行为：永远可读可写。
> 这与 vfs_poll() 在没有 ops-&gt;poll 时的兜底一致。

</details>

**位置**：`fs/pseudofs/pseudofs.c:615`

<a id="fn-fs-pseudofs-pseudofs.c-628"></a>

### `pseudo_close`

```c
int pseudo_close(int nid)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/pseudofs.c:628`

<a id="fn-fs-pseudofs-pseudofs.c-643"></a>

### `pseudo_ioctl_file`

```c
int pseudo_ioctl_file(void *file, int nid, uint64_t req, void *argp)
```

「按打开的文件」版本的 ioctl / close。见 pseudofs_internal.h 里
ioctl_file_fn 的说明 —— 只有需要 per-open 状态的节点(DRM 渲染节点)
才实现 *_file_fn, 其余节点自动退回上面那对老回调。

<details>
<summary>函数内备注（1 条）</summary>

**第 648 行**

> 同样截 32 位 —— 理由见 kernel/syscall/fs/ioctl.c 里那段说明。
> syscall 层已经截过一次, 这里是防别的调用者直接进来。

</details>

**位置**：`fs/pseudofs/pseudofs.c:643`

<a id="fn-fs-pseudofs-pseudofs.c-658"></a>

### `pseudo_close_file`

```c
int pseudo_close_file(void *file, int nid)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/pseudofs.c:658`

<a id="fn-fs-pseudofs-pseudofs.c-670"></a>

### `pseudo_stat_path`

```c
int pseudo_stat_path(const char *abspath, struct kernel_stat *st)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（2 条）</summary>

**第 674 行**

> /proc/self/fd/&lt;N&gt;

**第 683 行**

> /proc/&lt;pid&gt;  或  /proc/&lt;pid&gt;/status

</details>

**位置**：`fs/pseudofs/pseudofs.c:670`

<a id="fn-fs-pseudofs-pseudofs.c-697"></a>

### `pseudo_fill_stat`

```c
void pseudo_fill_stat(int nid, struct kernel_stat *st)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（2 条）</summary>

**第 703 行（行尾）**

> 虚拟设备号

**第 709 行**

> 目录和符号链接给个合理的 size

</details>

**位置**：`fs/pseudofs/pseudofs.c:697`

<a id="fn-fs-pseudofs-pseudofs.c-716"></a>

### `pseudo_readlink`

```c
int pseudo_readlink(const char *abspath, char *buf, size_t bufsz)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（2 条）</summary>

**第 721 行**

> /proc/self/exe → 当前进程的可执行路径

**第 731 行**

> /proc/self/fd/&lt;N&gt; → 合成路径(仅 0/1/2)

</details>

**位置**：`fs/pseudofs/pseudofs.c:716`

<a id="fn-fs-pseudofs-pseudofs.c-758"></a>

### `pseudo_getdents`

```c
int pseudo_getdents(int nid, uint64_t *off, void *buf, size_t bufsz)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/pseudofs.c:758`

<a id="mod-fs-pseudofs-rtctl_dev.c"></a>

## fs/pseudofs/rtctl_dev.c — /dev/rtctl

`fs/pseudofs/rtctl_dev.c` · 2 个函数 · 0 个有说明 · 133 行

用户态控制面：创建/启动/停止/销毁 RT worker，更新输入、读取输出与统计。
数据面在 kernel/rt/rt_worker.c。ABI 见 include/rt/rt_worker.h。

权限：V1 只允许 root（uid 0）。RT 优先级和模板能力将来再细化为
capability，不在这一版里假装已经做好。

<a id="fn-fs-pseudofs-rtctl_dev.c-26"></a>

### `rtctl_is_root`

```c
static bool rtctl_is_root(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/rtctl_dev.c:26`

<a id="fn-fs-pseudofs-rtctl_dev.c-32"></a>

### `rtctl_dev_ioctl`

```c
int rtctl_dev_ioctl(int nid, uint64_t req, void *argp)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/rtctl_dev.c:32`

<a id="mod-fs-pseudofs-rtworkers.c"></a>

## fs/pseudofs/rtworkers.c — /proc/rtworkers — 用户可配置 RT worker 的快照

`fs/pseudofs/rtworkers.c` · 6 个函数 · 1 个有说明 · 192 行

/dev/rtctl 的 STATS/LIST 需要 ioctl；这个 proc 文件让 shell 直接
`cat /proc/rtworkers`，也给 supervisor 一个无 syscall 的观察面。

列宽是固定的；表头和每一行都走同一组 put_col/put_num_col，改列宽时
只需要改下面的 W_* 宏，避免表头和内容各写一套 padding 迟早对不上。

<a id="fn-fs-pseudofs-rtworkers.c-39"></a>

### `rtw_state_name`

```c
static const char *rtw_state_name(uint32_t s)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/rtworkers.c:39`

<a id="fn-fs-pseudofs-rtworkers.c-55"></a>

### `rtw_tpl_name`

```c
static const char *rtw_tpl_name(uint32_t t)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/rtworkers.c:55`

<a id="fn-fs-pseudofs-rtworkers.c-75"></a>

### `put_col`

```c
static size_t put_col(char *buf, size_t pos, size_t bufsz, const char *str, int width, bool right)
```

str 按 width 对齐；right=true 右对齐（数字），false 左对齐（文本）。
超长时直接原样输出，不截断 —— 宁可某一行错位，也不要悄悄吞掉字段。

**位置**：`fs/pseudofs/rtworkers.c:75`

<a id="fn-fs-pseudofs-rtworkers.c-92"></a>

### `put_num_col`

```c
static size_t put_num_col(char *buf, size_t pos, size_t bufsz, uint64_t v, int width)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/rtworkers.c:92`

<a id="fn-fs-pseudofs-rtworkers.c-99"></a>

### `put_sep`

```c
static size_t put_sep(char *buf, size_t pos, size_t bufsz)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/rtworkers.c:99`

<a id="fn-fs-pseudofs-rtworkers.c-104"></a>

### `rtworkers_read`

```c
int rtworkers_read(int nid, uint64_t off, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/rtworkers.c:104`

<a id="mod-fs-pseudofs-util.c"></a>

## fs/pseudofs/util.c

`fs/pseudofs/util.c` · 6 个函数 · 0 个有说明 · 75 行

<a id="fn-fs-pseudofs-util.c-10"></a>

### `pfs_strlen`

```c
size_t pfs_strlen(const char *s)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/util.c:10`

<a id="fn-fs-pseudofs-util.c-18"></a>

### `pfs_strcmp`

```c
int pfs_strcmp(const char *a, const char *b)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/util.c:18`

<a id="fn-fs-pseudofs-util.c-27"></a>

### `pfs_strncmp`

```c
int pfs_strncmp(const char *a, const char *b, size_t n)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/util.c:27`

<a id="fn-fs-pseudofs-util.c-37"></a>

### `u64_to_dec`

```c
int u64_to_dec(char *buf, uint64_t v)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/util.c:37`

<a id="fn-fs-pseudofs-util.c-57"></a>

### `pfs_puts`

```c
int pfs_puts(char *buf, size_t pos, size_t bufsz, const char *s)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/util.c:57`

<a id="fn-fs-pseudofs-util.c-67"></a>

### `pfs_copy_out`

```c
int pfs_copy_out(uint64_t off, void *buf, size_t len, const char *src, size_t total)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/pseudofs/util.c:67`

<a id="mod-fs-pseudofs-vmm_dev.c"></a>

## fs/pseudofs/vmm_dev.c — /dev/vmm：从宿主 shell 里控制 guest 的字符设备

`fs/pseudofs/vmm_dev.c` · 9 个函数 · 3 个有说明 · 677 行

移植自 x-kernel: virt/kvmm-api/src/device.rs(chardev /dev/kvmm-vm)。

设计意图：宿主 busybox shell 先起来, 用户在 shell 里跑 /bin/vmm-run,
由它打开本设备, 启动 guest 并在 stdin/stdout 与设备之间双向搬字节。
这样宿主 tty 层始终独占真实 UART(signal_check_uart), VMM 不碰硬件；
guest 的输入输出全部经用户态 helper 中转。

协议(对标 kvmm-api 的 read/write/poll/release, 另加了分离/接入)：
open("/dev/vmm", O_RDWR)         打开设备
ioctl(fd, VMM_IOC_BOOT)          启动 guest；已在跑则接入它
ioctl(fd, VMM_IOC_BOOT_EX)       强制新建一个 VM, 出参回填 vmid
ioctl(fd, VMM_IOC_BOOT_CFG)      带参数启动/新建：vCPU 数 + 新建标志,
出参回填实际 vmid / 实际核数(vmm-run -c N)
ioctl(fd, VMM_IOC_LIST)          出参：所有 VM 的 {vmid,state} + 前台 vmid
ioctl(fd, VMM_IOC_ATTACH)        入参 vmid：把它设为前台(不必重新 boot)
ioctl(fd, VMM_IOC_STOP_VM)       入参 vmid：停指定的那个, 不要求先接入
write(fd, bytes, n)              永远是 guest 的串口输入
read(fd, buf, n)                 取 guest 串口输出；无数据返回 -EAGAIN
poll(fd)                         guest 有输出报 EPOLLIN；guest 退出报 EPOLLHUP
ioctl(fd, VMM_IOC_GET_STATUS)    出参 1 = 有 guest 在跑
ioctl(fd, VMM_IOC_DETACH)        分离：本次 close 不停 guest(Ctrl+T d)
ioctl(fd, VMM_IOC_STOP)          停止前台 guest(Ctrl+T k)
close(fd)                        默认停止 guest；分离过则不停止

后三个号(LIST/ATTACH/STOP_VM)是"按 vmid 寻址"那一组：在此之前要停某个
VM 必须先接入它, 而接入路径又只挑最小 vmid —— vm1/vm2 同时跑时你
根本碰不到 vm2。详见 include/fs/pseudofs.h 里那三个号上面的说明。

控制全部走 ioctl, write 永远只是数据 —— 原因见 include/fs/pseudofs.h 里
VMM_IOC_* 上面的那段说明(「首次写是命令」在接入场景下有二义性, 实测
会把 "bootlinux" 当成输入喂给 guest)。这也是与 kvmm 唯一的有意偏离：
kvmm 只有 chardev 没有 ioctl, 只能用「首次写是命令」。

VMM_IOC_BOOT 是「启动或接入」而不是只启动：上一次 Ctrl+T d 留在后台
的 guest 还在跑时, 直接接入它, 不重新加载镜像。于是用户只需要记住一条
命令(vmm-run), 内核按「有没有在跑的 guest」决定新建还是接入。

guest 镜像路径是写死的 /guests/linux 下那三个文件, 不像 kvmm 那样由
参数带进来；将来要支持多 guest 的话, 把参数挂在 BOOT 的 ioctl 出参上。

中断注入不在这里做：RX FIFO 由控制台 vdev 持有, 置 pending 的时机在 VMM
进入 guest 前(电平触发, 见 vmm_console_irq_asserted 的注释)。

<a id="fn-fs-pseudofs-vmm_dev.c-117"></a>

### `vmm_dev_is_owner`

```c
static int vmm_dev_is_owner(void)
```

调用者是不是本次会话的主人(见 g_vmm_owner_pid 的说明)。

**位置**：`fs/pseudofs/vmm_dev.c:117`

<a id="fn-fs-pseudofs-vmm_dev.c-124"></a>

### `vmm_dev_claim`

```c
static void vmm_dev_claim(void)
```

把当前任务登记成会话主人。

**位置**：`fs/pseudofs/vmm_dev.c:124`

<a id="fn-fs-pseudofs-vmm_dev.c-138"></a>

### `vmm_dev_boot_result`

```c
static void vmm_dev_boot_result(uint32_t *out_vmid, uint32_t *out_nr, uint32_t vmid, uint32_t nr_vcpus)
```

出参统一从这里填：调用方要的是本次实际选中/创建的 VM, 而不是
函数返回后再去读 g_vmm_fg_vmid —— 那时前台可能已经被别的 helper
改掉了(多 VM 下这不是理论问题)。

**位置**：`fs/pseudofs/vmm_dev.c:138`

<a id="fn-fs-pseudofs-vmm_dev.c-146"></a>

### `vmm_dev_boot`

```c
static int vmm_dev_boot(int force_new, uint32_t want_vcpus, uint32_t *out_vmid, uint32_t *out_nr)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（17 条）</summary>

**第 148 行**

> 每次会话从这里开始, 先清掉上一次可能残留的「分离」意图。
> 不清的话：分离过一次之后再 attach, 那次会话的 close 又会变成分离,
> guest 就永远停不掉了。

**第 155 行**

> 先挡住明显越界的值, 避免下面 cast 成 int 后失去符号

**第 161 行**

> 已经有一个 guest 在跑(典型场景：上一次 Ctrl+T d 把它留在后台了)
> → 本次会话就当它的前台, 直接接入, 不重新加载镜像。

> 这也是为什么不需要单独的 attach 命令：helper 只管发 VMM_IOC_BOOT,
> 内核按「有没有在跑的 guest」决定是新建还是接入。

**第 171 行**

> 先试「接回上次离开的那个」(screen -r 语义)。

> 这一条是补上的, 也是&quot;从 vm2 分离后再也接不回 vm2&quot;的直接原因：
> 从前这里只按最小 vmid 扫, 于是 vm1/vm2 同时跑时, 从 vm2 分离
> 之后再敲 vmm-run 会接到 vm1 —— g_vmm_fg_vmid 明明还记着 vm2,
> 却没有任何人看它。

> 注意： 只认 VM_RUNNING, 不能像从前那样只判 `!= VM_FREE`：vm_get 连
> LOADING / DYING 的槽位也返回(vm.c:100), 而接到一个正在收尾的
> VM 上, 随后的 write/read 就打进一个马上要被 memset 的槽位 ——
> 表现为&quot;attach 成功了但立刻 HUP 退出&quot;。这正是重启回归里
> 「没等收尾就再敲 vmm-run 只会接入」那个症状的根源。

**第 197 行**

> vmm_guest_running() 为真, 却挑不出一个 RUNNING 的 —— 那些 VM
> 都还在收尾(DYING)。返回 EBUSY 让调用方按&quot;上一个还没收完尾&quot;
> 重试, 而不是接上一个将死的槽位。

**第 206 行**

> 运行中的 VM 改不了核数：调用方显式指定了数量且对不上时必须
> 报错, 不能静默接入一个核数不同的 guest(调用方 -c 隐含新建,
> 正常走不到这里；这是给直接撕 ioctl 的人兜底)。

**第 219 行（行尾）**

> 这个任务从此是本次会话的主人(见 g_vmm_owner_pid)

**第 222 行**

> 通道应当是开着的；分离期间没关过, 这里只是兜底

**第 228 行**

> 没有在跑的 guest, 但上一个可能还没收完尾(vCPU 任务要跑到下一个安全点
> 才退出, 满载时最坏一个宿主 tick ≈ 10ms)。这里不等, 让调用方重试 ——
> 内核里做定时等待要么引 timer 依赖, 要么就是个没上界的自旋循环。

**第 235 行**

> 在建 VM 之前校验, 失败时什么都没动(槽位, 控制台都还没碰),
> 调用方可以安全地把它当成配置错误(EINVAL)而非瞬态错误重试。

**第 244 行**

> 先把控制台切到「通道模式」, 再启动 guest。

> 顺序有讲究：切了通道之后 VMM 就不再碰宿主 UART(vmm_console_pump 会
> 让位给 helper), 而 guest 的启动输出进 TX 缓冲等 helper 来取。反过来
> 先启动的话, guest 的头几行会走 klog 混进宿主日志里。

> 此刻 vCPU 任务还没被调度(guest_loader_run_linux 内部才 task_create,
> 而当前任务不会立刻让出), 所以不存在「输出漏到 klog」的窗口。

**第 254 行**

> 从 VM 池里取一个槽位, 配上镜像布局, 然后交给 loader。

> 从前这里是&quot;全局唯一那个 VM&quot;—— guest_loader 内部一个 static vm_t,
> 于是第二个 VM 必然覆盖第一个。现在是池：每次 boot 都拿一个新槽位。

**第 262 行**

> 没拿到槽位就什么都没动过 —— 控制台归属也还没切, 不用回滚

**第 271 行**

> 先把控制台切到「通道模式」, 再启动 guest。
> 顺序有讲究：切了通道之后 VMM 就不再碰宿主 UART(vmm_console_pump
> 会让位给 helper), guest 的启动输出进本 VM 的 TX 缓冲等 helper 来取。
> 反过来先启动的话, guest 的头几行会走 klog 混进宿主日志里。

**第 279 行**

> guest_loader_run_linux 内部也做了重入保护, 失败会返回负值

**第 284 行（行尾）**

> 槽位不能漏 —— 没人在跑了

**第 289 行（行尾）**

> 这个任务从此是本次会话的主人(见 g_vmm_owner_pid)

</details>

**位置**：`fs/pseudofs/vmm_dev.c:146`

<a id="fn-fs-pseudofs-vmm_dev.c-298"></a>

### `vmm_dev_write`

```c
int vmm_dev_write(int nid, const void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（2 条）</summary>

**第 307 行**

> write() 永远只是 guest 的串口输入 —— 启动/停止/分离全在 ioctl 里。
> 没有单独的「命令」分支, 所以也就不存在「这次的写到底是命令还是数据」
> 这个二义性(见文件头注释)。

**第 314 行（行尾）**

> 没有前台 VM, 写也没意义

</details>

**位置**：`fs/pseudofs/vmm_dev.c:298`

<a id="fn-fs-pseudofs-vmm_dev.c-322"></a>

### `vmm_dev_read`

```c
int vmm_dev_read(int nid, uint64_t off, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 340 行**

> 一个都没有才算空。非阻塞语义：helper 先用 poll() 等可读

</details>

**位置**：`fs/pseudofs/vmm_dev.c:322`

<a id="fn-fs-pseudofs-vmm_dev.c-347"></a>

### `vmm_dev_ioctl`

```c
int vmm_dev_ioctl(int nid, uint64_t req, void *argp)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（14 条）</summary>

**第 356 行（行尾）**

> 有在跑的就接入

**第 356 行（行尾）**

> 构建默认核数

**第 359 行**

> 强制新建一个 VM；出参回填它分到的 vmid(多 VM 的入口)

**第 369 行（行尾）**

> 总是新建

**第 369 行（行尾）**

> 构建默认核数

**第 379 行**

> 带参数启动(运行期 vCPU 数)。flags 只有 NEW；nr_vcpus == 0
> 表示沿用构建默认。出参回填实际 vmid / 实际核数。

**第 417 行**

> 上限取自结构体本身, 别写死——写死的话改了 info[] 容量这里会忘

**第 433 行**

> count 报真实总数(可能大于 cap), info[] 只填了前 cap 个 ——
> 调用方据此知道被截断了。当前 MAX_VMS=32 而 info[] 容量也是 32,
> 正常走不到截断；但池子大小一改, 这段仍然是对的。

> 为什么这里不做原子快照也够用：每次 vm_get 只借池锁查一遍, 拿到
> vm_t* 之后读 vmid/state 是无锁的, 理论上可能读到刚被回收的槽位。
> 对&quot;列个表给人看&quot;来说最坏结果是多一个/少一个条目, 而下游的
> ATTACH 会再校验一次状态, 所以不值得为此引一把新锁。

**第 462 行**

> 区分这两种失败, helper 才好给出一句有用的话：
> ENOENT —— 压根没这个 vmid(多半是列表看旧了)
> EBUSY  —— 有这个 VM, 但还没跑起来(LOADING)或正在收尾(DYING)

**第 476 行**

> 只把新前台打开, 不要去关旧前台的 console_owned。

> 那个标志的语义不是&quot;谁是前台&quot;, 而是&quot;这台 VM 的控制台由 helper
> 接管了&quot;, 而 vmm_console_pump(vcpu-&gt;vm) 是拿每个 VM 自己的标志
> 把关的(vmm_console.c:46)。把旧前台的清掉, 它继续在另一颗核上跑
> 的时候泵就会复活, 去跟宿主 tty 层抢同一个真实 UART 的硬件 FIFO
> (tty.c 的 signal_check_uart 是第一个消费者, 泵是第二个,
> 谁先跑谁拿到 —— 输入会随机丢给错误的一方)。

**第 503 行**

> 停一个指定的 VM —— 这是&quot;想退出 vm 不必先 vmm-run 进去&quot;的关键。
> 不要求它是前台；只有它正好是前台时才顺带把前台清掉。

> 只置 stop_req 就返回：真正的回收由该 VM 的 vCPU 任务在下一个安全点
> 自己做(vm.c 的并发规则：&quot;跨任务只传 vmid&quot;, &quot;销毁由 vCPU 任务自己
> 完成&quot;), 这里不等 —— 等了就要么引 timer 依赖, 要么是个没上界的自旋。

**第 513 行**

> 同样不要清 g_vmm_fg_vmid, 哪怕它正好是前台 —— 理由与
> VMM_IOC_STOP 那段完全相同：清了就等于让 attach 着它的 helper
> 永远收不到 EPOLLHUP, 只能靠用户敲键盘才发现 guest 没了。

**第 532 行**

> Ctrl+T d：把 guest 留在后台。只置标志 —— 真正的交接在 close 里,
> 那里会跳过「停止 guest」和「关 TX 通道」两件事。

> 注意： 必须限定 owner：g_vmm_detached 是会话级的全局标志, 而
> close 会消费它。要是让一个无关进程(比如 `vmm-run -k`)也能
> 置位, 它就能把 owner 的 close 变成分离 —— 用户明确要求停掉的
> guest 会被留在后台继续烧 CPU。

**第 551 行**

> 只停前台那个 VM —— 别的 VM(挂起或后台跑的)不受影响。

> 注意： 停完不要清 g_vmm_fg_vmid。清了之后 vmm_dev_poll 就再也
> 报不出 EPOLLHUP(它的判据是 fg_vmid != 0, 见那边), 正在 attach
> 这个 VM 的 helper 会无声地卡在 poll 里 —— 写回 EIO, 读回
> EAGAIN, 它却收不到任何事件, 只能等用户碰一下键盘才退出。
> 留着它, HUP 会把&quot;这个 VM 没了&quot;如实告诉 helper。
> (下一句 vm_get 会拿到一个 DYING/FREE 的槽位, poll 据此报 HUP。)

</details>

**位置**：`fs/pseudofs/vmm_dev.c:347`

<a id="fn-fs-pseudofs-vmm_dev.c-570"></a>

### `vmm_dev_poll`

```c
uint32_t vmm_dev_poll(int nid)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（3 条）</summary>

**第 577 行（行尾）**

> 永远收得下写

**第 583 行**

> guest 已经退出(比如 guest 自己 poweroff)：告诉 helper 别再等了。
> 未启动时不算 HUP —— 那时候 helper 正要发 bootlinux。

**第 586 行**

> 前台 VM 已经收尾(比如 guest 自己 poweroff)才报 HUP。
> 未启动时(fg==0)不算 —— 那时候 helper 正要发 BOOT。

</details>

**位置**：`fs/pseudofs/vmm_dev.c:570`

<a id="fn-fs-pseudofs-vmm_dev.c-600"></a>

### `vmm_dev_close`

```c
int vmm_dev_close(int nid)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（5 条）</summary>

**第 607 行**

> 不是本会话的主人就什么都不做 —— 见 g_vmm_owner_pid 的说明。
> 这一条挡的是「随便 open/close 一下就停掉别人的 guest」：`vmm-run -l`
> 是个纯查询, 它从没 BOOT 过, 必须让它走到这里就返回。

**第 616 行（行尾）**

> 本次会话到此结束

**第 618 行**

> Ctrl+T d 分离：guest 留在后台继续跑, 这里什么都不收拾。

> 特别是不能关 TX 通道 —— 关了 vmm_console_pump() 就会复活, 跟宿主
> shell 抢真实 UART；也不能清 RX FIFO —— 那里面的字节是给当前这个
> guest 的, 重新 attach 时还要用。

> 注意 console_owned 的语义是「这台 VM 的控制台归 helper」而不是
> 「谁是前台」, 所以分离时把它留着是对的。detach 与 close 在这件事上
> 必须一致, 否则后台那台 VM 的泵会去抢宿主 UART, 把用户按键推进一台
> 他看不见的 guest。

**第 636 行**

> 注意： fg 必须在 g_vmm_fg_vmid 清零之前取好。

> 从前的写法是把下面那两句清理放在清零之后, 并重新
> `vm_get(g_vmm_fg_vmid)` —— 而那时它已经是 0, vm_get(0) 恒为 NULL
> (vmid 从 1 起发放), 于是 `if (fg)` 从不成立, 那两句是死代码。
> 一直没爆是因为 VM 随后就被销毁, vm_free 的 memset 和下一次 boot 时
> vpl011_init / uart16550_init 的 memset 顺带把这些状态清了。

**第 653 行**

> 把控制台打回「直打宿主控制台」模式, 并清掉 RX FIFO 里的残留按键。
> 不清的话, 下一次 helper 启动 guest 时, 上一轮没取走的输入会先喂给
> 新 guest 的 getty；tx_set_enabled(0) 还会顺带丢弃 TX 环里没人再读的
> 残留输出(见 uart16550_tx_set_enabled)。

> 效果其实是余量：close 不停的那个 VM 马上就要被销毁, vm_free 和
> 下一次 *_init 的 memset 本来就会清掉这些状态。留着的意义是别让&quot;关了
> 控制台&quot;这件事依赖&quot;反正后面会 memset&quot;。

> 注意： 顺序：必须放在 vm_request_stop 之前。
> stop 只置标志, vCPU 任务可能立刻就在另一颗核上跑到 vm_free 把槽位
> memset 掉(guest 空闲时每条 WFI 都陷入 EL2, 窗口一点都不窄)。
> vpl011_of / uart16550_of 里那道 `d-&gt;st.owner != vm` 守卫只能挡住
> &quot;槽位已释放&quot;, 挡不住&quot;槽位释放后又被 vm_alloc 复用&quot;—— 复用时间址相同,
> owner 被重新设成同一个指针, 守卫会放行, 于是去关掉另一台 VM 的
> 控制台并清空它的 TX 环。先收拾就完全没有这个窗口：此刻它还活着。

</details>

**位置**：`fs/pseudofs/vmm_dev.c:600`

<a id="mod-fs-vfs-fat_vfs.c"></a>

## fs/vfs/fat_vfs.c — FatFs 的 VFS 胶水

`fs/vfs/fat_vfs.c` · 30 个函数 · 5 个有说明 · 856 行

设计要点：
- 绝对路径 /boot/foo 先转成 FatFs 的卷内路径 /foo（fat_relpath）。
- include/fs/vfs.h 不引入 ff.h；FIL/DIR 放在 kmalloc 出来的 fat_handle_t 里，
vfs_file_t 只存一个 void *。
- 所有 FatFs 调用都在 fs_global_lock() 内；和 lwext4 共用同一把锁。
- file->offset 是权威的逻辑偏移。FatFs 的 f_lseek 在越过 EOF 时会扩展
文件（且扩展区内容未定义），所以 seek 只改 offset；真正读写时再懒同步，
越过 EOF 的写由 fat_zero_fill() 补零。

<a id="fn-fs-vfs-fat_vfs.c-49"></a>

### `fat_errno`

```c
static int fat_errno(FRESULT fr)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/fat_vfs.c:49`

<a id="fn-fs-vfs-fat_vfs.c-81"></a>

### `fat_errno_write`

```c
static int fat_errno_write(FRESULT fr)
```

写路径上 FatFs 用 FR_DENIED 表示“磁盘满”；普通路径上它更像 EACCES。

**位置**：`fs/vfs/fat_vfs.c:81`

<a id="fn-fs-vfs-fat_vfs.c-91"></a>

### `fat_relpath`

```c
static int fat_relpath(const char *mp, const char *path, char *out, size_t outsz)
```

/boot      -> /
/boot/a    -> /a
/bootx     -> EINVAL（VFS 的前缀匹配保证不会发生，这里兜底）

<details>
<summary>函数内备注（1 条）</summary>

**第 98 行**

> 不允许把 FAT 挂在 /，但真发生了也按整路径处理

</details>

**位置**：`fs/vfs/fat_vfs.c:91`

<a id="fn-fs-vfs-fat_vfs.c-120"></a>

### `fat_ino_pair`

```c
static uint64_t fat_ino_pair(const char *dir, const char *name)
```

stat(rel) 用 fat_ino_pair(rel, "")；getdents 用 fat_ino_pair(h->rel, name)。
两者必须得到同一个值，所以根目录 "/" 后面不再补一个 '/'。

**位置**：`fs/vfs/fat_vfs.c:120`

<a id="fn-fs-vfs-fat_vfs.c-140"></a>

### `fat_is_leap`

```c
static bool fat_is_leap(int y)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/fat_vfs.c:140`

<a id="fn-fs-vfs-fat_vfs.c-145"></a>

### `fat_dos_to_unix`

```c
static int64_t fat_dos_to_unix(WORD fdate, WORD ftime)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/fat_vfs.c:145`

<a id="fn-fs-vfs-fat_vfs.c-172"></a>

### `fat_stat_locked`

```c
static int fat_stat_locked(const char *rel, FILINFO *fno)
```

调用方持有 fs_global_lock

**位置**：`fs/vfs/fat_vfs.c:172`

<a id="fn-fs-vfs-fat_vfs.c-178"></a>

### `fat_fill_stat`

```c
static int fat_fill_stat(const char *rel, struct kernel_stat *st)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 187 行**

> FatFs 的 f_stat(&quot;/&quot;) 返回 FR_INVALID_NAME（NS_NONAME 是“卷根本身”，
> f_stat 不接受）。卷根必须在这里合成，否则 ls /boot 会直接 ENOENT。

</details>

**位置**：`fs/vfs/fat_vfs.c:178`

<a id="fn-fs-vfs-fat_vfs.c-214"></a>

### `fat_zero_fill`

```c
static int fat_zero_fill(FIL *fp, uint64_t from, uint64_t to)
```

调用方持有 fs_global_lock；把 [from, to) 写成零（用于越过 EOF 的写/扩展）

**位置**：`fs/vfs/fat_vfs.c:214`

<a id="fn-fs-vfs-fat_vfs.c-243"></a>

### `fat_clamp_len`

```c
static UINT fat_clamp_len(size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/fat_vfs.c:243`

<a id="fn-fs-vfs-fat_vfs.c-250"></a>

### `fat_file_read`

```c
static int fat_file_read(vfs_file_t *file, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/fat_vfs.c:250`

<a id="fn-fs-vfs-fat_vfs.c-285"></a>

### `fat_write_common`

```c
static int fat_write_common(vfs_file_t *file, const void *buf, size_t len, bool honor_append)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/fat_vfs.c:285`

<a id="fn-fs-vfs-fat_vfs.c-330"></a>

### `fat_file_write`

```c
static int fat_file_write(vfs_file_t *file, const void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/fat_vfs.c:330`

<a id="fn-fs-vfs-fat_vfs.c-335"></a>

### `fat_file_seek`

```c
static int fat_file_seek(vfs_file_t *file, int64_t offset, int whence, uint64_t *new_off)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 361 行**

> 关键：不要在这里 f_lseek —— FatFs 会立即扩展文件。

</details>

**位置**：`fs/vfs/fat_vfs.c:335`

<a id="fn-fs-vfs-fat_vfs.c-367"></a>

### `fat_file_pread`

```c
static int fat_file_pread(vfs_file_t *file, void *buf, size_t len, int64_t offset)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/fat_vfs.c:367`

<a id="fn-fs-vfs-fat_vfs.c-379"></a>

### `fat_file_pwrite`

```c
static int fat_file_pwrite(vfs_file_t *file, const void *buf, size_t len, int64_t offset)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 386 行**

> pwrite 不受 O_APPEND 影响（不临时改 file-&gt;flags，避免 SMP 下的数据竞争）

</details>

**位置**：`fs/vfs/fat_vfs.c:379`

<a id="fn-fs-vfs-fat_vfs.c-392"></a>

### `fat_file_truncate`

```c
static int fat_file_truncate(vfs_file_t *file, uint64_t length)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/fat_vfs.c:392`

<a id="fn-fs-vfs-fat_vfs.c-423"></a>

### `fat_file_sync`

```c
static int fat_file_sync(vfs_file_t *file)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/fat_vfs.c:423`

<a id="fn-fs-vfs-fat_vfs.c-435"></a>

### `fat_file_stat`

```c
static int fat_file_stat(vfs_file_t *file, struct kernel_stat *st)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/fat_vfs.c:435`

<a id="fn-fs-vfs-fat_vfs.c-443"></a>

### `fat_file_close`

```c
static int fat_file_close(vfs_file_t *file)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/fat_vfs.c:443`

<a id="fn-fs-vfs-fat_vfs.c-479"></a>

### `fat_name_copy`

```c
static void fat_name_copy(char *dst, const char *src, size_t dstsz)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/fat_vfs.c:479`

<a id="fn-fs-vfs-fat_vfs.c-491"></a>

### `fat_dir_getdents`

```c
static int fat_dir_getdents(vfs_file_t *file, void *buf, size_t bufsz)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（4 条）</summary>

**第 517 行（行尾）**

> 目录结束

**第 519 行（行尾）**

> 卷标不是目录项

**第 531 行**

> 放不下：存起来，下一次先吐它，避免 f_readdir 游标丢失条目

**第 536 行**

> 缓冲区连一条记录都放不下

</details>

**位置**：`fs/vfs/fat_vfs.c:491`

<a id="fn-fs-vfs-fat_vfs.c-560"></a>

### `fat_dir_stat`

```c
static int fat_dir_stat(vfs_file_t *file, struct kernel_stat *st)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/fat_vfs.c:560`

<a id="fn-fs-vfs-fat_vfs.c-568"></a>

### `fat_dir_close`

```c
static int fat_dir_close(vfs_file_t *file)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/fat_vfs.c:568`

<a id="fn-fs-vfs-fat_vfs.c-581"></a>

### `fat_mount_open`

```c
static int fat_mount_open(const vfs_mount_t *mnt, const char *path, int flags, int mode, vfs_file_t **out)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（3 条）</summary>

**第 617 行**

> O_TRUNC 不能像 FatFs 那样顺手创建不存在的文件。
> f_stat 到 f_open 之间必须一直持锁，否则目标可能被换掉。

**第 668 行**

> 文件方式打不开：再按目录试一次（和 ext4_mount_open 的行为一致）

**第 693 行**

> 目录打不开：用 f_stat 区分“是个文件”与“不存在”

</details>

**位置**：`fs/vfs/fat_vfs.c:581`

<a id="fn-fs-vfs-fat_vfs.c-708"></a>

### `fat_mount_stat`

```c
static int fat_mount_stat(const vfs_mount_t *mnt, const char *path, struct kernel_stat *st)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/fat_vfs.c:708`

<a id="fn-fs-vfs-fat_vfs.c-717"></a>

### `fat_mount_readlink`

```c
static int fat_mount_readlink(const vfs_mount_t *mnt, const char *path, char *buf, size_t bufsz)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 723 行**

> FAT 没有符号链接；返回 EINVAL 让 follow_symlinks() 停止

</details>

**位置**：`fs/vfs/fat_vfs.c:717`

<a id="fn-fs-vfs-fat_vfs.c-727"></a>

### `fat_mount_unlink`

```c
static int fat_mount_unlink(const vfs_mount_t *mnt, const char *path, bool is_dir)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 755 行**

> 非空目录也会返回 FR_DENIED，探一下区分 ENOTEMPTY 与 EACCES

</details>

**位置**：`fs/vfs/fat_vfs.c:727`

<a id="fn-fs-vfs-fat_vfs.c-774"></a>

### `fat_mount_mkdir`

```c
static int fat_mount_mkdir(const vfs_mount_t *mnt, const char *path, int mode)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/fat_vfs.c:774`

<a id="fn-fs-vfs-fat_vfs.c-788"></a>

### `fat_mount_rename`

```c
static int fat_mount_rename(const vfs_mount_t *mnt, const char *oldpath, const char *newpath)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 819 行**

> POSIX rename 覆盖目标；FatFs 的 f_rename 不覆盖

</details>

**位置**：`fs/vfs/fat_vfs.c:788`

<a id="mod-fs-vfs-vfs.c"></a>

## fs/vfs/vfs.c — small VFS dispatch layer

`fs/vfs/vfs.c` · 78 个函数 · 4 个有说明 · 1119 行

<a id="fn-fs-vfs-vfs.c-65"></a>

### `mount_path_matches`

```c
static bool mount_path_matches(const char *mnt, const char *path)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:65`

<a id="fn-fs-vfs-vfs.c-76"></a>

### `vfs_find_mount`

```c
static const vfs_mount_t *vfs_find_mount(const char *path)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:76`

<a id="fn-fs-vfs-vfs.c-92"></a>

### `vfs_copy_path`

```c
void vfs_copy_path(vfs_file_t *file, const char *path)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:92`

<a id="fn-fs-vfs-vfs.c-102"></a>

### `vfs_alloc_file`

```c
vfs_file_t *vfs_alloc_file(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:102`

<a id="fn-fs-vfs-vfs.c-118"></a>

### `vfs_register_fat_mount`

```c
int vfs_register_fat_mount(const char *path)
```

vfs_register_fat_mount - 把一个已成功 f_mount 的 FAT 卷接到 VFS 前缀表

只在 fs_init() 阶段调用（此时还没有任务，不需要锁）。path 必须指向
静态存储，且不能与现有挂载点重复。

**位置**：`fs/vfs/vfs.c:118`

<a id="fn-fs-vfs-vfs.c-138"></a>

### `ext4_open_flags_from_linux`

```c
static int ext4_open_flags_from_linux(int flags)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:138`

<a id="fn-fs-vfs-vfs.c-164"></a>

### `pseudo_file_read`

```c
static int pseudo_file_read(vfs_file_t *file, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:164`

<a id="fn-fs-vfs-vfs.c-169"></a>

### `pseudo_file_write`

```c
static int pseudo_file_write(vfs_file_t *file, const void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:169`

<a id="fn-fs-vfs-vfs.c-174"></a>

### `pseudo_file_ioctl`

```c
static int pseudo_file_ioctl(vfs_file_t *file, uint64_t req, void *argp)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 176 行**

> 带 file —— DRM 渲染节点的 GEM handle 要按打开的文件隔离

</details>

**位置**：`fs/vfs/vfs.c:174`

<a id="fn-fs-vfs-vfs.c-180"></a>

### `pseudo_file_poll`

```c
static uint32_t pseudo_file_poll(vfs_file_t *file, int pool_idx)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:180`

<a id="fn-fs-vfs-vfs.c-186"></a>

### `pseudo_file_close`

```c
static int pseudo_file_close(vfs_file_t *file)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:186`

<a id="fn-fs-vfs-vfs.c-191"></a>

### `pseudo_file_getdents`

```c
static int pseudo_file_getdents(vfs_file_t *file, void *buf, size_t bufsz)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:191`

<a id="fn-fs-vfs-vfs.c-196"></a>

### `pseudo_file_seek`

```c
static int pseudo_file_seek(vfs_file_t *file, int64_t offset, int whence, uint64_t *new_off)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:196`

<a id="fn-fs-vfs-vfs.c-220"></a>

### `pseudo_file_stat`

```c
static int pseudo_file_stat(vfs_file_t *file, struct kernel_stat *st)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:220`

<a id="fn-fs-vfs-vfs.c-226"></a>

### `ext4_file_read`

```c
static int ext4_file_read(vfs_file_t *file, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 228 行**

> lseek 允许把 fpos 放到 EOF 之后；ext4_fread 会做 fsize - fpos
> (无符号下溢)，所以先截住。

</details>

**位置**：`fs/vfs/vfs.c:226`

<a id="fn-fs-vfs-vfs.c-246"></a>

### `ext4_file_write`

```c
static int ext4_file_write(vfs_file_t *file, const void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:246`

<a id="fn-fs-vfs-vfs.c-283"></a>

### `ext4_file_seek`

```c
static int ext4_file_seek(vfs_file_t *file, int64_t offset, int whence, uint64_t *new_off)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:283`

<a id="fn-fs-vfs-vfs.c-310"></a>

### `ext4_file_stat`

```c
static int ext4_file_stat(vfs_file_t *file, struct kernel_stat *st)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:310`

<a id="fn-fs-vfs-vfs.c-315"></a>

### `ext4_file_truncate`

```c
static int ext4_file_truncate(vfs_file_t *file, uint64_t length)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:315`

<a id="fn-fs-vfs-vfs.c-326"></a>

### `ext4_zero_fill`

```c
static int ext4_zero_fill(ext4_file *f, uint64_t to)
```

lwext4 的 ext4_fseek(SEEK_SET) 拒绝 offset > fsize（EINVAL），所以
pwrite 到 EOF 之后不能直接 seek。这里先按 POSIX 语义把空洞补零：
从当前 EOF 起 fwrite 零块，直到 offset，再 fseek 到目标位置。

**位置**：`fs/vfs/vfs.c:326`

<a id="fn-fs-vfs-vfs.c-346"></a>

### `ext4_file_pread`

```c
static int ext4_file_pread(vfs_file_t *file, void *buf, size_t len, int64_t offset)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 356 行（行尾）**

> POSIX: 从 EOF 之后读取返回 0

</details>

**位置**：`fs/vfs/vfs.c:346`

<a id="fn-fs-vfs-vfs.c-372"></a>

### `ext4_file_pwrite`

```c
static int ext4_file_pwrite(vfs_file_t *file, const void *buf, size_t len, int64_t offset)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 392 行**

> pread/pwrite 不改变文件位置；只有原位置仍在新 EOF 之内才能恢复

</details>

**位置**：`fs/vfs/vfs.c:372`

<a id="fn-fs-vfs-vfs.c-402"></a>

### `ext4_file_sync`

```c
static int ext4_file_sync(vfs_file_t *file)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 405 行**

> fsync 在 ext4 上仍是空操作（现状），FatFs 才真正 f_sync

</details>

**位置**：`fs/vfs/vfs.c:402`

<a id="fn-fs-vfs-vfs.c-409"></a>

### `ext4_regular_close`

```c
static int ext4_regular_close(vfs_file_t *file)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:409`

<a id="fn-fs-vfs-vfs.c-414"></a>

### `ext4_dir_getdents`

```c
static int ext4_dir_getdents(vfs_file_t *file, void *buf, size_t bufsz)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 419 行**

> 整个迭代圈进一个临界区：ext4_dir_entry_next 把条目拷进 ext4_dir
> (内联在本对象里)并返回它的地址, 循环里要一直拿着它用。只靠 --wrap
> 那层&quot;每次调用&quot;的锁不够 —— 两次调用之间另一个核可以对同一个目录对象
> 做操作。锁是可重入的, 里面的 __wrap_* 照常嵌套。

</details>

**位置**：`fs/vfs/vfs.c:414`

<a id="fn-fs-vfs-vfs.c-466"></a>

### `ext4_dir_stat_file`

```c
static int ext4_dir_stat_file(vfs_file_t *file, struct kernel_stat *st)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:466`

<a id="fn-fs-vfs-vfs.c-471"></a>

### `ext4_dir_file_close`

```c
static int ext4_dir_file_close(vfs_file_t *file)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:471`

<a id="fn-fs-vfs-vfs.c-476"></a>

### `pipe_file_read`

```c
static int pipe_file_read(vfs_file_t *file, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:476`

<a id="fn-fs-vfs-vfs.c-483"></a>

### `pipe_file_write`

```c
static int pipe_file_write(vfs_file_t *file, const void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:483`

<a id="fn-fs-vfs-vfs.c-490"></a>

### `pipe_file_close`

```c
static int pipe_file_close(vfs_file_t *file)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:490`

<a id="fn-fs-vfs-vfs.c-496"></a>

### `pipe_file_poll`

```c
static uint32_t pipe_file_poll(vfs_file_t *file, int pool_idx)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:496`

<a id="fn-fs-vfs-vfs.c-510"></a>

### `pty_file_read`

```c
static int pty_file_read(vfs_file_t *file, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:510`

<a id="fn-fs-vfs-vfs.c-518"></a>

### `pty_file_write`

```c
static int pty_file_write(vfs_file_t *file, const void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:518`

<a id="fn-fs-vfs-vfs.c-526"></a>

### `pty_file_ioctl`

```c
static int pty_file_ioctl(vfs_file_t *file, uint64_t req, void *argp)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:526`

<a id="fn-fs-vfs-vfs.c-531"></a>

### `pty_file_stat`

```c
static int pty_file_stat(vfs_file_t *file, struct kernel_stat *st)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:531`

<a id="fn-fs-vfs-vfs.c-543"></a>

### `pty_file_poll`

```c
static uint32_t pty_file_poll(vfs_file_t *file, int pool_idx)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:543`

<a id="fn-fs-vfs-vfs.c-559"></a>

### `pty_file_close`

```c
static int pty_file_close(vfs_file_t *file)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:559`

<a id="fn-fs-vfs-vfs.c-568"></a>

### `socket_file_read`

```c
static int socket_file_read(vfs_file_t *file, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:568`

<a id="fn-fs-vfs-vfs.c-574"></a>

### `socket_file_write`

```c
static int socket_file_write(vfs_file_t *file, const void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:574`

<a id="fn-fs-vfs-vfs.c-580"></a>

### `socket_file_close`

```c
static int socket_file_close(vfs_file_t *file)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:580`

<a id="fn-fs-vfs-vfs.c-585"></a>

### `socket_file_poll`

```c
static uint32_t socket_file_poll(vfs_file_t *file, int pool_idx)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:585`

<a id="fn-fs-vfs-vfs.c-649"></a>

### `pseudo_mount_open`

```c
static int pseudo_mount_open(const vfs_mount_t *mnt, const char *path, int flags, int mode, vfs_file_t **out)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:649`

<a id="fn-fs-vfs-vfs.c-669"></a>

### `ext4_mount_open`

```c
static int ext4_mount_open(const vfs_mount_t *mnt, const char *path, int flags, int mode, vfs_file_t **out)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（2 条）</summary>

**第 675 行**

> O_CREAT 的 mode 只在新文件上生效；先记一下是否已存在

**第 684 行**

> 新建文件的 mode 一定要落盘 —— 包括 mode=0（umask 0777 时
> umask01 会检查 0000 的权限位）。以前只有 (mode &amp; 07777) 非零
> 才调用 ext4_mode_set，mode 0 会保留 lwext4 默认的 0666。

</details>

**位置**：`fs/vfs/vfs.c:669`

<a id="fn-fs-vfs-vfs.c-730"></a>

### `vfs_open`

```c
int vfs_open(const char *path, int flags, int mode, vfs_file_t **out)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:730`

<a id="fn-fs-vfs-vfs.c-742"></a>

### `vfs_create_pipe_file`

```c
int vfs_create_pipe_file(int pipe_idx, bool is_write_end, int flags, vfs_file_t **out)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:742`

<a id="fn-fs-vfs-vfs.c-759"></a>

### `vfs_create_pty_file`

```c
int vfs_create_pty_file(int pty_idx, bool is_master, int flags, const char *path, vfs_file_t **out)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:759`

<a id="fn-fs-vfs-vfs.c-777"></a>

### `vfs_create_socket_file`

```c
int vfs_create_socket_file(int sock_idx, int flags, vfs_file_t **out)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:777`

<a id="fn-fs-vfs-vfs.c-800"></a>

### `vfs_ref`

```c
void vfs_ref(vfs_file_t *file)
```

refcnt 必须原子：fork / dup 之后同一个 vfs_file_t 会被多个任务同时持有,
两边的 ref++ / ref-- 撞在一起就是丢更新 —— 计数偏小的一方会提前 kfree,
另一方接着用的是已经还给 kmalloc 的对象("文件内容看起来是乱码"这类
症状)。用 sub_fetch 的返回值判断归零, 顺带把"读 refcnt 再减"那个窗口
去掉了。

**位置**：`fs/vfs/vfs.c:800`

<a id="fn-fs-vfs-vfs.c-806"></a>

### `vfs_close`

```c
int vfs_close(vfs_file_t *file)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:806`

<a id="fn-fs-vfs-vfs.c-819"></a>

### `vfs_discard_unopened`

```c
void vfs_discard_unopened(vfs_file_t *file)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:819`

<a id="fn-fs-vfs-vfs.c-825"></a>

### `vfs_read`

```c
int vfs_read(vfs_file_t *file, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:825`

<a id="fn-fs-vfs-vfs.c-832"></a>

### `vfs_write`

```c
int vfs_write(vfs_file_t *file, const void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:832`

<a id="fn-fs-vfs-vfs.c-839"></a>

### `vfs_ioctl`

```c
int vfs_ioctl(vfs_file_t *file, uint64_t req, void *argp)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:839`

<a id="fn-fs-vfs-vfs.c-846"></a>

### `vfs_getdents`

```c
int vfs_getdents(vfs_file_t *file, void *buf, size_t bufsz)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:846`

<a id="fn-fs-vfs-vfs.c-853"></a>

### `vfs_seek`

```c
int vfs_seek(vfs_file_t *file, int64_t offset, int whence, uint64_t *new_off)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:853`

<a id="fn-fs-vfs-vfs.c-860"></a>

### `vfs_stat_file`

```c
int vfs_stat_file(vfs_file_t *file, struct kernel_stat *st)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:860`

<a id="fn-fs-vfs-vfs.c-867"></a>

### `vfs_poll`

```c
uint32_t vfs_poll(vfs_file_t *file, int pool_idx)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:867`

<a id="fn-fs-vfs-vfs.c-876"></a>

### `vfs_pread`

```c
int vfs_pread(vfs_file_t *file, void *buf, size_t len, int64_t offset)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:876`

<a id="fn-fs-vfs-vfs.c-887"></a>

### `vfs_pwrite`

```c
int vfs_pwrite(vfs_file_t *file, const void *buf, size_t len, int64_t offset)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:887`

<a id="fn-fs-vfs-vfs.c-898"></a>

### `vfs_truncate`

```c
int vfs_truncate(vfs_file_t *file, uint64_t length)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:898`

<a id="fn-fs-vfs-vfs.c-905"></a>

### `vfs_sync`

```c
int vfs_sync(vfs_file_t *file)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:905`

<a id="fn-fs-vfs-vfs.c-914"></a>

### `vfs_socket_index`

```c
int vfs_socket_index(vfs_file_t *file)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:914`

<a id="fn-fs-vfs-vfs.c-921"></a>

### `vfs_file_matches_socket`

```c
bool vfs_file_matches_socket(vfs_file_t *file, int sock_idx)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:921`

<a id="fn-fs-vfs-vfs.c-926"></a>

### `vfs_file_matches_pty`

```c
bool vfs_file_matches_pty(vfs_file_t *file, int pty_idx, bool is_master)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:926`

<a id="fn-fs-vfs-vfs.c-931"></a>

### `vfs_proc_fd_target`

```c
int vfs_proc_fd_target(vfs_file_t *file, char *buf, size_t bufsz)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:931`

<a id="fn-fs-vfs-vfs.c-978"></a>

### `vfs_read_to_phys`

```c
int vfs_read_to_phys(vfs_file_t *file, uint64_t offset, void *buf, size_t len)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:978`

<a id="fn-fs-vfs-vfs.c-983"></a>

### `pseudo_mount_stat`

```c
static int pseudo_mount_stat(const vfs_mount_t *mnt, const char *path, struct kernel_stat *st)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:983`

<a id="fn-fs-vfs-vfs.c-989"></a>

### `ext4_mount_stat`

```c
static int ext4_mount_stat(const vfs_mount_t *mnt, const char *path, struct kernel_stat *st)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:989`

<a id="fn-fs-vfs-vfs.c-995"></a>

### `ext4_mount_unlink`

```c
static int ext4_mount_unlink(const vfs_mount_t *mnt, const char *path, bool is_dir)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:995`

<a id="fn-fs-vfs-vfs.c-1004"></a>

### `ext4_mount_mkdir`

```c
static int ext4_mount_mkdir(const vfs_mount_t *mnt, const char *path, int mode)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:1004`

<a id="fn-fs-vfs-vfs.c-1014"></a>

### `ext4_mount_rename`

```c
static int ext4_mount_rename(const vfs_mount_t *mnt, const char *oldpath, const char *newpath)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:1014`

<a id="fn-fs-vfs-vfs.c-1023"></a>

### `vfs_stat_path`

```c
int vfs_stat_path(const char *path, struct kernel_stat *st)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:1023`

<a id="fn-fs-vfs-vfs.c-1034"></a>

### `vfs_mkdir`

```c
int vfs_mkdir(const char *path, int mode)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:1034`

<a id="fn-fs-vfs-vfs.c-1047"></a>

### `vfs_unlink`

```c
int vfs_unlink(const char *path, bool is_dir)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:1047`

<a id="fn-fs-vfs-vfs.c-1060"></a>

### `vfs_rename`

```c
int vfs_rename(const char *oldpath, const char *newpath)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:1060`

<a id="fn-fs-vfs-vfs.c-1076"></a>

### `pseudo_mount_readlink`

```c
static int pseudo_mount_readlink(const vfs_mount_t *mnt, const char *path, char *buf, size_t bufsz)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:1076`

<a id="fn-fs-vfs-vfs.c-1094"></a>

### `ext4_mount_readlink`

```c
static int ext4_mount_readlink(const vfs_mount_t *mnt, const char *path, char *buf, size_t bufsz)
```

ext4 的 readlink。原先是 `return -VFS_ENOENT;` 的桩, 于是根文件系统上
任何 readlink 都失败 —— 症状是 `ls` 能列目录(只读目录项), 而
`ls -l` / `ls -ls` 报 "cannot read link: No such file or directory"
(长格式要 readlink 才能显示 `linuxrc -> xxx`)。

实现在 lwext4 里是现成的, path.c 的 follow_symlinks() 一直在用,
只是没接到 VFS 这一层。注意 ext4_readlink() 内部以 EXT4_DE_SYMLINK
打开, 不会跟随最后一级链接 —— 这正是 readlink 要的语义。

返回值：成功时是写入 buf 的字节数(>0), 失败时是负 errno。

**位置**：`fs/vfs/vfs.c:1094`

<a id="fn-fs-vfs-vfs.c-1110"></a>

### `vfs_readlink`

```c
int vfs_readlink(const char *path, char *buf, size_t bufsz)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`fs/vfs/vfs.c:1110`
