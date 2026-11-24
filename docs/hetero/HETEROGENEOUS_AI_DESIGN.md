# Avatar 异构 AI 执行环境设计草案

- 状态：Draft v0.1（2026-11-22）
- 参考平台：SpacemiT K3（8×X100 + 8×A100，AIA，TCM/DMA，IME）
- 适用范围：Avatar OS / RISC-V 64 位、异构 CPU 类、AI 运行时、TCM/DMA、调度与 QoS
- 相关文档：[平台配置指南](../ARCH_PLATFORM_PROFILE_GUIDE.md)、[RISC-V trap](../RISCV64_TRAP.md)、[HEXT VMM bugfix](../bugfix/RISCV64_HEXT_VMM_BUGFIX.md)、[OpenSBI ECALL bugfix](../bugfix/RISCV64_ECALL_OPENSBI_BUG.md)、[实时架构](../realtime/ARCHITECTURE.md)、[项目参考](../PROJECT_REFERENCE.md)、[机会清单](IDEAS.md)

> 本文是设计草案，不是已实现接口的文档。所有 syscall、结构体、Kconfig 名称均为草案，合并前需要 ABI/API review。

---

## 0. TL;DR

Avatar 不应该把 K3 的 16 个 RISC-V 核当成同质 CPU：0–7 是 X100（RV64GCVH、VLEN=256、有 H），8–15 是 A100（RV64GCV、VLEN=1024、无 H、带 SpacemiT-IME、TCM/DMA）。

Linux 的供应商 HMP 用“线程分类 + 亲和性围栏 + `/proc/set_ai_thread`”解决了正确性，但它是一个补丁式方案：默认线程只能跑 X100，跨类 affinity 直接 `EINVAL`，没有自动负载均衡，TCM 只是 `/dev/tcm` 字符设备，IRQ/hotplug 都要特判。

Avatar 的机会不是“写一个更快的调度器”，而是把这五件事做成 first-class：

1. **执行类（execution class）**：CPU 能力、VLEN、H/IME/TCM 和 OPP 都是一等属性，而不是一个统一 `cpu_t`。
2. **向量状态安全**：明确“脏向量状态不跨 VLEN 迁移”；分阶段做 clean-only 迁移，拒绝用 `SIGSEGV` 发现错误。
3. **TCM/DMA 作为内存层**：`mmap`/分配器/零拷贝/配额/清理，不再依赖全局可写的 `/dev/tcm`、`/dev/ai_dma`。
4. **AI + 实时混合调度**：X100/RT 核跑控制，A100 跑推理；class-aware runqueue、QoS、能耗和尾延迟优先。
5. **AI runtime 合作**：内核只提供机制，ONNX Runtime/llama.cpp 等留在用户态；但必须给它们稳定的 class/TCM/DMA/QoS ABI。

最重要的现实约束：**单模型吞吐由 AI runtime（IME/TCM/EP）决定，不是内核调度器单独决定。** 2026-11-22 在 K3 Com260 IFX 上实测，同一份 YOLOv8s INT8 模型，A100 + SpaceMIT EP 约 31–39 FPS，而 X100 普通 CPU EP 只有约 0.56 FPS。Avatar 如果没有等价的 AI 运行时，就不可能仅靠内核赢 Linux。

**成功标准不是“全面替代 Linux”，而是：在 K3 这类异构 AI CPU 上，指定 AI+实时混合负载的端到端延迟/尾延迟/能耗/多模型组合能力优于 Linux HMP，同时单模型性能不落后于同一 runtime 下的 Linux 基线。**

---

## 1. 背景

### 1.1 K3 硬件事实

| 项目 | X100（0–7） | A100（8–15） |
|---|---|---|
| 角色 | 通用 CPU / 控制 / KVM | AI 计算 |
| ISA | `rv64imafdcvh`（RVA23） | `rv64imafdcv`（RVA23*，不含 H） |
| RVV | 1.0，VLEN=256 | 1.0，VLEN=1024 |
| 自定义 ISA | 无 | SpacemiT-IME 矩阵/点积指令 |
| 拓扑 | 8 核，共享 8MB L2 | 2 cluster × 4 核，每 cluster 1MB L2 + 1.5MB TCM |
| 频率 | 最高 2.4GHz | 最高 2.0GHz |
| 其它 | H 扩展，可做 KVM/Xvisor | DMA、TCM；无 H，不可做 KVM host |

平台另含 AIA/IMSIC、IOMMU、2× RT24 实时核和 GPU；RT24 目前由供应商固件/ESOS 管理，Avatar 是否接管属于开放问题。

Linux 侧可观察到的事实：

- 设备树有 16 个 `cpu@` 节点；A100 节点带 `cpu-ai` 属性，有独立 `opp_table0_a100`。
- `lscpu` 显示两组合并：A100（8 核，2.0GHz）和 X100（8 核，2.4GHz）；`nproc` 默认是 8，因为当前线程被 HMP 限制在 X100。
- `/proc/device-tree/cpus/cpu@8` 的 `riscv,isa = rv64imafdcv`；`cpu@0` 是 `rv64imafdcvh`。
- Linux 报 `tcm 0.tcm: direct mmap phys 0x0 size 0x300000 block_size 0x60000 block_num 8 via /dev/tcm`。
- `/dev/tcm`、`/dev/ai_dma` 存在。

### 1.2 Linux 供应商 HMP 的行为

`CONFIG_SPACEMIT_HMP=y` 时：

- 设备树 A100 节点标 `cpu-ai`，内核维护 regular/AI 两个 cpumask。
- 新线程默认是 `HMP_REGULAR`，只能在 X100 上跑。
- `sched_setaffinity()` 跨类返回 `-EINVAL`；常规线程只能设 X100，AI 线程只能设 A100。
- 默认 IRQ 亲和性限定在 regular 核；绑到 A100 的 IRQ 会被重映射。
- 最后一块 regular CPU 不允许 offline；有 AI 线程时最后一块 AI CPU 不允许 offline。
- `/proc/set_ai_thread`（只写）把调用线程标为 AI，之后进程只能跑 CPU 8–15，且**不会自动迁回**。
- 注册必须早于 exec 或使用向量指令；否则动态链接器/OpenBLAS 已按 VLEN=256 缓存决策，迁到 A100 会 `SIGSEGV`。
- 没有自动跨类负载均衡；策略是“围栏 + 人工 pin”。

这套机制正确、简单、能跑厂商 AI 栈，但代价明显：

- 进程内混合使用 X100 + A100 的线程要手工拆分且受亲和性 API 限制。
- 没有 QoS、deadline、能耗或多模型调度。
- TCM 没有进入 mm/scheduler；DMA 和 TCM 的权限模型很原始。
- 没有统一的“CPU 能力”视图，用户态需要知道 `/proc/set_ai_thread` 这种 Linux 专有路径。
- 向量迁移问题被回避，而不是被建模。

### 1.3 SpacemiT ORT EP 的集成事实

供应商 Vision/YOLO/LLM 走 ONNX Runtime + `libspacemit_ep.so`。有两个关键集成点：

1. **`Ort::SessionOptionsSpaceMITEnvInit()`**：配置 `SPACEMIT_EP_INTRA_THREAD_NUM`（AI 线程数，K3 为 8）、`SPACEMIT_EP_INTER_THREAD_NUM=1`、`SPACEMIT_EP_USE_GLOBAL_INTRA_THREAD=0`，并查询 TCM 布局。直接用 Python ORT 加 `SpaceMITExecutionProvider`、不调用这个初始化，会在 `SpineNode` 构图时 segfault。
2. **AI 线程必须由 HMP 标记**：`/proc/set_ai_thread` 或等价机制；否则运行在 X100，不会使用 A100/IME 路径。

这说明：内核需要提供的是 **稳定的 AI 线程 class、TCM/DMA 资源和正确的 session ABI 前置条件**，而不是自己实现模型图执行器。

### 1.4 已有性能基线（2026-11-22，Com260 IFX，8GB）

YOLOv8s，640×640，厂商 INT8 量化 ONNX（`yolov8s.q.onnx` / `no_dfl`），纯模型前向：

| 配置 | 中位延迟 | FPS |
|---|---:|---:|
| A100 + SpaceMIT EP，8 threads | 32.0 ms | 31.2 |
| A100 + SpaceMIT EP，4 threads | 25.8 ms | 38.7 |
| X100 + CPU EP | 1779.8 ms | 0.56 |
| A100 + CPU EP（对照） | 3468.9 ms | 0.29 |

厂商 model-zoo 的阶段性数据（引擎 2.0.6，含/不含前后处理）：

| 配置 | 含前后处理 | 纯推理 |
|---|---:|---:|
| 4×A100 核 | 34.3 FPS | 39.4 FPS |
| 8×A100 核 | 48.5 FPS | 59.0 FPS |

结论：

- 同一模型，A100 EP 与 X100 CPU EP 差 50 倍以上；A100 只换 CPU EP 反而更慢。**runtime/EP 是主要变量。**
- 我们的 8-thread 结果低于官方 8 核数据，4-thread 结果接近官方 4 核数据；说明线程配置、引擎版本、session 参数仍会影响结果，Avatar 的基准必须锁定这些变量。
- 60 TOPS 是 INT4 sparse 峰值，不是 YOLOv8s 的实际吞吐；不要用它做验收。

> 参考：K3 上的 Qwen3-4B（A100 83 tok/s prefill、11.2 tok/s decode；X100 19 / 5.1），Qwen3-30B-A3B（32GB 板，4-bit，约 38 tok/s prefill、8–11 tok/s decode）。本板 8GB，30B 不可行；Avatar 的首批 LLM 验收模型应选 0.5B–4B，或换 32GB 板。

---

## 2. 目标与非目标

### 2.1 目标

1. 把 X100 / A100 / RT 核建模成显式的 **CPU 执行类**，提供能力探测、线程分类和迁移策略。
2. 让**同一进程**可以安全地组合 X100 控制线程与 A100 推理线程；跨类迁移只在定义明确的条件下发生。
3. 提供 TCM/DMA 的一等内存/缓冲能力：分配、映射、零拷贝、配额、生命周期和清理。
4. 在 K3 上 bring-up A100 class（无 H、VLEN=1024、IME、TCM/DMA），不破坏 X100 的 H/VMM 路径。
5. 设计 AI/RT 混合调度：class-aware runqueue、QoS/截止时间、IRQ/AIA、能源/热约束。
6. 为 AI runtime 提供稳定 ABI：能力探测、class、TCM/DMA、AI job/配额、可观测性；避免 `/proc/set_ai_thread` 这类临时接口成为长期契约。
7. **在相同 runtime、模型、量化和时钟条件下**，在指定混合负载上对比 Linux HMP；目标是在尾延迟、能耗、多模型/模型切换、隔离性中至少一项取得可重复的改进，单模型吞吐不劣化。

### 2.2 非目标

- 不在内核里实现 ONNX Runtime、llama.cpp 或张量编译器；不调度算子图。
- 不自研完整的 NPU/GPU 驱动或 AI 框架生态。
- 不承诺“全面超过 Linux”或“60 TOPS 跑满”；不以峰值算力代替实测。
- 不在 A100 上运行 VMM/KVM（A100 无 H）；VMM 仍只属于 X100 class。
- 第一阶段不做脏向量状态的透明跨 VLEN 迁移；不做未经证明的“自动迁移”。
- 不为了 Linux 二进制兼容而牺牲整个 ABI；兼容层是用户态选项，不是内核契约。
- 不接管 RT24/ESOS（开放问题，先保留接口）。

### 2.3 成功标准（草案）

**功能：**

- 16 个 hart 在 Avatar 上分区为 X100/A100 两个 class，class 能力可通过 syscall 查询。
- A100 class 可运行普通 RISC-V 用户线程；无 H 路径；VLEN=1024 的向量上下文正确保存/恢复。
- TCM 可被用户态映射；DMA 可对 TCM/DRAM 缓冲工作；进程退出/崩溃后资源清理。
- 同一进程的 X100 线程与 A100 线程可并发运行；向量脏状态迁移被安全拒绝并返回明确错误。
- RT 线程 + A100 推理线程混合运行时，RT 抖动可测量、可约束。

**性能：**

- 在相同 runtime（优先厂商 EP/AI SDK）下，单模型性能不劣于 Linux HMP 基线（目标：差距 ≤5%）。
- 指定的混合负载（控制环 + YOLO/LLM，多模型/模型切换）在 p99 延迟、抖动或 token/J 上相对 Linux HMP 有可重复改进（建议目标：≥20%，以实测为准）。
- 不以“内核调度 overhead”做主要卖点；瓶颈必须在同一 runtime 下定位。

**安全/正确性：**

- DMA 必须经过 IOMMU/地址校验；TCM 释放后清零；无世界可写 DMA 接口。
- 跨 VLEN 迁移不会产生 `SIGSEGV` 或静默数据损坏。
- 现有 VMM/guest 与 X100 路径不受影响。

---

## 3. 架构原则

- **P1：CPU class 优先于同质 CPU 假设。** 能力（ISA/VLEN/H/IME/TCM/OPP）是调度和 ABI 的一等输入。
- **P2：显式契约。** 线程在 exec/创建时选 class；跨类迁移由策略控制，不静默发生。
- **P3：向量安全优先。** 脏向量状态不跨 VLEN 迁移；必要时返回 `EXDEV`/`EAGAIN`，而不是让用户态崩溃。
- **P4：TCM/DMA 是内存层，不是 char device。** 统一配额、映射、清理、安全。
- **P5：内核机制，用户态 runtime。** 模型图、量化、算子调度在用户态；内核只负责 class、TCM、DMA、QoS、IRQ、隔离和记账。
- **P6：同 runtime 比较。** 没有厂商 EP/等价 runtime，Avatar 的单模型性能没有意义；基准必须锁定模型、量化、线程、时钟、前后处理。
- **P7：安全与可观测性。** DMA/TCM 能力大，默认最小权限；所有放置/迁移/资源事件可观测。

---

## 4. 总体架构

```text
用户态
┌────────────────────────────────────────────────────────────┐
│ 控制/实时应用        AI runtime (ONNX RT / llama.cpp / EP)  │
│ (RT control loop)    (vision / LLM / VLM workers)           │
│        │                         │                         │
│        └─────────── 共享内存 / futex / DMA ────────────────┘
│                        │  syscalls
├────────────────────────┼────────────────────────────────────┤
内核                     ▼
┌────────────────────────────────────────────────────────────┐
│ hetero ABI: probe / class / migration / tcm / dma / job    │
├────────────────────────────────────────────────────────────┤
│ class-aware scheduler + RT/QoS  │ vector state manager     │
│                                  │                          │
│ TCM/DMA 内存层 + 配额/IOMMU       │ AI job/资源记账          │
│                                  │                          │
│ IRQ/AIA + OPP/thermal + observability                      │
├────────────────────────────────────────────────────────────┤
│ X100 class (0–7)   │ A100 class (8–15) │ RT/其它（开放）    │
│ VLEN 256, H, 2.4G  │ VLEN 1024, IME    │                    │
└────────────────────────────────────────────────────────────┘
```

组件职责：

| 组件 | 职责 | 不负责 |
|---|---|---|
| class 管理 | 枚举、能力、拓扑、OPP、用户探测 | 设备驱动 |
| 调度器 | class-aware runqueue、准入、迁移策略、RT/QoS | 模型图/算子调度 |
| 向量状态 | 保存/恢复、VLEN、脏状态、迁移检查 | 用户库的 VLEN 缓存 |
| TCM/DMA | 分配、映射、零拷贝、配额、清理 | 模型格式/量化 |
| AI job | 资源/配额/记账/取消；可选 QoS 容器 | 推理执行 |
| AI runtime（用户态） | 算子、量化、EP、TCM 使用、线程池 | 内核策略 |
| IRQ/AIA | 设备中断路由、class 亲和 | 模型调度 |

---

## 5. 执行类与线程模型

### 5.1 能力模型

```c
/* 草案，ABI 版本化 */
#define AVATAR_HETERO_ABI_VERSION 1

enum avatar_cpu_class_id {
    AVATAR_CLASS_GENERAL = 0,   /* X100 */
    AVATAR_CLASS_AI      = 1,   /* A100 */
    AVATAR_CLASS_RT      = 2,   /* 预留：RT24 或 X100 保留核 */
    AVATAR_CLASS_MAX     = 3,
};

#define AVATAR_CPU_F_HAS_H     (1u << 0)
#define AVATAR_CPU_F_HAS_IME   (1u << 1)
#define AVATAR_CPU_F_HAS_TCM   (1u << 2)
#define AVATAR_CPU_F_HAS_DMA   (1u << 3)

struct avatar_cpu_class_info {
    __u32 class_id;
    __u32 nr_harts;
    __u32 vlenb;              /* K3: X100=32, A100=128 */
    __u32 flags;              /* AVATAR_CPU_F_* */
    __u32 tcm_bytes;
    __u32 tcm_block_bytes;
    __u32 l2_bytes;
    __u32 max_freq_khz;
    __u64 isa_ext[2];         /* 需要与 RISC-V 发现机制对齐 */
    __u64 capacity;           /* 调度用的相对算力，非 TOPS */
};

struct avatar_hetero_info {
    __u32 abi_version;
    __u32 nr_classes;
    struct avatar_cpu_class_info classes[AVATAR_CLASS_MAX];
    __u32 default_class;
    __u32 reserved;
};
```

boot 时从 DT/ACPI 建立 class 表；当前 K3 观察：

- class 0：hart 0–7，`rv64imafdcvh`，vlenb=32，有 H。
- class 1：hart 8–15，`rv64imafdcv`，vlenb=128，无 H，IME/TCM/DMA。
- class 2：预留；RT24 是否可见取决于固件，暂不承诺。

不把 `nr_harts` 写死为 8：板级 profile 和固件可能改变可见 hart 数，能力表由平台探测生成。

### 5.2 线程属性

```c
enum avatar_migration_policy {
    AVATAR_MIGRATE_NEVER          = 0, /* 默认：创建时 class 固定 */
    AVATAR_MIGRATE_IF_VEC_CLEAN   = 1, /* future：仅在无脏向量状态时 */
    AVATAR_MIGRATE_EXPLICIT       = 2, /* future：显式申请并接受失败 */
};

struct avatar_thread_attrs {
    __u32 class_mask;           /* 允许的 class 位图 */
    __u32 preferred_class;      /* 0..MAX，或 AVATAR_CLASS_AUTO */
    __u32 migration_policy;
    __u32 qos_class;            /* best-effort / latency / throughput */
    __u64 tcm_quota_bytes;
    __u32 ai_job_fd;            /* 可选：加入一个 job/配额域 */
};
```

默认策略：

- 普通进程：`class_mask = GENERAL`，`migration = NEVER`，保持与今天的单核类型行为一致。
- AI 线程：必须显式申请 AI class（特权/配额；初期 root，后续 capability）。
- RT 线程：需要 RT 配额/特权；不与 AI 自动混排。
- 内核内部线程：绑定原 class，不参与自动迁移；避免破坏设备亲和性。

### 5.3 迁移规则（第一阶段）

一个线程一旦执行过向量指令，记录 `vector_dirty` 与 `vector_vlenb`。允许切换 class 的必要条件：

```c
static bool class_migration_allowed(struct thread *t, struct cpu_class *dst)
{
    if (!(t->hetero.class_mask & (1u << dst->id)))
        return false;
    if (t->hetero.migration_policy == AVATAR_MIGRATE_NEVER)
        return false;
    if (t->vec.dirty && t->vec.vlenb != dst->vlenb)
        return false;                 /* 脏状态跨 VLEN：拒绝，不尝试“修” */
    if (t->hetero.migration_policy == AVATAR_MIGRATE_IF_VEC_CLEAN && t->vec.dirty)
        return false;
    return true;
}
```

错误语义：从用户态显式申请迁移时，返回 `EXDEV`（不支持的 class/脏向量）或 `EAGAIN`（暂时不允许）；不要返回成功后再崩溃。`execve()` 和线程创建是选择 class 的安全点；之前线程的 class 不自动延续到新 class。

### 5.4 用户 ABI 草案

```c
/* syscall 编号未分配，名称仅占位 */
long sys_avatar_hetero_probe(struct avatar_hetero_info *info, size_t size);
long sys_avatar_thread_set_class(pid_t tid, unsigned int class_id,
                                 struct avatar_thread_attrs *attrs,
                                 unsigned int flags);
long sys_avatar_thread_get_class(pid_t tid, struct avatar_thread_attrs *attrs);
long sys_avatar_thread_set_migration(pid_t tid, unsigned int policy);
```

需要考虑的选项：

- 扩展 `sched_setaffinity` 还是新增 syscall？建议新增 `avatar_thread_*`，避免改变 Linux/POSIX 语义；`sched_setaffinity` 仍在 class 内生效。
- 是否使用 `prctl`？跨类/迁移/TCM 配额语义较复杂，独立 syscall 更清晰。
- 旧程序只传 X100；新程序显式 probe，不再猜 `/proc/set_ai_thread`。
- 错误码、权限、ABI 版本、32/64 位兼容需要单独评审。

---

## 6. 向量状态与迁移

### 6.1 状态与成本

RISC-V RVV 状态（示意）：

- 32 个向量寄存器；
- `vtype`/`vl`/`vstart`/`vxrm`/`vcsr` 等控制状态；
- 上下文大小 ≈ `32 × vlenb + 控制字`；K3：
  - X100：约 1KB/线程；
  - A100：约 4KB/线程。

跨 class 上下文切换不能只换 `vsetvli`：用户的 `vtype`/`vl` 缓存、动态链接器和数学库可能已经按一种 VLEN 生成/缓存了路径。Linux 供应商 HMP 的“注册必须早于向量使用”正是为了避免这种不确定性。

### 6.2 分阶段策略

**Phase 1（正确性优先，MVP）**

- class 固定；`vector_dirty` 后禁止跨 VLEN class 迁移。
- 显式迁移请求返回 `EXDEV`；用户态可查询原因。
- `execve`/线程创建选择 class；AI runtime 在 exec 前选择 A100，之后不再迁回。
- 上下文切换支持 per-hart VLEN；内核绝不用“全局 VLEN”假设。
- 信号帧/ucontext/ptrace 记录 VLEN 与向量状态；不把 A100 的 4KB 状态按 X100 的 1KB 布局解释。

**Phase 2（clean-only 迁移）**

- 在系统调用边界、显式 `madvise`/hint 或线程阻塞点，如果向量状态干净，允许迁移；
- 迁移时重新初始化向量语义（`vsetvli` 由用户态/运行时决定）；
- 仍不允许“脏状态透明转换”；把迁移成本、失败率暴露给用户态。

**Phase 3（研究）**

- 规范化向量状态格式、VLEN 256↔1024 转换；
- 编译器/运行时协作：按 class 生成代码变体、避免运行时缓存 VLEN；
- 在没有形式化保证前，不把“自动透明迁移”作为卖点。

```c
/* 概念伪代码：上下文切换 */
if (prev->vec.dirty)
    save_vector_state(prev, prev->vec.vlenb);   /* 1KB 或 4KB */
if (next->vec.dirty)
    restore_vector_state(next, next->vec.vlenb);
/* 不把 prev 的 VLEN 状态复制给 next；用户重新 vsetvli */
```

### 6.3 与现有 Avatar 代码的关系

- Avatar 当前 riscv64 异常/切换路径假定 HS-mode/H；`hstatus` 的符号名在旧 binutils 下会被编码成 0x200（vsstatus），现有代码用数值/平台宏规避。A100 无 H，**不能在 A100 上执行任何 H CSR 访问**；需要 class-aware 的异常/切换路径，或把 A100 线程限制在不走 VMM 的代码路径。
- 现有 SG2002（无 H）路径可作为 no-H 参考，但 K3 的 A100 还多出 VLEN=1024、IME、TCM/DMA。
- VMM 只在 X100 class；guest 的向量状态与 host 线程向量状态分开管理。

---

## 7. 内存、TCM 与 DMA

### 7.1 内存层

| 层 | 用途 | 特点 |
|---|---|---|
| DRAM | 通用内存、模型权重、页缓存 | 大、较慢、通用 |
| TCM | AI 激活/scratch/热数据、DMA 缓冲 | 小（K3 Linux 报 3MB/8 blocks），低延迟，需显式管理 |
| L2 | 硬件缓存 | 由硬件管理，不直接分配 |
| IOMMU 映射 | DMA 地址空间 | 隔离/校验，不能靠全局设备 |

### 7.2 TCM 分配与映射

K3 Linux 观察到的布局：`size=0x300000`、`block_size=0x60000`、`block_num=8`。Avatar 不应依赖 `/dev/tcm` 的数值，而应从平台描述/固件接口探测。

草案 API 二选一（需 ABI review）：

```c
/* 方案 A：fd + mmap */
int tcm_alloc(size_t bytes, size_t align, unsigned int flags);
void *mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_TCM, fd, 0);

/* 方案 B：显式映射原语 */
int tcm_map(struct avatar_tcm_region *out, size_t bytes, unsigned int flags);
```

约束：

- 按 block 粒度分配（K3 0x60000）；不足则失败，不自动 fallback 到 DRAM（用户态决定）。
- 默认只允许 AI class 线程使用；配额；进程退出/崩溃自动回收。
- 释放清零（避免模型/数据泄漏）；可配置性能与安全权衡。
- TCM 不是普通可换页内存：不参与页回收；`fork` 语义需明确（建议不允许隐式继承，需显式复制/重新分配）。
- TCM 与具体 A100 cluster 的映射关系（是否是 per-cluster、跨 cluster 访问开销）必须由平台文档/实测确定，不能假设统一 NUMA。

### 7.3 DMA 与零拷贝

- 提供显式的 DMA 缓冲对象（或 TCM fd 的 DMA 导出能力）；不鼓励任意用户态物理地址 DMA。
- DMA 地址必须经 IOMMU/平台校验；记录缓冲区生命周期；进程退出/取消 job 时 abort 所有在途 DMA。
- 与 AI runtime 的接口：模型权重/激活可以是 DRAM 或 TCM；允许 async copy + double buffering；提供 fence/完成通知（futex/eventfd 或 Avatar 等价机制）。
- 目前 Linux 的 `/dev/ai_dma` 是全局设备；Avatar 应改为 capability/句柄，避免跨进程干扰。
- TCM 是否与 CPU cache 一致、是否需要 cache maintenance，是**开放问题**；在文档/驱动确认前，按最保守方式处理（显式 clean/invalidate，或使用非缓存映射）。

### 7.4 与 VMM/guest 的关系

- 第一阶段：TCM/DMA 不暴露给 guest；X100 VMM 继续使用自己的 stage-2 和 guest 内存。
- 第二阶段：可评估把 AI 缓冲以受控方式映射给 guest（virtio/AI 设备），但需要 IOMMU/stage-2 联合验证。
- A100 无 H，不作为 VMM host；guest 的 H 功能只在 X100。

---

## 8. 调度、QoS 与中断

### 8.1 class-aware 调度

- 每个 class 维护独立 runqueue/负载；同一 class 内可正常 load balance。
- 跨 class 的放置发生在：`execve`、线程创建、显式 class 设置、clean-only 迁移点；不在每次 wakeup 时静默迁移。
- `capacity`/OPP/能耗是调度输入；不要用单一“core count”代表算力。
- 允许同一进程内多个 class 的线程：AI worker 在 A100，控制/IO 在 X100；通过共享内存/futex/DMA 通信。
- 不让 AI class 线程抢占 RT 控制线程；保留 X100 的一部分算力给控制/IRQ/内核，避免 A100 推理把交互延迟拖垮。

### 8.2 AI 与实时混合

K3 的卖点是“AI + 实时”，但 Linux 的通用调度器需要靠 cgroup/RT/HMP 组合。Avatar 可以：

- 为控制环保留 RT 配额（X100 或 RT24，若可接管）；
- AI 推理线程 best-effort/throughput QoS，设 TCM/CPU 配额；
- 使用 per-class 带宽/LLC/内存 QoS（如果硬件提供）；
- 测量 RT 抖动（cyclictest 等价）与推理 p50/p99，而不是只看平均 FPS；
- 支持“推理突发”与“控制确定性”的边界策略（例如 deadline miss 降级）。

### 8.3 IRQ/AIA

- K3 使用 AIA/IMSIC；Avatar 需要先完成 AIA 驱动/路由，不能照搬 QEMU PLIC 假设。
- 设备 IRQ（DMA、Tensor/VPU、网络）按 job/class 路由到合适的核；避免所有 IRQ 挤在 X100 或跨 class 抖动。
- 支持 IRQ affinity 与 CPU class 的联合约束；AI 线程退出后回收 IRQ。
- 与 Linux HMP 的“IRQ 重映射到 regular”不同，Avatar 可以按 job 的 class 显式选择，但必须有安全上限。

### 8.4 功耗/热/频率

- class 级 OPP：X100 2.4GHz / A100 2.0GHz；按负载调频。
- 能耗感知放置：A100 跑 AI 的同时，X100 可以降频；TCM 命中减少 DRAM 能耗。
- 热约束下的降级策略：限制 A100 频率/线程数，而不是把 AI 迁到 X100（VLIW/VLEN/IME 不匹配）。
- 与厂商固件/ESOS 的功耗管理接口需要确认，避免 Avatar 和 ESOS 同时控制。

---

## 9. AI Runtime 与用户态接口

### 9.1 分工

- **内核**：class、TCM、DMA、IRQ、QoS、配额、记账、隔离、可观测性。
- **用户态**：ONNX Runtime/llama.cpp/EP、量化、模型加载、算子调度、线程池、TCM 策略、模型缓存。
- **不做**：内核内张量运行时、内核内 ONNX 解析、内核内模型调度。

### 9.2 Runtime 接入路径（按优先级）

1. **复用厂商用户态 AI SDK/EP + Avatar 兼容层**（现实首选）
   - 厂商 EP/AI SDK 已经基于 Linux/ONNX Runtime；Avatar 需要提供等价的 `mmap`/TCM/DMA/线程/能力查询/`SessionOptionsSpaceMITEnvInit` 前置条件。
   - 需要处理：`/dev/tcm`、`/dev/ai_dma`、`/proc/set_ai_thread`（或让厂商 runtime 使用 Avatar 的 class API）、A100 affinity、TCM 布局查询。
   - 优点：性能最接近 Linux；缺点：ABI/许可证/厂商支持需要谈。
2. **把 ONNX Runtime/llama.cpp 的 SpacemiT 后端移植到 Avatar**
   - 需要 Linux 兼容层或直接移植；工作量大，但可控。
3. **为特定模型写最小 native kernel**
   - 仅用于原型/验证 TCM/调度，不作为“超过 Linux”的依据。

推荐：M2 先做路径 1 的兼容层；同时把 runtime 需要的 ABI 固化为 Avatar 的 `ai_buffer`/`tcm`/`class` 接口；路径 2 作为中期。

### 9.3 AI job/资源域

可选但建议：把“AI 线程组 + TCM 配额 + DMA 上下文 + 优先级/截止时间”绑定成一个 `ai_job` fd/对象：

```c
struct avatar_ai_job_params {
    __u32 class_id;        /* AI */
    __u32 nr_threads_hint;
    __u64 tcm_quota_bytes;
    __u64 deadline_ns;     /* 0 = best effort */
    __u32 priority;
    __u32 flags;
};
/* 草案：创建 fd；线程通过 attrs.ai_job_fd 加入；退出/取消时统一回收 */
```

它不是算子调度器；它只做：
- CPU/TCM/DMA 配额与准入；
- 生命周期与清理（崩溃/取消）；
- 统计（延迟、TCM 用量、DMA 字节）；
- QoS（优先级、deadline、降级）。

### 9.4 模型生命周期

- 模型加载/量化/编译离线完成；内核不解析模型。
- 多模型常驻：DRAM 权重 + TCM 热数据；模型切换时预取/锁页；可观测首 token/首帧延迟。
- 8GB 板：30B 模型不现实；验收用 0.5B–4B；32GB 才考虑 Qwen3-30B-A3B。
- 安全：模型文件权限、TCM 清零、DMA 隔离；不允许无权限进程读他人的 TCM/DMA。

### 9.5 工具链/代码变体

- X100/A100 的代码变体：VLEN 不同，IME 指令不同；用户态加载器/运行时必须按 class 选择二进制或 kernel。
- 暴露能力：`hetero_probe`/hwprobe；不要硬编码 `vlenb`。
- IME：由厂商工具链/intrinsics 生成；Avatar 内核不模拟 IME；X100 上不允许执行 IME 指令（否则非法指令，由 trap/ABI 处理）。
- 编译器支持：GCC/Clang 的 SpacemiT target 仍在演进；Avatar 的 ABI 需要允许 vendor toolchain 生成代码。
- 运行时缓存：任何库都不得在迁移后复用旧 VLEN 的决策；exec/class 选择要早于向量使用。

### 9.6 用户态 C++ offload runtime（类 CUDA 派发，但不是 device 模型）

> 来源：2026-11-22 讨论。想法是“像 CUDA 那样，用 C++ 把某些计算显式派发到指定核心”。这里把它作为设计输入记录下来；完整机会清单见 [IDEAS.md](IDEAS.md)。

A100 是同一个 Avatar 下的 CPU class，不是独立 GPU：共享 MMU、虚拟地址空间、进程和内核。CUDA 的独立 device memory、grid/block/warp/SIMT、`cudaMemcpy` 不应直接照搬；应实现 **class-aware 异步执行器 + 共享地址空间 + TCM scratch**。

分工：

- 内核：class、TCM/DMA、IRQ、QoS、AI job、配额、观测（本设计前面的部分）。
- 用户态 `libavatar_hetero`：worker pool、launch/parallel_for、future/event、TCM 分配、class 变体加载。
- 模型推理继续用厂商 EP/llama.cpp；这个库面向自定义 kernel、预处理/后处理、控制数学、小算子，不替代 AI runtime。

API 草案：

```cpp
namespace av = avatar::hetero;

// 异步派发到 AI class（A100 worker pool）
auto fut = av::launch<av::ai>(av::workers(8), [&](av::worker& w) {
    auto scratch = w.tcm<float>(64 * 1024);   // TCM scratch，显式生命周期
    compute(scratch, w.worker_id());
});
fut.wait();   // 或 fut.get()

// 数据并行：在 A100 class 上跑 parallel_for
av::parallel_for<av::ai>(0, N, 8, [&](int i) {
    out[i] = transform(in[i]);
});

// 精确 hart 只用于诊断/RT；默认按 class 交给调度器
av::launch<av::ai>(av::harts({8, 9, 10, 11}), kernel);
```

语义：

- 所有 C++ 指针共享同一 VA；没有 `cudaMemcpy`；TCM 通过显式 API 分配/映射，作为 scratchpad 或 DMA 源/目标。
- `launch` 返回 future/event，可取消；AI worker 不迁移 class；脏向量状态不跨 VLEN 迁移。
- 默认给 class，不把 hart 编号写进可移植代码；RT/诊断才给 hart mask。
- 同一进程可以有 `general` 线程和 `ai` 线程；通过共享内存/futex/atomic 通信，不通过内核消息拷贝。

编译：

- 每个 class 单独变体：A100 用 `rv64gcv_zvl1024b` + 厂商 IME intrinsics；X100 用 `rv64gcv`（VLEN=256）。
- 用 CMake 目标/构建规则产出 class-specific object，运行时按 class/probe 选择；加载器不得把 A100 变体给 X100。
- 标准 C++ 标量代码可以在任一 class 跑；一旦使用向量/IME，class 基本固定；IME 只在 A100。
- 不依赖“一份二进制自动最优”；不要求通用编译器生成 IME。

边界：

- 8 个 A100 核不是几千个 GPU lane；不要把 grid/block/warp 生搬到 RVV/IME。
- 不要把 offload runtime 放进内核；内核只提供机制。
- 不要用它替代 ONNX Runtime/llama.cpp；AI 模型端到端仍以厂商 EP 为基线。
- 不要把 `std::execution`、OpenMP target、自定义 DSL 同时做成三套；先做一个最小 API，再评估标准集成。

里程碑建议：M2 后做 `libavatar_hetero` MVP（`launch<Class>`、`parallel_for<Class>`、future/event、TCM scratch）；M3 与 AI/RT 混合负载联调。

---

## 10. K3 平台 bring-up 需求

### 10.1 CPU/AIA/OPP

- 从固件/DT 获取 16 个 hart、class、`cpu-ai`、OPP、cache、AIA 中断拓扑。
- 用 SBI HSM 启动 secondary harts；确认固件允许 Avatar 启动 A100 核（开放问题）。
- 实现 K3 AIA/IMSIC 驱动，替换 QEMU PLIC 假设；per-hart/class IRQ 路由。
- class 的 OPP/频率/电源域；热管理；与 ESOS/固件协调。

### 10.2 TCM/DMA/IOMMU

- TCM 物理布局、per-cluster 映射、cacheability/coherency、block size、保护。
- DMA 引擎编程接口、IOMMU/stage-2、完成/错误中断、超时、取消。
- 设备树/ACPI 描述与 driver；不要依赖 Linux `/dev/tcm` ABI。
- 用户态零拷贝 buffer 生命周期与安全。

### 10.3 无 H 路径与 VMM 边界

- A100 无 H：异常/切换代码不得访问 `hstatus`/H CSRs；需要 per-class 代码路径。
- 现有 `hstatus` 符号编码问题：旧 binutils 把符号 `hstatus` 编成 0x200（vsstatus）；在 HS-mode 可用，在 no-H/VS-mode 非法。K3 的 A100 上必须修掉/避免。
- VMM/guest 只在 X100；A100 不参与 H 扩展、KVM、嵌套虚拟化。
- X100 的 H 路径与 A100 的 no-H 路径共享代码时，使用 class 分支/编译期配置，确保不会误执行 H 指令。

### 10.4 平台 profile

- 新增 K3 平台 profile（例如 `k3-riscv64`）：
  - CPU class 表；
  - AIA/TCM/DMA 驱动；
  - 无 H class 的编译开关；
  - 默认 class/affinity；
  - 板级内存/OPP/thermal 参数。
- 保持 QEMU `qemu-virt-riscv64` 平台不被破坏；K3 代码用平台 profile/Kconfig 隔离。
- 构建/测试矩阵：QEMU TCG、QEMU KVM（X100 guest）、K3 硬件；A100 只在硬件/特殊固件环境测试。

---

## 11. 观测、基准与验收

### 11.1 指标

- 单模型：prefill/decode（LLM）、FPS/latency（视觉）、模型加载/首帧。
- 混合负载：端到端 p50/p99、RT 抖动、AI 吞吐、deadline miss。
- 资源：TCM 使用/命中、DMA 字节、DRAM 带宽、class 利用率、迁移拒绝次数、向量切换字节。
- 能耗：J/frame、J/token、平均/峰值功耗。
- 安全：配额拒绝、DMA 越界、TCM 清零验证。

### 11.2 方法

- **同 runtime/同模型/同量化/同线程/同时钟** 比较 Linux HMP 与 Avatar。
- 锁定 CPU 频率（如果可）、关闭后台服务、固定输入、warmup、重复 3 次取中位/p99。
- 明确是否含前后处理；模型 zoo 的“含/不含”不能混排。
- 记录引擎版本（例如 SpacemiT ORT 2.0.7）、TCM 配置、AI 线程数、KV cache 类型。
- 对 YOLO：厂商 `.q.onnx`（INT8）和 `no_dfl` 分别测；对 LLM：Qwen3-4B/0.5B–4B，f16 KV，`-t 8 -tb 8`。
- 对 RT：控制环频率 + 推理负载下的 cyclictest 等价抖动。

### 11.3 已有基线（用于 regress/对照）

| 项目 | 数据 | 备注 |
|---|---|---|
| YOLOv8s INT8，A100 EP，8 threads | 32.0ms / 31.2 FPS | Avatar 之前的最小 C++ runner；纯前向 |
| YOLOv8s INT8，A100 EP，4 threads | 25.8ms / 38.7 FPS | `onnxruntime_perf_test`，引擎 2.0.7 |
| YOLOv8s INT8，X100 CPU EP | 1779.8ms / 0.56 FPS | 对照，不是 AVX 优化路径 |
| YOLOv8s INT8，A100 CPU EP | 3468.9ms / 0.29 FPS | 说明 EP 必需 |
| 官方 model-zoo，8×A100，含/不含前后处理 | 48.5 / 59.0 FPS | 引擎 2.0.6，阶段性 |
| Qwen3-4B，A100 / X100 | 83 / 11.2 vs 19 / 5.1 tok/s | LPC 2026；prefill/decode |
| Qwen3-30B-A3B，32GB | ~38 prefill / 8–11 decode | 社区；本板 8GB 不可行 |

> 这些数字只用于建立比较基线。Avatar 的目标不是“超过 59 FPS”，而是同 runtime 下不落后，并在混合负载指标上建立优势。

---

## 12. 里程碑

### M0：需求/资料/基线（现在）

- 收集 K3/A100/TCM/DMA/AIA 文档、固件约束、AI SDK/EP ABI。
- 固化本设计的 open questions。
- 搭 benchmark harness（YOLOv8s/llama.cpp/混合负载），在 Linux 上记录基线。

**退出条件**：文档评审通过；Linux 基线可重复。

### M1：A100 execution class bring-up

- class 表、能力探测、`hetero_probe`。
- 16 hart 启动，A100 无 H；VLEN=1024 的向量上下文；class 默认策略。
- 一个最小 A100 用户程序（向量/标量）跑通；不迁移。

**退出条件**：A100 class 可运行用户态；X100 VMM 不受影响；无 H 路径稳定。

### M2：TCM/DMA + AI job 骨架

- TCM 分配/mmap/配额/清理；DMA buffer + IOMMU。
- 一个 AI runtime 兼容层/最小 runner（优先厂商 EP），跑通 YOLOv8s 或 llama.cpp。
- 记录 TCM 命中、延迟、能耗。

**退出条件**：同一模型在 Avatar + 兼容层上的性能基线可测；无内存泄漏/越权。

### M3：class-aware 调度与混合负载

- class-aware runqueue、混合 X100/A100 线程、clean-only 迁移策略、IRQ/AIA 路由。
- 控制环 + YOLO/LLM 混合负载；RT 抖动/尾延迟对比 Linux HMP。

**退出条件**：指定混合负载在 p99/抖动或能耗上达到目标；单模型不劣化 >5%。

### M4：AI runtime/QoS/安全

- 厂商 EP/AI SDK 或 llama.cpp 端到端接入；AI job/配额；多模型/模型切换。
- 功耗/热、安全审计（DMA/TCM）、可观测性。

**退出条件**：多模型/多租户场景可重复；配额/清理/越权测试通过。

### M5：K3 产品化 profile

- 板级 profile、AIA/OPP/thermal、驱动、回归、文档。
- 与 Linux HMP 的最终对比报告（同 runtime/模型/负载）。

**退出条件**：可重复的验收报告；明确的“更好”指标和边界。

---

## 13. 风险与开放问题

### 13.1 高风险

1. **固件是否允许 Avatar 独立启动 A100 核**：Linux 能启动不代表 Avatar 的 SBI/固件路径允许；可能被 ESOS/厂商 AI 固件占用。
2. **A100 无 H**：现有 Avatar H/VMM 路径不能直接复用；`hstatus` 编码/异常路径需修正。
3. **TCM/DMA 文档与 coherency**：TCM 是否 cacheable、是否 per-cluster、DMA 一致性/cache maintenance 未确认，错误会导致数据损坏。
4. **AI runtime 移植**：厂商 EP/ONNX Runtime/llama.cpp 依赖 Linux ABI；兼容层工作量和许可证/支持不确定。没有 runtime，性能目标不成立。
5. **性能归因**：内核调度不是单模型瓶颈；如果投入只做调度，可能没有可测收益。
6. **8GB 内存**：30B 不可行；模型/工作负载选择受限。
7. **安全**：TCM/DMA 能力大；DMA 越界/信息泄漏代价高。
8. **AIA/IRQ**：K3 硬件使用 AIA，Avatar 当前 QEMU 经验不足；IRQ 路由错误会破坏实时性。

### 13.2 开放问题（需厂商/实测）

- A100 的启动流程、复位/时钟/电源；OpenSBI/固件是否暴露 HSM 和 16 个 hart？
- A100 是否被 RT24/ESOS 或 AI 固件占用？Avatar 能否独占？
- TCM 的物理映射、per-cluster 语义、cacheability、coherency、保护单元？
- DMA 引擎/IOMMU 编程接口；是否支持 per-job 地址空间？
- IME 指令/工具链/ABI 的开放程度；能否在 Avatar 用户态合法使用？
- `/dev/tcm`、`/dev/ai_dma` 的厂商 ABI 是否稳定？能否由 Avatar 重实现？
- A100 的 AIA/IMSIC 中断路由与 guest/VS 支持？
- 现有 `hstatus` 编码问题在 K3/A100 上的所有路径；是否需要统一改动。
- RT24 是否归 Avatar，还是 ESOS 固定占用。
- 官方 8 核 59 FPS 与我们 8-thread 32.8 FPS 的差异来源（引擎/线程/session/模型）。
- 32GB vs 8GB 板的内存/带宽对 30B 模型的影响。

### 13.3 非目标性风险

- 试图“自己写一个 ONNX Runtime/llama.cpp”会拖垮项目；应优先复用/移植厂商栈。
- 把“内核调度器”当作单模型性能的银弹；应以同 runtime 基线约束目标。
- 过早承诺“完全自动迁移”会引入正确性风险；分阶段。

---

## 14. 附录 A：K3 现场观察

```text
# 设备树类信息（节选）
cpu@0..7:  riscv,isa = rv64imafdcvh, status = okay
cpu@8..15: riscv,isa = rv64imafdcv,  cpu-ai, status = okay
opp_table0_x100 / opp_table0_a100

# lscpu（节选）
Spacemit(R) A100  8 cores, max 2000 MHz
Spacemit(R) X100  8 cores, max 2400 MHz
CPU(s): 16, On-line: 0-15

# Linux HMP
CONFIG_SPACEMIT_HMP=y
/proc/set_ai_thread (--w--w--w-)
hmp_set_ai_thread / hmp_cpu_affinity_restrict / ...
smp: Brought up 1 node, 16 CPUs

# TCM
tcm 0.tcm: direct mmap phys 0x0 size 0x300000
          block_size 0x60000 block_num 8 via /dev/tcm
/dev/tcm, /dev/ai_dma

# SpacemiT ORT
libspacemit_ep.so 2.0.7
Ort::SessionOptionsSpaceMITEnvInit(...)
SPACEMIT_EP_INTRA_THREAD_NUM = 8
SPACEMIT_EP_INTER_THREAD_NUM = 1
SPACEMIT_EP_USE_GLOBAL_INTRA_THREAD = 0
```

---

## 15. 附录 B：ABI/接口草案清单

| 项目 | 草案 | 备注 |
|---|---|---|
| 能力探测 | `avatar_hetero_probe()` | 返回 class/vlenb/H/IME/TCM/DMA/频率 |
| 线程 class | `avatar_thread_set_class()` | class_mask、迁移策略、QoS |
| 迁移 | `AVATAR_MIGRATE_NEVER` / `IF_VEC_CLEAN` | 第一阶段只 NEVER；clean-only 为后续 |
| TCM | `tcm_alloc()` + `mmap(MAP_TCM)` | block 粒度；配额；清零 |
| DMA | `dma_alloc()` / buffer fd | IOMMU；生命周期；取消 |
| AI job | `avatar_ai_job_create()` | 可选资源/QoS 域；不是图调度 |
| 能力权限 | `CAP_HETERO_AI` / `CAP_TCM` / `CAP_DMA` | 初期 root；后续细粒度 |
| 观测 | counters/tracing | class/migration/TCM/DMA/job |
| 兼容 | 用户态 Linux 兼容层 | 不把 Linux ABI 作为内核契约 |

具体 syscall 编号、结构体对齐、ABI 版本、错误码在 M1 前完成 review；本表仅为范围声明。

---

## 16. 参考资料

- SpacemiT K3 Datasheet: https://github.com/spacemit-com/docs-chip/blob/main/en/key_stone/k3/k3_docs/k3_ds.md
- SpacemiT K3 HMP 文档: https://www.spacemit.com/community/document/info?lang=zh&nodepath=software/SDK/buildroot/k3_buildroot/device/hmp.md
- SpacemiT AI SDK 文档: https://www.spacemit.com/community/document/info?nodepath=ai/application_tools/ai-sdk.md&lang=en
- SpacemiT model-zoo-vision（模型与 INT8 性能表）: https://gitee.com/spacemit-robotics/model-zoo-vision
- LPC 2026: Heterogeneous RISC-V Cores in Linux: https://lpc.events/event/20/contributions/2479/attachments/2178/4814/LPC2026-Heterogeneous-RISC-V-Cores-in-Linux-SpacemiT-202610.pdf
- RISC-V H 扩展、RVV 1.0、AIA 规范（以 ratified 版本为准）
- Avatar: [平台配置指南](../ARCH_PLATFORM_PROFILE_GUIDE.md)、[RISC-V trap](../RISCV64_TRAP.md)、[HEXT VMM](../bugfix/RISCV64_HEXT_VMM_BUGFIX.md)、[实时架构](../realtime/ARCHITECTURE.md)

---

## 17. 待评审决策

1. class ABI 用独立 syscall 还是 `prctl`/`sched_setattr` 扩展？
2. TCM 用 fd+mmap 还是物理区间映射？是否允许 `fork` 继承？
3. AI job 是否作为正式对象，还是先只做线程 class + TCM 配额？
4. A100 class 是否允许非 root 使用（配额/capability）？
5. RT24 是否纳入 Avatar，还是保持 ESOS 所有？
6. 是否接受“单模型性能与 Linux 持平、在混合负载/能耗上取胜”的定位？
7. 首个验收工作负载：YOLOv8s + 1kHz 控制，还是 Qwen3-0.6B/4B 多模型？
