# FAT 双分区挂载（MBR，ext4 root + FAT /boot）

本文说明 Avatar 如何同时挂载 **p1 FAT + p2 ext4** 的 MBR 磁盘：
ext4 挂 `/`，FAT 可写挂到 `/boot`（编译期可改）。实现入口是
`kernel/fs/init.c` 的 `fs_init()`。

---

## 1. 布局与识别

`fs_init()` 自己解析扇区 0（不再用 lwext4 的 `ext4_mbr_scan()`——它只保留
0x83，FAT 分区会被丢掉）：

| 类型 | 含义 | 去向 |
|---|---|---|
| `0x83` | Linux native (ext4) | 第一个挂 `/` |
| `0x01/0x04/0x06/0x0B/0x0C/0x0E/0xEF` | FAT12/16/32、EFI ESP | 第一个挂 `FAT_MOUNT_POINT` |

判据刻意收紧：必须有 **至少一条类型已知且 LBA/长度在设备范围内** 的分区才算
MBR。裸 FAT 镜像的引导扇区同样有 `0x55AA`，只看签名会误判。

没有 MBR 时：

- `ramblk` 平台（QEMU、RK3588）按旧行为把整盘当 ext4；
- `sdblk` 平台（`block_root_needs_mbr()` 为真）报错，因为它的根就在分区 2。

不支持 GPT（保护性 MBR 0xEE 会打日志）、扩展分区（0x05/0x0F）和逻辑分区；
只支持 4 个主分区。

---

## 2. 挂载点

默认 `/boot`，编译期可改：

```sh
make PLATFORM=qemu-virt-x86_64 FAT_MOUNT_POINT=/mnt/fat kernel
```

- 必须是绝对路径、不能是 `/`、不能以 `/` 结尾、不能位于 `/dev`、`/proc`、`/sys` 下。
- 它进了 `_CFG_SIG`，**改完必须 `make clean`**（头文件依赖不被跟踪，见 CLAUDE.md）。
- `/boot` 会遮蔽 ext4 根上的同名目录；ext4 里的 `/boot` 只用于让 `ls /` 看得到挂载点。
  老镜像没有这个目录也能 `ls /boot`（VFS 是前缀路由），只是 `ls /` 里看不到它；
  `make rootfs` 现在会自动 `mkdir -p $(ROOTFS_STAGE)$(FAT_MOUNT_POINT)`。
- `/proc/mounts` 仍是硬编码字符串（不含 FAT），`statfs` 也仍是全局假数据；
  `df /boot` 的数字不可信。

---

## 3. 能做什么

`ls /boot`、`cat`、`cp`、`mv`、`rm`、`mkdir`、`touch`、`echo > /boot/file`
都可用。VFS 把命名空间操作也接到了挂载分发：

- `mkdir` → `f_mkdir`；`rm`/`rmdir` → `f_unlink`（非空目录返回 `ENOTEMPTY`）；
- `rename` → 同一挂载内 `f_rename`；跨挂载（`mv /boot/x /tmp/`）返回 `EXDEV`，
  busybox 会退化成 copy+unlink；
- `open` 支持 `O_CREAT/O_EXCL/O_TRUNC/O_APPEND/O_DIRECTORY`；
- `fsync/fdatasync` → `f_sync`；文件关闭（`f_close`）也会 flush。

**不支持**：

- 从 FAT 执行 ELF/bin（`elf_loader.c` / `bin_loader.c` 仍只走 ext4）；
- 符号链接、权限位、硬链接（FAT 没有这些概念；`chmod` 仍是空操作）；
- 非 ASCII 文件名（`FF_LFN_UNICODE=0` + CP437，启动文件用 ASCII 即可）；
- 单文件 > 4 GiB（`FF_LBA64=0`，FAT32 上限）；
- 非 512 字节扇区的 FAT 卷；
- exFAT。

`sync()` 系统调用仍是空操作；要确保落盘，请关闭文件或调用 `fsync`。

---

## 4. 持久化：哪个平台真的写进卡里

| 平台 | 块设备 | 写入后果 |
|---|---|---|
| **SG2002 / SD 卡** | `sdblk`（直接 PIO 读写 SDHCI） | **真正持久化**，重启后还在 |
| QEMU | `ramblk`（`-device loader` 把镜像装进 RAM） | 只改内存副本；重启 QEMU 丢失 |
| RK3588 | `ramblk`（U-Boot 把 rootfs 搬进内存） | 只改内存副本；重启丢失 |

RK3588 要持久化，需要另做 eMMC/SD 块驱动并让 rootfs/FAT 都从卡上读——
那是独立的一项工作，不在当前 FAT 支持范围内。

---

## 5. 构建与测试

### 5.1 QEMU 自动回归（推荐）

生成 p1 FAT + p2 ext4 的 MBR 测试镜像并跑完整读写回归：

```sh
make PLATFORM=qemu-virt-x86_64 ROOTFS_MBR=1 LOG=info test-fat
make PLATFORM=qemu-virt-riscv64 ROOTFS_MBR=1 LOG=info test-fat   # TCG，较慢
make PLATFORM=qemu-virt-aarch64 ROOTFS_MBR=1 LOG=info test-fat   # TCG，较慢
```

脚本做这些事：

1. `tools/make_mbr_image.sh` 生成 `build/<p>/rootfs-mbr-<arch>.img`
   （p1 FAT，p2 ext4；`FAT_PART_MB` 默认 16，<33 MiB 用 FAT16，否则 FAT32）。
   改 `FAT_PART_MB` 后要先删掉已有的 `rootfs-mbr-*.img`（make 不跟踪这个
   变量的值，镜像不会自动过期）；
2. `tools/fat_regress.sh` 启动 QEMU，等 shell 提示符，然后断言：
   `ls /boot`、读写、`>>` 追加、`mv`（同挂载）、跨挂载 `mv`、`mkdir`、`cp`、
   `ls -l`、`rm`、`rm -rf`，最后再读一次 ext4 的 `/etc/passwd`。
3. 同时检查启动日志里 FAT/ext4 挂载成功、无 `panic`/mount 失败。

想手工进 shell：

```sh
make PLATFORM=qemu-virt-x86_64 ROOTFS_MBR=1 LOG=info run-fs-mbr
```

**注意**：`ROOTFS_MBR=1` 的镜像名是 `rootfs-mbr-<arch>.img`，默认镜像仍是
裸 ext4（`rootfs-<arch>.img`），两者不会互相覆盖。但内核变体标志
（`GUEST_LINUX=` 等）仍按 CLAUDE.md 的同变体顺序处理。

### 5.2 SG2002 实机

SD 卡布局（U-Boot 从 p1 读 kernel/rootfs，内核从 p2 挂根）：

```
p1  FAT   (0x0c/0x0b)   U-Boot: kernel_riscv64.bin / rootfs.img
p2  ext4  (0x83)        内核 rootfs
```

启动后：

```sh
ls /boot
cp /etc/passwd /boot/test.txt
cat /boot/test.txt
# 重启（或断电再上电），在 PC / U-Boot 侧确认 p1 里的 test.txt 还在
```

这是唯一能验证“跨重启持久化”的路径。

---

## 6. 实现位置

| 文件 | 作用 |
|---|---|
| `kernel/fs/init.c` | 文件系统总入口：自己解析 MBR；挂 ext4 `/`，再 `fatfs_mount()` + `vfs_register_fat_mount()` |
| `fs/fatfs_port/diskio.c` | FatFs 的 disk I/O（直接走共享 `bdif->bread/bwrite`）和挂载 |
| `fs/fatfs_port/ffconf.h` | FatFs R0.15 配置（`FFCONF_DEF=80286`） |
| `kernel/fs/vfs/fat_vfs.c` | 路径翻译、open/read/write/seek/truncate/getdents/stat/命名空间操作 |
| `kernel/fs/vfs/vfs.c` | 动态 mount 表、命名空间分发、pread/pwrite/sync 分发 |
| `include/vfs.h` + `kernel/fs/vfs/vfs_internal.h` | FAT 文件类型/不透明 handle、内部常量 |
| `include/fs_lock.h` + `fs/lwext4_port/fs_lock.c` | lwext4 与 FatFs 共用的全局可重入锁 |
| `third_party/fatfs/` | FatFs R0.15 上游源码（唯一 vendor patch 是 `ffconf.h` wrapper） |

### 锁与块设备共享

FAT 分区和 ext4 分区共享同一个 `bdif`（MBR 分区只是不同的 `part_offset`）。
FAT 的 diskio 只做 **512 字节对齐的整扇区** I/O，不碰 lwext4 的 `ph_bbuf`；
两个文件系统的所有 API 调用由 `fs_global_lock()` 串行化。FatFs 不是线程安全
的，这个锁不能省。

### 一个顺带修的 sdblk bug

`driver/blk/sdblk.c:sdblk_bdev_open()` 原来无条件把 `bdev->part_size` 覆盖成
整盘容量。挂分区时 `ext4_block_init()` 会调用它，导致分区的 `part_size`
错误；FAT 的 `GET_SECTOR_COUNT`/边界检查会因此变成整盘。现在只有在
`part_size == 0` 或明显越界时才回填整盘容量。

---

## 7. 常见现象

- **`ls /boot: No such file or directory`**：FAT 没挂上，或挂载点没注册。
  查启动日志里的 `FAT volume mounted at ...` / `FAT partition mounted at ...`；
  没有的话看 `FAT mount ... failed: FRESULT=N`。
- **写了但重启后没有**：在 QEMU/RK3588 的 `ramblk` 上这是预期行为；见 §4。
- **改了 `FAT_MOUNT_POINT` 没生效**：忘了 `make clean`（它进了 `_CFG_SIG`）。
- **FAT 卷不是 512B 扇区**：当前直接拒绝（`FF_MIN_SS == FF_MAX_SS == 512`）。
