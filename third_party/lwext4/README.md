# lwext4 (Avatar OS 内嵌副本)

本目录**不再是 git submodule**，而是直接由本仓库维护的源码副本。
之所以改成内嵌：我们 fork 并改动了 lwext4，而 `.gitmodules` 指向的却是上游
URL —— 父仓库钉住的却是 fork 的 commit，导致全新 clone 必然失败
（`did not contain <sha>. Direct fetching of that commit failed`）。
详见 `docs/` 与本仓库的提交历史。

## 来源

| | |
|---|---|
| 上游 | https://github.com/gkostka/lwext4 |
| 基线 commit | `58bcf89` "Documentation improvements (#65)" |
| 我们的 fork | https://github.com/pengzechen/lwext4 @ `f1333c9`（领先基线 9 个 commit） |

本目录 = 该 fork 的 **`src/` + `include/` + `LICENSE`**。
上游的 `fs_test/`、`toolchain/`、`blockdev/` 以及各类构建文件
（`Makefile`/`CMakeLists.txt`/`fs_test.mk`/`_config.yml`/`CHANGELOG`/
`.travis.yml`/`.clang-format`/`.gitignore`/`README.md`）已删除 ——
本内核的构建（`Makefile` 的 `LWEXT4_SRCS := $(wildcard src/*.c)`）不引用它们。

> ⚠️ 因此**上游的构建体系在本副本里不可用**。要跑 lwext4 自带的测试，
> 请用上面 fork 的完整仓库。

## 许可：本目录现在是纯 BSD-3-Clause

lwext4 上游是**混合许可**，其中两个文件是 GPLv2，按上游 README 的说法
「Some of the source files are licensed under GPLv2. **It makes whole lwext4
GPLv2 licensed.**」那会造成整个内核被 GPL 传染。因此我们处理掉了这两个文件：

| 文件 | 处理 |
|---|---|
| `src/ext4_xattr.c` | **整块移除**。本内核不暴露 xattr API；`fs/lwext4_port/generated/ext4_config.h` 里 `CONFIG_XATTR_ENABLE` 置 0，`src/ext4.c` 里 4 个 xattr wrapper 用 `#if CONFIG_XATTR_ENABLE` 守卫。 |
| `src/ext4_extent.c` | **干净室重写**。按公开的 on-disk 格式规范独立实现，见文件头的完整声明。 |

**校验方法**（换机器/换人接手时先跑一遍）：

```bash
cd third_party/lwext4 && grep -rl "GNU General Public License" src/ include/
# 应无输出
```

*注*：`LICENSE` 仍是上游的原文（GPL-2.0 全文 + "部分文件另有许可"的说明），
保留它是因为 BSD-3-Clause 要求随附版权与许可声明，且它是这段历史的凭据。
剩余每个文件的**文件头**里都带完整的 BSD-3-Clause 声明。

## ⚠️ 已知风险 / 未验证的点

按「风险从高到低」排。**接手改动这个目录前请先读这一节。**

### 1. `ext4_extent.c` 的深树路径未经验证（最高风险）

`src/ext4_extent.c` 是重写的（约 620 行），**只验证过一条路径**：

- ✅ 读路径（树遍历 + 叶子内二分）
- ✅ 简单 create（顺序写，与前一 extent 合并成一条）
- ❌ **多级分裂**（叶子满 → 分裂 → 索引项上推 → 父节点满 → 继续上推）
- ❌ **根长高**（根满 → 整根下沉 → 根变成 depth+1 的索引）

后两条只在「单个文件产生 >4 个非连续 extent 且树需要长高」时触发，
普通的 LTP 与 e2fsck 跑**覆盖不到**。

**已做过的验证**（截至重写时的最后一轮）：
- x86_64 引导 + 加载 busybox：✅
- `LTP` 全套：**63/63 PASS，0 内核异常** ✅
- 对 guest 写过的镜像跑 `e2fsck -fn`：**exit 0（干净）** ✅
  —— 这是独立裁判（e2fsprogs 是另一个实现），extent 树/块分配/inode 计数
  写错它一定会报，比 LTP 绿不绿有说服力

**还没做的验证**（要真正覆盖深树路径，必须补上）：
写一个 shell 脚本放进 rootfs（**不要塞进 QEMU 的命令行** —— UART 输入缓冲
会截断长命令行，症状是命令被回显但从不执行），**交错增长多个文件**以逼出
碎片与非连续 extent，然后追加 / 截断 / 删除，最后把镜像拿回宿主跑 `e2fsck`。
纯顺序追加只会得到一条长 extent，触发不了分裂。

### 2. `ext4_extent_remove_space` 不回收空节点（有意取舍）

叶子被清空后保留一个 `entries == 0` 的空节点，树**不回缩**。
回缩要递归改父节点、出错就是数据损坏，而本内核的使用场景（rootfs +
少量临时文件）几乎不会累积到需要回缩。代价是浪费若干个块，**以及**：
如果将来有人做「反复大文件写-删」的压力测试，会看到空闲块被慢慢吃掉。

### 3. uninitialized extent 只读不写

`extent_is_uninit()` / `extent_len()` 已按规范的 bit15 规则正确处理
（注意 `ee_len == 32768` 是**已初始化**的 32768，不是未初始化的 0），
但创建路径**不会生成**未初始化 extent。对 rootfs 场景够用；
如果有工具依赖 fallocate 式的预分配，那块会退化成分配真实块。

### 4. 校验尾只算非根节点

`metadata_csum` 打开时，非根 extent 节点的末 4 字节是 crc32c；
根内嵌在 inode 的 `i_block[15]` 里、由 inode 自身的校验和覆盖，
本实现据此跳过。**如果这个理解有误，症状会是 e2fsck 报根节点校验错** ——
届时优先查这里。

### 5. `CONFIG_XATTR_ENABLE = 0` 是硬性的

`fs/lwext4_port/generated/ext4_config.h` 里置 0。改成 1 会链接失败
（`src/ext4_xattr.c` 已删）。要恢复 xattr 得把那个文件从 fork 取回来，
但那样 GPL 就回来了。

### 6. 构建状态：不要并行跑 `make kernel` 和 `make rootfs`

两个 target 都会重编内核（`rootfs` 依赖 `$(ROOTFS_IMG)`，而后者依赖内核产物），
交错执行会把 `build/<platform>/` 搞成中间状态，症状是链接期报
`cannot find <某个>.o`。单独重跑一次 `make kernel` 即可恢复 —— 不是代码问题。

## 移植胶水在别处

本目录只有上游源码。项目自己的适配在 **`fs/lwext4_port/`**：

- `fs_lock.c` —— 用 `-Wl,--wrap` 把 27 个 lwext4 API 全局串行化
  （lwext4 是单线程用户态库，块缓存的红黑树没有任何内部同步）
- `kmalloc.c` —— lwext4 的 `ext4_user_malloc/free` 实现（512 KiB 堆）
- `fs_init.c` —— 挂载
- `generated/ext4_config.h` —— 本项目的配置（上面提到的 `CONFIG_XATTR_ENABLE` 等）
