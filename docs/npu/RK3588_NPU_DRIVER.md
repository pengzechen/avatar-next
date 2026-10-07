# RK3588 NPU 驱动完整支持方案

> **目标**：让**未经修改的官方 `librknnrt.so`** 在本内核上跑起来。
> **参考**：`github.com/bmilde/rknpu-driver-dkms` 里的 `rknpu-0.9.8`（Rockchip 官方
> vendor 驱动，从 `rknn-llm` 拷出）。注意该 DKMS 包 README 自述
> **"WIP! The driver does not compile currently."** —— 它是**移植素材**，不是能跑的基线。
> 本文所有 ABI 事实都实测自 `librknnrt.so` 二进制（见 §3），不是照抄头文件。

---

## 0. 结论摘要

> ✅ **2026-10 已在 RK3588 真机跑通**：**未修改的官方 `librknnrt.so`** 加载
> MobileNetV1 模型并完成推理，`start.elapsed() = 2.57 ms`，分类结果正确
> （top-1 65.6%）。无 IOMMU、全连续内存。复现见 §5 的 P2/P3/P4 实现记录。

三句话：

1. **难点不在 NPU**。NPU 自己的逻辑（寄存器序列、作业、中断）约 1000 行，是整件事里
   最小的一块，而且我们已经有寄存器定义和中断桩。
2. **真正的门槛是三个本内核完全没有的子系统**：`dma-buf`、`dma-heap`、
   以及一个 **DRM 渲染节点形状的 ioctl 多路复用器**。官方 librknnrt 的每个缓冲区
   都从 `/dev/dma_heap/*` 分配、以 dmabuf fd 在用户态流转、再经 `PRIME_FD_TO_HANDLE`
   交给驱动 —— 这条路绕不过去。
3. **好消息是 ABI 面很小**：全部算下来约 **16 个 ioctl**（§3 表），且
   librknnrt 会通过 `RKNPU_GET_IOMMU_EN` **主动询问**驱动有没有 IOMMU 再决定策略 ——
   所以「无 IOMMU、全连续内存」是官方支持的配置，不是权宜之计。

> 📌 **真机上踩的坑几乎都不在 NPU 逻辑里**，而是三类：
> (a) **只在本内核成立的前提**（MMIO 要走 VMA、物理地址唯一的出口）；
> (b) **移植时被"参考实现"掩盖的 vendor 行为**（任务按核切片 `subcore_task[]`）；
> (c) **从来没执行过的通路**（QEMU 上 `npu.base0` 为 0，整条提交路径之前从没跑过）。
> 每一类都能让日志看起来像硬件坏了。三条都在 §5 有记录。

---

## 1. 现状盘点（我们有什么）

| 能力 | 现状 | 位置 |
|---|---|---|
| NPU 电源域 | ✅ 上电四个域（PD_NPU / NPUTOP / NPU1 / NPU2），**只有 power-on，没有 power-off** | `driver/npu/rkpm.c` |
| NPU MMIO / 寄存器定义 | ✅ 基址、中断号、寄存器偏移、packed task 描述符 | `driver/npu/rknpu.h:31-61` |
| 版本校验 | ✅ 读魔数 `0x46495245`（"FIRE"） | `driver/npu/rknpu.c:90` |
| 中断 | ⚠️ 只读 `INT_STATUS`、清 `INT_CLEAR`、**不跟踪作业** | `driver/npu/rknpu.c:60-80` |
| 用户态接口 | ⚠️ `/dev/npu` 节点存在但是**桩**，直接返回 `-PFS_ENOSYS` | `pseudofs.c:365`、`dev.c:264` |
| 连续物理内存 | ✅ `pmm_alloc_pages` / `kalloc_pages` / `kmalloc` | `include/pmm.h:69`、`include/kmalloc.h` |
| "DMA 缓冲"雏形 | ⚠️ ION 分配器 + `FDT_ION` fd，**已有逐页 mmap 进用户态的路径** | `driver/ion/ion.c`、`mmap.c:174-245` |
| 缓存维护 | ✅ `clean/invalidate/clean_and_invalidate_dcache_range` | `include/cache.h:105-167` |
| 内核线程 | ✅ `task_create(name, entry, arg, prio)` | `task/task.h:257` |
| 阻塞/唤醒 | ✅ 自旋锁、睡眠 mutex、裸 `list_t` + `task_block/task_unblock` | `task/mutex.h`、`task/task.h:371` |
| **IOMMU / SMMU** | ❌ **完全没有**（全仓库零命中） | — |
| **dma-buf / dma-heap** | ❌ 没有 | — |
| **DRM（任何形式）** | ❌ 没有 | — |
| **时钟框架 / 复位控制器** | ❌ 完全没有；NPU CRU 未碰 | — |
| **下半部 / workqueue** | ❌ 没有；ISR 里不能睡眠 | `task/preempt.h:9` |

设备模型是**硬编码初始化**（`kernel/main.c:94` 的 `platform_init_runtime_drivers()`，
按 `#if DRIVER_*` 调用），平台参数来自 `platforms/rk3588-aarch64/platform.conf`
经 `tools/gen_platform.py` 生成的 KV 表 —— **没有设备树解析、没有 probe/bind**。
本方案沿用这个惯例，不引入驱动框架。

---

## 2. 目标与它带来的硬约束

`librknnrt.so`（实测版本 `2.3.2 (429f97ae6b@2025-04-09)`）的初始化路径要求：

```
open("/dev/dma_heap/...")  →  DMA_HEAP_IOCTL_ALLOC   ← 分配
        ↓ 拿到 dmabuf fd
DMA_BUF_IOCTL_SYNC                                   ← 用户态驱动缓存维护
        ↓
open("/dev/dri/renderD*")  →  PRIME_FD_TO_HANDLE     ← dmabuf 换成 GEM handle
        ↓
DRM_IOCTL_RKNPU_MEM_MAP / MEM_SYNC                   ← 映射 / 同步
        ↓
DRM_IOCTL_RKNPU_SUBMIT                               ← 提交作业
```

**顺带它还会读**（缺失时行为待实测，见 §7 风险 R6）：
`/sys/class/drm/%s/device/config`、`/sys/class/devfreq/dmc/cur_freq`、
`/sys/kernel/debug/rknpu/freq`、`/sys/kernel/debug/clk/*/clk_rate`。
这些是**带宽/DVFS 估算**用的，属于「没有也能跑，但跑得不对」的一类。

---

## 3. 实测 ABI 面（反汇编 `librknnrt.so` 得出）

方法：`objdump -d` 全库，定位 104 个 `bl ioctl@plt` 调用点，回溯 `movz`/`movk`
组装出 `w1` 的值，再用 **size 字段与已知结构体大小交叉验证**。
未被还原的调用点（约 40%）多数是间接调用或寄存器搬运，**所以下表是下界而非全集** ——
但这没关系，§8 的验证策略本来就要求 shim 打印未知 ioctl，以实测补全。

### 3.1 DRM 路径（`/dev/dri/renderD*`，magic `'d'`）

| 请求号 | 名称 | 说明 |
|---|---|---|
| `0xc0406400` | `DRM_IOCTL_VERSION` | size=64 = `sizeof(struct drm_version)` ✓ 交叉验证通过 |
| `0x80046402` | `DRM_IOCTL_GET_MAGIC` | `_IOR('d',0x02,4)` |
| `0x0000641e` / `0x0000641f` | `SET_MASTER` / `DROP_MASTER` | `_IO('d',..)`，无参数；调用点带 EINTR/EAGAIN 重试循环 |
| `0x00006430` / `0x00006431` | `AGP_ACQUIRE` / `AGP_RELEASE` | 老 DRM 探测惯例，现代驱动返回 `-EINVAL` 即可 |
| `0xc008640a` | `DRM_IOCTL_GEM_FLINK` | size=8 = `{handle,name}` |
| `0xc00c642d` / `0xc00c642e` | `PRIME_HANDLE_TO_FD` / `FD_TO_HANDLE` | size=12 = `struct drm_prime_handle` ✓ |
| `0xc0086440` | `DRM_IOCTL_RKNPU_ACTION` | size=8 |
| `0xc0686441` | `DRM_IOCTL_RKNPU_SUBMIT` | size=104 = `sizeof(struct rknpu_submit)` ✓ |
| `0xc0306442` | `DRM_IOCTL_RKNPU_MEM_CREATE` | size=48 |
| `0xc0106443` | `DRM_IOCTL_RKNPU_MEM_MAP` | size=16 |
| `0xc0106444` | `DRM_IOCTL_RKNPU_MEM_DESTROY` | size=16 |
| `0xc0206445` | `DRM_IOCTL_RKNPU_MEM_SYNC` | size=32 |

另有若干 `'d'` 号（`0xc0046427/28`、`0xc0086421`、`0xc0206416/17`、`0xc0106418/26`、
`0xc028641b`、`0xc0186419`、`0x80386433`、`0x40046411`、`0x40086424`、`0x40106436`）
还原出来但**尚未逐一确诊**，可能是 libdrm 风格的辅助调用或扫描噪声。
**shim 先按「未知 → 打日志 + 返回 `-EINVAL`」处理**，上板后按日志定夺。

### 3.2 misc 路径（`/dev/rknpu`，magic `'r'`）

| 请求号 | 名称 | 说明 |
|---|---|---|
| `0xc0087200` | `IOCTL_RKNPU_ACTION` | size=8 |
| `0xc0687201` | `IOCTL_RKNPU_SUBMIT` | size=104 |
| `0xc0287202` | `IOCTL_RKNPU_MEM_CREATE` | **size=40，不是 48** ⚠️ |
| `0xc0107204` | `IOCTL_RKNPU_MEM_DESTROY` | size=16 |

> ⚠️ **misc 路径的 `MEM_CREATE` 是 40 字节，DRM 路径是 48 字节** —— 两个路径的
> 结构体布局**不同**（40 字节对应 `sram_size` 为 `__u32` 的旧版布局：
> `handle(4)+flags(4)+size(8)+obj_addr(8)+dma_addr(8)+sram_size(4)+iommu_domain_id(4)`）。
> 照 `include/rknpu_ioctl.h` 里那个 48 字节的结构体去解 misc 路径，**会错位**。
> 这条只有从二进制里才看得出来，头文件对不上。

### 3.3 其它设备

| 请求号 | 名称 | 说明 |
|---|---|---|
| `0xc0184800` | `DMA_HEAP_IOCTL_ALLOC` | size=24 = `struct dma_heap_allocation_data` ✓ |
| `0x40086200` | `DMA_BUF_IOCTL_SYNC` | size=8 = `struct dma_buf_sync` ✓ **缓存维护的唯一入口** |
| `0x40106202` | dma-buf nr=2 size=16 | fence 导出/导入，待确诊 |

**sysfs / debugfs 读取**（见 §2 末尾）。

---

## 4. 架构

分四层，**自下而上实现**，每层都能独立验收：

```
┌──────────────────────────────────────────────────────────────┐
│ L4  DRM 兼容 shim        /dev/dri/renderD128  (pseudofs 节点) │
│      ioctl 多路复用：'d' 编码 → GEM / PRIME / RKNPU            │
├──────────────────────────────────────────────────────────────┤
│ L3  NPU 作业引擎                                               │
│      提交 · 队列 · 中断完成 · 超时软复位                        │
├──────────────────────────────────────────────────────────────┤
│ L2  GEM 对象层                                                 │
│      handle 表 · dmabuf ↔ handle · 逐页 mmap                   │
├──────────────────────────────────────────────────────────────┤
│ L1  dma-buf + dma-heap                                         │
│      fd 化的缓冲区 · /dev/dma_heap/cma · 缓存维护               │
├──────────────────────────────────────────────────────────────┤
│ L0  物理内存：启动时预留的 CMA 连续区（platform.conf 参数）      │
└──────────────────────────────────────────────────────────────┘
```

**关键设计决定：设备地址抽象只留一个出口。**
参考驱动里 `dma_addr` 有两个来源 —— 无 IOMMU 时 `sg_dma_address(sgt) == 物理地址`
（`rknpu_gem.c:261`），有 IOMMU 时是 `iova_start`（`:561`）。我们**现在只实现前者**，
但把它收敛到唯一一个函数：

```c
/* 缓冲区 → 交给 NPU 的设备地址。将来上 IOMMU 时只改这一个函数。 */
static inline uint64_t npu_dev_addr(npu_buf_t *b);
```

所有写进 task 描述符和 `PC_DATA_ADDR` / `PC_DMA_BASE_ADDR` 的值都必须经它。
**这是整个方案里最省事、也最容易被绕过的一条纪律。**

---

## 5. 分阶段实施

### P0 — dma-buf + dma-heap（**必须先做，且与 NPU 无关**）✅ 已完成

> **状态**：aarch64 已实测通过 —— `tools/dmatest_p0.sh aarch64` 报
> `5 通过 0 失败`。构建开关 `DMA=1`（`NPU=rknpu` 时自动开）。
>
> ⚠️ 实现过程中挖出一个**既有的、跨架构的**内存安全 bug（设备缓冲区映射进
> 用户态后会被 munmap / 进程退出错误释放）。**动 mmap / 页表释放路径之前
> 必读** `docs/bugfix/DEVICE_BUFFER_MMAP_OWNERSHIP.md` —— 那里列了**必须
> 同时改的 7 处**，只改一条会漏掉 fork 场景。
>
> riscv64 / x86_64 只做到"能编译"，未跑过用例（与 §9 未决问题 3 相关）。

* `include/dma_buf.h` + `driver/dma/dma_buf.c`
  - `struct dma_buf { size, pa/kva, file, refcount, ops }`
  - fd 化：复用现有 fd 池机制，**把 `FDT_ION` 泛化成 `FDT_DMABUF`**
  - 实现 `DMA_BUF_IOCTL_SYNC`（`0x40086200`）→ 直接调 `clean_and_invalidate_dcache_range`
* `driver/dma/dma_heap.c` + `/dev/dma_heap/cma` 节点
  - `DMA_HEAP_IOCTL_ALLOC`（`0xc0184800`）→ 从 L0 预留区分配，返回 dmabuf fd
* `sys_mmap` 泛化：现在 `mmap.c:174-245` 只对 `FDT_ION` 逐页映射，
  改成「fd → ops → mmap 回调」。**`vfs_file_ops_t` 目前没有 mmap 成员，要加。**

**验收**：用户态 `open("/dev/dma_heap/cma")` → alloc → 写数据 →
`DMA_BUF_IOCTL_SYNC` → `mmap` 读回一致。**不碰 NPU 就能测。**

### P1 — CMA 预留区

* `platform.conf` 加 `npu.cma_base` / `npu.cma_size`（建议 512 MB 起，
  真实模型权重+中间张量可能上百 MB）
* boot 时从 PMM 中 `pmm_free_pages` 掉这块，交给独立的连续分配器

> **为什么不能直接用 `pmm_alloc_pages`**：它是单一全局位图的 next-fit 扫描
> （`kernel/mm/pmm.c:68-110`），系统跑一会儿就碎片化，一个几十 MB 的连续请求
> 会随机失败 —— 而且是**模型加载到一半才失败**，很难查。
> 预留区保证「能分配就一定连续」。

**验收**：反复 alloc/free 数百次不泄漏；分配大块不会因碎片失败。

### P2 — GEM 对象层 ✅ 已完成（aarch64 实测）

* handle 表（每 fd 一份，随 close 回收）
* `PRIME_FD_TO_HANDLE` / `PRIME_HANDLE_TO_FD` ↔ dma-buf
* `GEM_FLINK`（实现为 handle→全局名映射，或用不到就返回 `-EINVAL`）
* `MEM_CREATE` / `MEM_MAP` / `MEM_DESTROY` / `MEM_SYNC`
  - `MEM_SYNC` 语义参考 `rknpu_gem.c:1684`：**非 cacheable 的缓冲区直接 `-EINVAL`**
* 逐页 mmap（复用 P0 的回调）

**验收**：用户态能 alloc → map → 读写出正确内容。仍不需要 NPU 出结果。

### P3 — NPU 作业引擎 ✅ 已实现（**移植自真机验证过的代码**）

* task 数组校验（`rknpu_task_t` 已是 packed，见 `rknpu.h:51-61`）
* 提交序列（§6.1）
* 中断完成（§6.2）
* 作业队列：每核一个待跑队列，`spinlock` + `list_t`
* 超时 + 软复位（参考 `rknpu_job.c:699` 的 `rknpu_job_timeout_clean`）

**中断上下文约束**：ISR 里**不能睡眠**（`task.c:901` 有 `!in_irq_context()` 断言）。
所以 ISR 只做「置完成标志 + `list_del` + `task_unblock`」，返回用户态的重活留给
提交者自己（它阻塞在 ioctl 里，是进程上下文，可以睡）。
**没有 workqueue，需要延后执行就自己 `task_create` 一个内核线程。**

**验收**：跑通一次最小 submit（哪怕只有一个空 task），中断能回、作业能完成。

### P4 — DRM 兼容 shim ✅ 已完成（aarch64 实测）

* pseudofs 新节点 `/dev/dri/renderD128`（渲染节点**不需要 master**，
  `SET_MASTER` 直接返回 0 即可）
* ioctl 分发：先解 `_IOC` 的 `type/nr/size` 三元组，再分派
* **未知 ioctl 一律打日志 + 返回 `-EINVAL`** —— 这是把 §3 的表补全的手段
* 顺带加 `/dev/rknpu` misc 路径（同一个 handler，只换解码 + 注意 §3.2 的 40 字节差异）

**验收**：`librknnrt` 能 `open` + 初始化到能提交作业；日志里没有未实现的 ioctl。

---

### P2/P3/P4 实现记录

代码：`include/rknpu_drm.h`（ABI，每个号都钉了 `_Static_assert`）、
`driver/npu/rknpu_drm.c`（GEM + ioctl）、`driver/npu/rknpu.c`（作业引擎）、
验收程序 `apps/c/drmtest.c` + `tools/drmtest_p2p4.sh`。

**验收结果**：`19 通过 0 失败`（QEMU 上；SUBMIT 回 -ENODEV 是预期，
因为 QEMU 没有 NPU —— GEM/PRIME/MEM_* 根本不碰硬件，所以内存路径可验）。

#### 两个实现期踩到的坑（都不是 NPU 特有的）

**① ioctl 请求号被符号扩展**（`kernel/syscall/fs/ioctl.c`）
musl 的声明是 `int ioctl(int, int, ...)`（glibc 是 `unsigned long`），于是
`0xc0406400` 这种最高位为 1 的号在用户态是**负数**，进内核时变成
`0xffffffffc0406400`，和 32 位的 `_IOC` 常量永远比不上 —— 症状是"所有
带高位 magic 的 ioctl 全部走 default 分支"，而 glibc 编的程序完全正常。
Linux 内核的 `SYSCALL_DEFINE3(ioctl, ..., unsigned int, cmd, ...)` 取的就是
32 位，我们要照做。`pseudo_ioctl` 里本来就有 `(uint32_t)req` —— 说明这个坑
以前被踩过，只是当时只补了一条路径。

**② MEM_MAP 的假偏移必须页对齐**
mmap 的 ABI 要求 offset 页对齐，musl 的 `mmap()` 包装在**进内核之前**就会
`if (off & (PAGE_SIZE-1)) { errno = EINVAL; }`。写成 `BASE + id` 时 id=2 得到
`0x10000000002`，用户态连内核都没进就 EINVAL 了。改成 `BASE + (id << 12)`。
DRM 真实的 `drm_vma_offset_manager` 同样按页走。

#### ★ 真正让 librknnrt 连上的三条（QEMU 上联调实测）

这三条是"设备根本不认"和"一路跑到 SUBMIT"的分界，**全部只有跑起来才看得见**：

**① 它扫的是 `card0..card15`，不是 `renderD*`。**
文档 §3 从字符串表推出 `renderD`，于是最初只挂了 `/dev/dri/renderD128`
—— 结果 librknnrt **从头到尾没 open 过它**。内核 syscall trace 抓到的实际
顺序是：

```
/proc/self/maps   →   /dev/rknpu   →   /dev/dri/card0 … card15   →   /proc/dri/0/name … 7/name
```

所以 `card0` 是主路径（`renderD128` 一并留着，成本只是一行）。

**② DRM_IOCTL_VERSION 是"两阶段"协议，长度必须回真实长度。**
libdrm 的 `drmGetVersion()` 先传 `name=NULL/name_len=0` **只为问长度**，拿到
之后再 malloc 缓冲、第二次调用取串。我最初回的是"这次实际写入的长度"
（`name==NULL` 时就是 0），于是 libdrm 不分配，调用方拿到 `name=NULL`
还去 `strstr` —— **空指针 SIGSEGV**。

症状极具误导性：日志里 `DRM_IOCTL_VERSION` 连打两次**都返回 0**，紧接着
进程在自己代码里崩（`pc` 在 exe 的映射区间内），内核侧一切正常。
`GET_UNIQUE`（`0xc0106401`）是同一套协议，同理。

**③ 驱动名必须是 `rknpu`。**
DRM 节点上报的名字我最初起的是 `rknpu-avatar` —— librknnrt 按名字做身份
匹配，不认。`DRM_IOCTL_VERSION` 和 `/proc/dri/0/name` 两处都要是 `rknpu`。

> 联调方法记一笔：`make ... LOG=debug LOG_MODULES=syscall` +
> 内核自带的 `/proc/syscalls` 环形缓冲，直接把 librknnrt 的每次
> open/ioctl 抓出来。上面三条都是这么定位的 —— **不要靠读字符串表猜**。

#### ★ 真机（RK3588）第一次跑暴露的：MMIO 必须走 VMA

**症状**：`rknn` 走到 SUBMIT 时内核崩，而 `rknpu_init` 阶段一切正常。

```
[el1_sync] EL1 exception: EC=0x25, ESR=0x96000045, FAR=0xfdab0010
[el1_sync] DFSC=0x5 (translation=1 perm=0)
  #0  rknpu_submit_task+0xf0
```

`FAR=0xfdab0010` 就是 `NPU0_BASE + PC_DATA_ADDR`，**翻译错误（没映射）**。

**为什么极具误导性**：`init` 读版本寄存器（`0xfdab0000`）是成功的，而
`0xfdab0010` **是同一个 4 KiB 页**。"同页能读不能写"在直觉上不成立，会让人
往"寄存器权限/时钟"方向查。

**真因**：`rknpu.c` 用的是

```c
g_npu0_base = platform_get_uintptr("npu", "base0");   /* ✗ 裸物理地址 */
```

而 rk3588 的 `platform.conf` 里 `mmio_vma = true` —— MMIO 必须经
`KERNEL_VMA` 访问。GICv3 一直是对的（用的 `platform_get_mmio`）：

```c
g_npu0_base = platform_get_mmio("npu", "base0");      /* ✓ 自动加 KERNEL_VMA */
```

**为什么 init 能读**：boot 页表里有 MMIO 的**恒等映射**，所以裸物理地址在
`kernel_main` 阶段能用；等 busybox 起来、CPU 切到 `create_uvm()` 建的
**用户进程页表**之后恒等映射就没了，只剩 VMA 映射 —— 于是同一个地址从
"能读"变成"没映射"。

**QEMU 上为什么测不出来**：那里没有 `npu.base0`，`rknpu_init()` 在
`g_npu0_base == 0` 时就直接 return 了，**整条 MMIO 路径从没被执行过**。
这类"只在真机上才会走到"的代码，QEMU 回归给不了任何保证。

> 硬编码的 MMIO 回退地址（如 `RKNPU_PMU1_BASE`）同样要换算，所以加了
> `platform_mmio_needs_vma()` 访问器 —— 别在驱动里各写一份判断。

#### ★ 真机第二次跑暴露的：任务要按核切片（`subcore_task[]`）—— **已修，真机验证通过**

现象：`作业超时: task_status=0x2d002c completed=44`（提交的是 120 个任务），
`int_status=0x0`、`raw=0xc0000000`，NPU 跑到第 44 个就不动了。而 librknnrt
自己打印的是 `task number: 40`。

**根因**：我们在提交时用了 `args->task_start` / `args->task_number`（整个
120 条），但 vendor **从不这么干**。`rknpu_job_subcore_commit_pc()`
（`rknpu_job.c:307-330`）里：

```c
if (rknpu_dev->config->num_irqs > 1) {        /* rk3588 有 3 个 IRQ → 恒真 */
    for (i...) if (i == core_index) { write 0xe to 0x1004 / 0x3004 }
    switch (job->use_core_num) {
    case 1: case 2:  slice = args->subcore_task[core_index];
    case 3:          slice = args->subcore_task[core_index + 2];
    }
    task_start  = slice.task_start;
    task_number = slice.task_number;          /* ← 无条件覆盖 */
}
```

librknnrt 把一个模型的所有 op 拍成一张大任务数组，再**按核切片**填进
`subcore_task[5]`，每片只喂给自己的核。我们把整张表塞给 core 0，等于让它去跑
别的核的活 —— 表现出来就是跑到某处卡死、中断永不到来，而日志里
`task_status` 是个"跑到一半"的数字，完全不像配置错误。

**索引规则**（必须照抄）：3 核时各片填在 `[2..4]`，单核/双核填在 `[0..1]`。
`core_mask == RKNPU_CORE_AUTO_MASK(0)` 表示"驱动自己挑一个核"，vendor 在这种
情况下**强制 `use_core_num = 1`**。

我们只驱动 core 0，所以取 `subcore_task[2]`（3 核时）或 `subcore_task[0]`。
切片来自用户态，采信前做自洽校验（非 0、落在 `[task_start, task_start+task_number)`
内），不合法就**报错并退回完整范围** —— 不校验的话失败现场会从"跑一半"变成
"提交 0 个任务"，两者都像硬件问题。

> 附带一条：`PC_TASK_CONTROL` **不是普通可读写寄存器**，硬件会消费命令段。
> 实测写 `0x7078` 回读 `0x1078`（任务数 0x78 保留，命令段 0x7→0x1），而那次
> NPU 确实跑了任务。所以回读校验只比任务数那一段，否则每次提交都误报
> "写没进 NPU"。

#### 多核提交（`core_mask` ≥ 2 个核）

RK3588 有 3 个 NPU 核：`base0/1/2 = 0xFDAB/AC/AD0000`，IRQ 142/143/144，
每个核是**并列的独立设备**（各自的 MMIO 窗口、各自的中断线），只有核内寄存器
偏移是共用的。多核路径按 vendor 的 `rknpu_job_commit()` 实现：

| 环节 | 做法（对齐 vendor） |
|------|--------------------|
| 认哪些 mask | 只认 `0x1/0x2/0x4/0x3/0x7`。**`0x5`、`0x6` 是 vendor 明确不支持的**，照实拒绝，不替它兜底 |
| AUTO(0) | 走 `rknpu_schedule_core_index()` → 挑负载最小的核 + **强制单核** |
| 切片索引 | `use_core_num` 为 1/2 → `subcore_task[core]`；为 3 → `subcore_task[core + 2]` |
| 提交 | 每个核提交**自己那一片**，写在该核 base 上 |
| 期望中断 | **每核一份**：`该片 last_task->int_mask`。共用一份会让先完成的核把后完成的判成"状态不匹配" |
| 等待 | 等到**所有**核都报完成（vendor 的 `interrupt_count` 归零才 `wake_up`） |

两个容易写错、且**单核时完全看不出来**的点：

1. **`0x1004`/`0x3004` 写的值里带核号**：vendor 写
   `0xe + 0x10000000 * core_index`，不是常量 `0xe`。core 0 时两者相同，
   所以单核跑通完全掩盖了它；写错的话 core1/core2 的 CNA/CORE 指针会指到
   错误位置，症状是"提交了但一个任务都不完成"。
2. **三个 ISR 必须分开注册**。`irq_install()` 的回调签名是
   `void(*)(uint64_t *frame)`，frame 里没有"是哪条中断"的信息，所以只能写
   三个薄包装转调 `rknpu_irq_common(core)` —— vendor 也是三个
   `rknpu_coreN_irq_handler`。

> ⚠️ **多核是否真的会跑起来，取决于 librknnrt 给不给 `core_mask`**。
> 它给 AUTO 或单核时，vendor 驱动也是单核 —— 那就没有 3 倍可拿，这是
> "对齐 vendor"的必然结果，不是我们的实现没生效。

**真机实测（2026-10，官方 demo）**：

```
[rknpu] submit flags=0x5 core_mask=0x0 -> 1 core(s), use_core_num=1
[rknpu]   core0 tasks[0..39] (40 条) expect=0x300
[rknpu] submit flags=0x1 core_mask=0x0 -> 1 core(s), use_core_num=1
[rknpu]   core0 tasks[40..49] (10 条) expect=0x300
```

两条结论：

1. **`core_mask = 0x0` = AUTO** —— 这个 demo 要的就是单核，vendor 驱动跑它
   也是单核。多核路径已实现但**不会被这个 demo 触发**；真要用满三个核，得让
   应用侧显式要（`rknn_set_core_mask()` 之类）。
2. **librknnrt 是按 job 分批提交的**：这里两个 job（40 条 + 10 条），每批的
   `subcore_task[i]` 才是"这一批要跑多少"。**`args->task_number` 是任务数组的
   容量（120），不是工作量** —— 这正是当初"提交 120 条、跑到 44 卡死"的确切
   解释：我们把容量当成了工作量。

> 📌 **日志是这个窗口里最贵的东西，量性能时一定要关。**
> 应用侧的 `start.elapsed()` 把整个 SUBMIT 系统调用都算进去，而板子跑
> 1.5 Mbaud（每字符 ~6.7µs）。真机上因为多打了两行 INFO + 一次误报的
> ERROR 五连打（合计约 770 字符），MobileNetV1 从 **2.57ms 涨到 7.10ms**
> —— **日志本身 4~5ms，比推理还慢**。所以每次提交的这行是
> `KLOG_MODULE_DEBUG`，要看：`LOG=debug LOG_MODULES=driver`。

> 📌 **"写完回读校验"在这个硬件上不成立，已经删掉了 —— 别再想着加回来。**
>
> 初衷是对的：把"寄存器没写进去"（时钟/电源）和"写进去了但作业不跑"
> （描述符）分开，因为两者现场都是"超时 + task_status=0"。但 PC 侧的
> `DATA_ADDR` / `TASK_CONTROL` 是**硬件消费型**寄存器，PC 取走之后值就变了，
> 回读是在跟它抢时间。
>
> 真机上的两个反例正好互补，而且**两次推理结果都完全正确**：
>
> | 提交 | `DATA_ADDR` | `TASK_CONTROL`(任务数段) |
> |---|---|---|
> | 第一次 | 写 `0x5b4abc0` 回读 **`0x0`** | 正常 |
> | 第二次 | 正常 | 写 `0xa` 回读 **`0x0`** |
>
> 命中位置每次都不一样 —— 这是竞态，不是故障。代价还很实在：一次误报
> 5 行日志，在 1.5 Mbaud 上约 3.8ms，**而它落在 `start.elapsed()` 里**。
> 同一份内核连跑两次实测 **2.57ms / 6.40ms**，差值就是这段日志。
>
> 需要"写没进去"的诊断时看**超时路径** —— 那里逐核 dump 了完整 PC 寄存器
> 快照。作业没跑完才需要现场；跑完了就不需要。

**修完的结果**（真机，`./bin/rknn`，MobileNetV1）：

```
[rknpu2/src/main.rs:97:5] start.elapsed() = 2.57075ms
Top 5:
  528: desktop computer (65.625%)
  783: screen, CRT screen (9.527588%)
  527: desk (8.544922%)
```

一行 `[rknpu]` 错误都没有 —— 回读一致、切片有效、作业完成、中断到达。
注意**成功路径本来就是静默的**（IRQ 日志是 DEBUG 级，只在状态不匹配时打），
所以"日志干净"本身就是通过判据，不用额外加打印。

#### 与 vendor 驱动的三处有意分歧

提交序列（`rknpu_job_commit`）**逐行对照 testos-reflector**（真机跑过
conv2d/matmul/pool），与 vendor `rknpu-0.9.8` 不同的地方是：

1. 提交前无条件写 `PC_DATA_ADDR = 1` 切 slave 模式（vendor 只在 `pc_dma_ctrl` 下写）；
2. `0x1004`/`0x3004` 是 CNA/CORE 的 **S_POINTER**，写值 `0xe`。vendor 把 `0xe`
   当基址去算偏移（`0xe + 0x10000000 * i`）—— 参考项目的读法才对；
3. `PC_DMA_BASE_ADDR` 写 **0**（参考项目注释：librknnrt 也是 0）。vendor 写 `task_base_addr`。

#### GEM handle 按打开的文件隔离

DRM 里 handle 是 per-`drm_file` 的。pseudofs 原本没有 open 钩子，所以加了
一对**按文件**的回调（`ioctl_file_fn` / `close_file_fn`，追加在
`pseudo_node_t` 末尾，为 NULL 时自动退回老回调 —— 现存 7 个 handler 一行没改）。
⚠️ 追加字段后**必须把 `g_nodes[]` 每行都补上对应的 NULL**，
`-Wextra` 的 `-Wmissing-field-initializers` 会逐行报。

### P5 — 平台补齐（**当前完全空白**）

> 💡 **配方在参考项目里**（`testos-reflector/src/dev/`，真机验证过）：
> `cru.c` 有 NPU 全部时钟的门控位，`scmi.c` 管 `clk_npu`（那颗时钟归 ATF 管，
> 要走 SCMI 而不是直接写 CRU）。需要的时钟名见 `rknpu.h:14` 的注释：
> `clk_npu aclk0..2 hclk0..2 pclk`。
> 关键寄存器：`RK3588_CLKGATE_CON(27/28/29/30)`、`RK3588_CLKSEL_CON(73/74)`。
> **不必对着 TRM 从零推。**

* **NPU 时钟**：参考驱动用 `clk_bulk_get_all` 拿一组时钟。我们**没有时钟框架**，
  只能像 `cvitek_eth.c:284` 那样**裸 MMIO 操作 CRU** —— 需要从 RK3588 TRM
  查出 NPU 相关的门控/分频寄存器。
* **软复位**：参考驱动有 `srsts[]`（多个），我们一个都没有，超时恢复需要它。
* **电源 off**：`rkpm` 只有 `power_on`，需要补 `power_off`（PMU 的 idle-ack 协议）。

> ⚠️ **这一阶段的风险被低估的概率最高**：前四个阶段都是软件协议，有明确的对错；
> P5 是对着 TRM 猜寄存器，**错了的症状是「NPU 毫无反应」**，没有任何报错。

#### ⚠️ 但先别急着实现它：参考项目**根本没调用**那个函数

2026-10 复查参考项目源码发现：`enable_rk3588_npu_clocks()` 以及它下面那一串
`enable_aclknpu0/…/enable_ptimer_root`，**在整个仓库里没有任何调用者**
（`grep -rn` 只命中 `cru.c` 自身的定义和 `cru.h` 的声明）。而它的 NPU 用例
是**真机跑通**的。

结论：**NPU 时钟在参考项目的启动链（U-Boot / BL31）里就已经是开的**，它不需要
自己开。所以「真机上作业不启动」**不能默认归因于时钟**，得先拿数据把它排掉，
再决定要不要写 P5。

配方仍然留在上面（`cru.h` 的宏都齐了：`CLKSEL_CON`=0x300、`CLKGATE_CON`=0x800、
`SOFTRST_CON`=0xa00，CRU base 0xFD7C0000），真要写的时候照抄即可。

#### 真机超时的现场取证（2026-10 加的诊断）

现象：`作业超时: task_status=0x0 completed=0 / int_status=0x0 raw=0xc0000000
expect=0x300` —— NPU 一个任务都没完成，中断也没来。这个现场**同时**符合好几种
完全不同的故障，所以加了三条只读诊断把它们分开：

| 诊断 | 位置 | 它回答什么 |
|------|------|-----------|
| `rknpu: CRU GATE29/30/27/28 …` | `rknpu_init`（每次启动一行）| **时钟到底开没开**。0 = 开，1 = 关；`clk_npu_dsu0` 关着会额外标出来 |
| `提交后回读不一致` | `rknpu_job_commit` 末尾（只在**不一致**时打）| 写进寄存器没有。打出来 = NPU 那一侧压根没响应（时钟/电源/基址错）；**不打** = 写进去了，问题在描述符内容 |
| `task[N] flags=… regcmd_addr=…` + `regcmd@buf+0x…` | 超时路径 | 用户态到底填了什么。`regcmd_addr` 若为 0 或不在任何 dma-buf 内会直接点明；在的话按 u64 dump 出前 8 个字 |

设计意图：**一次上板就能分流**。注意最后一条的必要性 —— 任务描述符是
**librknnrt 填的**，我们只读不写；而 QEMU 上 `npu.base0` 为 0，
`rknpu_submit_task` 直接返回 `-ENODEV`，所以**这条数据通路在真机之前从未执行过**，
描述符内容属于完全未验证的输入。

`dma_buf_find_by_phys()`（`driver/dma/dma_buf.c`）就是为这条诊断加的：
用户态在描述符里填的是设备地址，拿到数字没法直接读内容，得反查是哪个缓冲区。

---

## 6. 硬件时序

### 6.1 提交（`rknpu_job.c:346-372` 的逐条对应）

```c
/* 多核时先设每核使能：offset 0xf008，核间步长 0x10000000 */
for (i = 0; i < num_irqs; i++)
    if (i == core_index) {
        REG_WRITE(0x1004, 0xe + 0x10000000 * i);
        REG_WRITE(0x3004, 0xe + 0x10000000 * i);
    }

REG_WRITE(first_task->regcmd_addr,  PC_DATA_ADDR);      /* 0x10 */
REG_WRITE((first_task->regcfg_amount + 4 /*EXTRA*/ + 2 /*scale*/ - 1) / 2 - 1,
          PC_DATA_AMOUNT);                              /* 0x14 */
REG_WRITE(last_task->int_mask,      INT_MASK);          /* 0x20 */
REG_WRITE(first_task->int_mask,     INT_CLEAR);         /* 0x24 */
REG_WRITE(((0x6 | task_pp_en) << 12 /*pc_task_number_bits*/) | task_number,
          PC_TASK_CONTROL);                             /* 0x30 */
REG_WRITE(args->task_base_addr,     PC_DMA_BASE_ADDR);  /* 0x34 */
REG_WRITE(0x1, PC_OP_EN);                               /* 0x08 —— 启动 */
REG_WRITE(0x0, PC_OP_EN);                               /* 0x08 */
```

注意 `task_base_addr` / `regcmd_addr` 都是**设备地址**（无 IOMMU 时即物理地址），
必须经 §4 那个唯一的 `npu_dev_addr()` 出口。

### 6.2 中断完成（`rknpu_job.c:640-680`）

```c
status = REG_READ(INT_STATUS);            /* 0x28 */
if (rknpu_fuzz_status(status) != job->int_mask[core_index]) {
    /* 不匹配：打日志（含 RAW_STATUS 和 task counter），清中断，返回 */
}
REG_WRITE(RKNPU_INT_CLEAR_ALL /*0x1FFFF*/, INT_CLEAR);
rknpu_job_done(job, 0, core_index);
```

`rknpu_fuzz_status()` 是个**「按 2 位一组取饱和」**的函数：把 status 的
`0x3 / 0xc / 0x30 / 0xc0 / 0x300 / 0xc00` 每对位，**只要有一位非零就置满两位**，
其余位丢掉。作用是容忍中断状态的抖动（多核/多任务叠加时计数字段会变）。
**必须原样实现**，否则会出现「作业明明跑完了但中断比对不上」的假故障 ——
而日志只会说 `invalid irq status`，不指向真因。

---

## 7. 风险清单

按「踩到后排查难度」排序：

| # | 风险 | 症状 | 对策 |
|---|---|---|---|
| **R1** | `DMA_BUF_IOCTL_SYNC` 实现成空操作 | **静默数据损坏**，模型输出随机错 | 真做 clean/invalidate；或把 NPU 缓冲一律映射成非 cacheable（参考驱动对非 cacheable 的 buffer 直接拒绝 MEM_SYNC，语义上是自洽的） |
| **R2** | CMA 预留区太小 / 用全局 PMM | 模型加载到一半失败 | 见 P1 |
| **R3** | **P5 时钟/复位寄存器猜错** | NPU 毫无反应，**无任何报错** | 对着 TRM 逐个核对；先用版本寄存器（`0x46495245`）当探针 —— 读得出说明时钟通了 |
| **R4** | `fuzz_status` 抄错 | 假故障，日志误导 | 原样移植，加单元测试 |
| **R5** | misc 路径用 DRM 的 48 字节结构体解码 | 参数错位 | 见 §3.2 警告 |
| **R6** | 缺少 `/sys/class/drm/*/device/config` 等 sysfs | 初始化可能失败，或退化成错误的带宽估算 | 先跑起来看日志；必要时提供假 sysfs 节点 |
| **R7** | ISR 里睡眠 | 断言崩溃 / 整机挂死 | 见 P3 的上下文约束 |
| **R8** | 用户指针裸解引用 | 用户传坏地址 → 内核态 #PF → 整机挂死 | **CLAUDE.md 明文规则**：ioctl 参数一律走 `copy_to_user_bytes` / `from_user`。注意 `dev.c` 里现有的 ION ioctl 就有裸解引用（`dev.c:124,226`），新代码**不要**照抄那个 |

---

## 8. 验证策略

**分层验证，每层独立可测**（这是分阶段的主要理由）：

1. **P0/P1 阶段**：纯用户态测试程序，不碰 NPU。分配 → 写 → sync → mmap → 校验。
2. **P2 阶段**：同上，加上 PRIME 往返（fd→handle→fd 后数据仍一致）。
3. **P3 阶段**：用自写的最小 submit 程序（task 描述符手工构造），
   先提交 **1 个 task**，确认中断能回。**不要一上来就跑真实模型。**
4. **P4 阶段**：跑真实 `librknnrt.so`，**先把所有 ioctl 打日志**，
   用日志补全 §3 的表，再逐个实现。
5. **端到端**：官方 `rknn_model_zoo` 里的 `mobilenet` 之类小模型。

**回归门禁建议**：参照本仓库已有的 `tools/boot_regress.sh` 风格，
做一个 `tools/npu_smoke.sh` —— 连续跑 N 次最小 submit，要求每次都拿到正确结果。
NPU 这类设备最怕的是**间歇性**故障，单次通过说明不了问题。

---

## 9. 未决问题

1. **`librknnrt` 是否真的会走 `/dev/rknpu` fallback？** 2.3.2 里两条路的 ioctl
   都在，但哪条被优先选择、fallback 的触发条件是什么，需要上板实测。
   —— 这决定 P4 里 misc 路径是「必须」还是「可选」。
2. **`RKNPU_MEM_IOMMU` 标志**：无 IOMMU 时，librknnrt 在
   `RKNPU_GET_IOMMU_EN` 返回 0 之后，是否还会在 `MEM_CREATE` 里带这个 flag？
   若带，按参考驱动 `rknpu_gem.c:707` 的语义应**忽略**（而非报错）。
3. **DVFS 要不要做**：参考驱动 808 行 devfreq + IPA/opp 表。不做则 NPU 跑在
   固定频率（能出结果，慢且费电）。**建议第一版不做**，先把功能跑通。
4. **SRAM / NBUF**：`RKNPU_MEM_TRY_ALLOC_SRAM` 等标志对应片上 SRAM 分配，
   参考驱动有 `sram_mm`（`rknpu_drv.c:1479`）。**第一版忽略这些 flag。**
5. **多 VM / 多进程并发**：本内核有 VMM，NPU 上下文是否需要 per-VM 隔离？
6. **安全缓冲（`RKNPU_MEM_SECURE`）**：需要 TEE，**明确不在范围内**。

---

## 10. 工作量粗估

| 阶段 | 内容 | 相对量 |
|---|---|---|
| P0 | dma-buf + dma-heap + mmap 泛化 | 中 |
| P1 | CMA 预留 | 小 |
| P2 | GEM 对象层 | 中 |
| P3 | NPU 作业引擎 | 中 |
| P4 | DRM shim（含 misc 路径） | 中 |
| P5 | 时钟/复位/电源 | **不确定**（取决于 TRM 清晰度） |

参考驱动 9228 行里，真正需要「重新实现」的 NPU 专属逻辑约 1000 行；
其余是 GEM（1757）、devfreq（808）、debugfs（605）、IOMMU（619）——
那些要么是我们用不到的 Linux 基础设施，要么（IOMMU）已决定推迟。
**所以这不是一个「移植 9000 行」的任务，而是一个「补三个子系统 + 写 1000 行设备逻辑」的任务。**
