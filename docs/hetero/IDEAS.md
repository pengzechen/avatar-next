# Avatar × K3 机会清单与实验想法

- 状态：Living backlog v0.1（2026-11-22）
- 关联：[Avatar 异构 AI 执行环境设计草案](HETEROGENEOUS_AI_DESIGN.md)
- 说明：这是“想法池”，不是承诺；优先级/工作量/风险会随 M0–M5 的实测调整。
- 比较纪律：性能结论必须基于**同模型、同量化、同 runtime、同时钟**；不要把官方含/不含前后处理的数字混排。

优先级：P0 = 决定架构/验收；P1 = 高价值；P2 = 后续/依赖前项。  
工作量/风险：S（小）/ M（中）/ L（大）/ H（高）。

---

## 0. 已记录的用户想法

### IDEA-001：类 CUDA 的 C++ 计算派发（用户重点）

**来源**：2026-11-22 讨论。  
**一句话**：用 C++ 把某段计算显式派发到指定核心/class，像 CUDA 一样有“launch”语义，但适配 A100 是 CPU class 而不是独立 GPU 的事实。

**核心方案**：
- 用户态 `libavatar_hetero`：`launch<Class>`、`parallel_for<Class>`、future/event、TCM scratch、class-specific worker pool。
- 内核只提供 class/TCM/DMA/QoS/AI job，不把 runtime 放进内核。
- 共享虚拟地址空间，不需要 `cudaMemcpy`；TCM 作为显式 scratch，而不是独立 device memory。
- A100 变体：`rv64gcv_zvl1024b` + 厂商 IME intrinsics；X100 变体：`rv64gcv`（VLEN=256）。
- 默认给 class，不把 hart 编进可移植代码；精确 hart 只用于 RT/诊断。
- 脏向量状态不跨 VLEN 迁移；IME 只在 A100。

**详细设计**：见 [设计草案 9.6 节](HETEROGENEOUS_AI_DESIGN.md#96-用户态-c-offload-runtime类-cuda-派发但不是-device-模型)。

**第一个实验**：
- 用 `parallel_for<AI>` 跑一个向量/矩阵 kernel，和 X100 版本对比；
- 测量 launch 开销、TCM scratch 收益、是否出现 VLEN 状态问题；
- 先不接 ONNX/llama.cpp，验证 API/编译变体/worker pool。

**风险**：如果 AI 栈不先跑通，这个 runtime 只会成为“能调用 A100 的空壳”；必须和 TCM/DMA/QoS 一期推进。

---

## 1. 机会矩阵

| ID | 优先级 | 想法 | 为什么是 Avatar/K3 | 第一个实验 | 工作量/风险 |
|---|---|---|---|---|---|
| IDEA-002 | P0 | AI + RT 混合临界调度 | X100/RT 控制 + A100 推理；Linux HMP 只做静态围栏 | 1kHz 控制 + YOLO，比较 p99/抖动 | L / M |
| IDEA-003 | P0 | 零拷贝视觉流水线 | MPP/V2D/DMA/TCM + A100；Linux 通用内存不感知 TCM | 摄像头 NV12 → 推理，测拷贝/延迟 | L / M |
| IDEA-004 | P0 | 每 job 的 TCM/DMA QoS 与安全 | 替代全局可写 `/dev/tcm`、`/dev/ai_dma`；多模型/多租户 | 两个 job 超配额/取消/泄漏测试 | M / M |
| IDEA-005 | P0 | class-level 可观测性 | 需要证明“比 Linux 好”；Linux 没有统一 timeline | trace 一帧 YOLO + 控制环，定位瓶颈 | M / L |
| IDEA-006 | P1 | 多模型 TCM 缓存与热切换 | 8GB/32GB 差异；机器人多技能；Linux page cache 不感知 TCM | 两个小模型交替，测切换/首帧 | M / M |
| IDEA-007 | P1 | AI job 取消/超时/恢复 | 机器人安全；Linux 缺少端到端 AI 资源取消 | kill/oom/取消推理，控制环不受影响 | M / M |
| IDEA-008 | P1 | 能耗感知的模型选择/DVFS | K3 端侧功耗；tokens/J、J/frame；A100/X100 OPP | YOLOv8n/s/m 的 J/frame；LLM tok/J | M / M |
| IDEA-009 | P1 | A100 的 IME 用于非 DNN 数学 | MPC/IK/Kalman/SLAM 也能吃矩阵指令 | 矩阵乘/向量 kernel 在 A100 vs X100 | M / M |
| IDEA-010 | P1 | 向量状态感知的 loader/代码变体 | VLEN 256/1024；避免库缓存错误 | X100/A100 两版预处理器；加载时选择 | M / L |
| IDEA-011 | P2 | 异构 hypervisor：guest 在 X100，AI 在 A100 | A100 无 H，Linux 嵌套 KVM 不可行；Avatar VMM 只在 X100 | X100 上 guest + A100 YOLO 并发 | L / H |
| IDEA-012 | P2 | AI 感知的模型预取/缓存 | 首帧/首 token、模型切换；NVMe/DRAM/TCM | 冷/热加载对比；预取策略 | M / M |
| IDEA-013 | P2 | `std::execution` / OpenMP target 集成 | 标准 C++ 生态；减少自定义 DSL | AI class sender 或 OpenMP offload 原型 | L / M |
| IDEA-014 | P2 | FPGA 自定义 RISC-V 扩展 / AI 加速器 | 固定 K3 核不能改 ISA；FPGA 可做自研 SoC 或外挂加速 | 软核 RoCC/CV-X-IF + 矩阵协处理器；或 K3 PCIe FPGA | L / H |

> 注：IDEA-001 单独记在上一节，因为它是用户提出的主想法。

---

## 2. 重点想法说明

### IDEA-002：AI + RT 混合临界调度

**问题**：K3 的目标场景是机器人/边缘，不是只跑 benchmark。控制环要确定性，AI 推理要吞吐；Linux 需要 CFS/EAS + PREEMPT_RT + HMP 拼起来，且 HMP 只做静态 class 围栏。

**Avatar 做法**：
- X100/RT 类保留固定配额，A100 类跑推理；
- class-aware runqueue + deadline/QoS；
- AI job 资源记账，控制环不被 TCM/DMA/IRQ 抖动拖垮；
- 推理 deadline miss 时降级/丢弃，而不是阻塞控制。

**验收**：1kHz 控制 + YOLOv8s；比较 Linux HMP 与 Avatar 的 p99、最大抖动、推理吞吐；同模型/同 runtime。

### IDEA-003：零拷贝视觉流水线

**问题**：K3 有 MPP、V2D、OpenCL、DMA、TCM；视觉链路如果每级都 memcpy，端到端延迟和能耗都会浪费。

**Avatar 做法**：
- 相机/MPP 的 NV12 帧通过 DMA 进入 TCM/DRAM 缓冲；
- A100 worker 直接消费，预处理/后处理在 X100 或 A100 的 class 上按阶段拆分；
- 内核提供 buffer 生命周期、DMA fence、cache 维护、配额；
- 不把整层 CNN 拆到两个 class（VLEN 不同），而是按 pipeline stage 拆。

**验收**：同一路视频，比较 Linux HMP vs Avatar 的端到端 p50/p99；记录 copy bytes 和 J/frame。

### IDEA-004：每 job 的 TCM/DMA QoS 与安全

**问题**：Linux 的 `/dev/tcm`、`/dev/ai_dma` 是全局设备（当前甚至 world-writable），多模型/多进程没有配额和隔离。

**Avatar 做法**：
- TCM/DMA 以 capability + fd 暴露；每 job/进程配额；
- IOMMU 校验 DMA；释放清零；崩溃/超时回收；
- AI job 准入：TCM 不足直接拒绝，不静默 fallback；
- 支持多模型/多租户，但不泄露 TCM 内容。

**验收**：两个 job 竞争 TCM；超配额被拒；kill/崩溃后无泄漏；非法 DMA 被 IOMMU 拦截。

### IDEA-005：class-level 可观测性

**问题**：没有观测就无法证明“比 Linux 好”，也无法定位 31 FPS 和 59 FPS 的差异。

**Avatar 做法**：
- 每 class 的 runqueue/迁移/拒绝/向量切换字节；
- AI job 的 launch/等待/TCM/DMA/IRQ/首帧/尾延迟；
- 一帧推理的时间线：预处理、DMA、推理、后处理、控制唤醒；
- 用户态可读取的轻量 counters + 可选 trace。

**验收**：一次 YOLO + 控制混合运行的 timeline 能解释主要延迟来源。

### IDEA-006：多模型 TCM 缓存与热切换

**问题**：机器人可能要 YOLO + VLM/LLM + 专用模型并存；8GB 板不可能全部常驻，32GB 板也有切换成本。

**Avatar 做法**：
- 模型权重 DRAM 缓存、热数据/scratch TCM；
- 预取/锁页/优先级；模型切换时保留/驱逐策略；
- 与 AI job/配额联动，避免换模型把控制环拖死。

**验收**：两个模型交替 100 次，测切换延迟、首帧、TCM 命中、DRAM 带宽。

### IDEA-007：AI job 取消/超时/恢复

**问题**：机器人安全要求 AI 推理可中止；Linux 的进程/线程取消很难覆盖 DMA/TCM/IRQ 的完整生命周期。

**Avatar 做法**：
- job fd 的 cancel/timeout；abort DMA、回收 TCM、停止 AI worker；
- 控制线程不受影响；记录取消原因和资源状态；
- 防止已取消 job 继续写共享 buffer。

**验收**：取消长推理/异常 job；无泄漏、无 DMA 残留、控制环抖动可测。

### IDEA-008：能耗感知的模型选择/DVFS

**问题**：K3 端侧/机器人功耗重要；A100 2.0GHz vs X100 2.4GHz，TCM 命中降低 DRAM 能耗；Linux HMP 没有统一的能量模型。

**Avatar 做法**：
- class 级 OPP/thermal；按 AI job 的 deadline/吞吐选择模型规格/频率；
- 统计 J/frame、tokens/J；在满足 deadline 时降频/停 A100；
- 与 RT 控制环协调，避免功耗/热节流破坏控制。

**验收**：YOLOv8n/s/m 和 0.5B–4B LLM 的 J/frame、tok/J 曲线；给出“满足 p99 的最省电配置”。

### IDEA-009：A100 的 IME 用于非 DNN 数学

**问题**：SpacemiT-IME 主要被当作 AI 指令，但矩阵乘/点积/线性代数同样适合 MPC、IK、Kalman、SLAM。

**Avatar 做法**：
- 用 `libavatar_hetero` 把控制数学 offload 到 A100；
- 用小 TCM scratch、宽 RVV/IME；
- 与控制环在同一进程/共享内存，避免网络/IPC。

**验收**：矩阵乘/向量运算的 A100 vs X100 延迟、J/次；看是否值得把部分控制数学搬过去。

### IDEA-010：向量状态感知的 loader/代码变体

**问题**：X100/A100 的 VLEN 不同；通用库可能在 X100 上缓存 VLEN=256 的决策，迁移到 A100 后崩溃。

**Avatar 做法**：
- 按 class 加载不同库/内核变体；
- 动态链接器/loader 在 exec 前选择 class；
- 提供 hwprobe/能力查询；禁止脏向量跨 VLEN 迁移；
- 对多版本库做显式路径。

**验收**：同一程序在 X100/A100 两个变体上正确运行；误加载时给出明确错误，不 SIGSEGV。

### IDEA-011：异构 hypervisor（P2）

**问题**：A100 没有 H，Linux KVM 的 nested 方案走不通；但 Avatar 的 VMM（H）可以在 X100 上跑 guest Linux/RTOS，同时 A100 跑 host AI。

**Avatar 做法**：
- X100: guest VMs / H 虚拟化；
- A100: host AI workers；
- TCM/DMA 由 host 控制，guest 默认不直接拿；
- 隔离/IRQ/内存带宽实测。

**风险**：guest 和 host AI 抢 X100/DRAM/IRQ；A100 与 guest 的共享数据面需要 virtio/共享内存方案；工作量最大，先作为研究项。

### IDEA-012 / 013：标准 C++ 集成与模型预取

- **IDEA-012**：AI-aware prefetch/page cache：模型权重从 NVMe/DRAM 预取，降低首帧/首 token 和切换；需要和文件系统/page cache 协作。
- **IDEA-013**：`std::execution` scheduler 或 OpenMP target 接入：在自定义 `libavatar_hetero` 稳定后，再做标准集成，避免三套 API 并存。

### IDEA-014：FPGA 自定义 RISC-V 扩展 / AI 加速器（用户新想法）

**先澄清一个边界**：K3 的 X100/A100 是固定硅片，**FPGA 不能给这两类核增加指令**。只有两条现实路径：

**Path A：自研 FPGA RISC-V SoC，Avatar 作为 OS**
- 用 VexRiscv/CVA6/BOOM 等软核，通过 RoCC（Rocket/BOOM）、CV-X-IF（CVA6）或 VexRiscv custom instruction 接口加矩阵/点积协处理器；
- 自定义状态、TCM/DMA、IRQ、MMU、timer 都要自己/开源 IP 组合；
- Avatar 的 class/TCM/DMA/AI job 设计可以直接复用；把 FPGA AI 核作为新的 execution class；
- 适合：研究新 ISA、验证 Avatar 异构调度/向量状态、ASIC 前原型、特定模型/算子加速；
- 不适合：在软核上用几条自定义指令“复制”60 TOPS 的 A100 量级通用 AI CPU；频率/资源/内存带宽和软件栈都差很远。

**Path B：K3 + PCIe FPGA 加速卡**
- K3 有 PCIe；FPGA 作为外挂加速器，通过 DMA/descriptor/queue 工作，**不是给 X100/A100 加指令**；
- Avatar 写驱动，把 FPGA 暴露为 AI class/设备；用 `libavatar_hetero` 或 AI job 调度；
- 适合：给 K3 增加特定加速器；可以复用 HLS/TVM/自研 systolic array；比自研完整 SoC 现实；
- 风险：PCIe 带宽、DMA/IOMMU、驱动、工具链、功耗/成本。

**Path C：FPGA SoC 做 Avatar 的异构实验平台**
- 实例化两种不同 VLEN/ISA 的 RISC-V 核，专门验证 class、向量状态、TCM/调度；
- 性能不是目标，研究/验证是目标。

**性能直觉**（必须实测，不要只看峰值）：
- K3 A100：60 TOPS INT4 sparse、8 核 2.0GHz、VLEN=1024、TCM/DMA + 成熟软件栈；
- 软核 + 自定义 SIMD/matrix 在 FPGA 上通常受频率（百 MHz 级）、DSP 数量、片上/DRAM 带宽、工具链限制，量级上很难追平 A100；
- 高端 AI FPGA 的硬化 AI Engine/DSP 阵列可以到几十到上百 INT8 TOPS，但那是专用阵列，不是“RISC-V 核加几条指令”；而且端到端仍受内存和编译器影响；
- 如果目标是“某个模型/算子 + 特定精度/稀疏性 + 低功耗/确定性”，FPGA 可能很有竞争力；通用 AI CPU 平替不现实。

**第一个实验**：
- Path A：VexRiscv/CVA6 + 一个 GEMM/卷积协处理器，在 Avatar 上跑通；对比 X100/A100 的延迟、频率、资源、TOPS/W；先做自定义 `.insn`/intrinsics，不碰 ONNX/llama.cpp；
- Path B：K3 + PCIe FPGA 的 DMA 回环/一个 3×3 conv 或 GEMM，比较 PCIe 传输 + 计算 vs A100；先验证 IOMMU/DMA/驱动。

**待回答**：你的 FPGA 型号/接口（PCIe？板载 SoC？）、目标模型/算子、目标是研究 ISA 还是产品性能——不同答案会改变 Path A/B 的选择。

---

## 3. 建议推进顺序

### Wave 1（地基）

1. IDEA-004 TCM/DMA QoS/security
2. IDEA-005 可观测性
3. IDEA-010 向量 loader/代码变体
4. IDEA-002 的 RT/control 侧原型

### Wave 2（可演示的差异化）

1. IDEA-001 C++ offload MVP
2. IDEA-003 零拷贝视觉
3. IDEA-006 多模型缓存/切换
4. IDEA-007 取消/恢复
5. IDEA-008 能耗

### Wave 3（研究/生态）

1. IDEA-009 A100 非 DNN 数学
2. IDEA-011 异构 hypervisor
3. IDEA-012 预取
4. IDEA-013 标准 C++ 集成
5. IDEA-014 FPGA 自定义 RISC-V 扩展/加速器（先做可行性/性能模型）

---

## 4. 不建议做的事

- 从零写 ONNX Runtime/llama.cpp 来“超过 Linux”；优先复用厂商 EP/AI SDK，或移植。
- 把 AI runtime、模型图、算子调度放进内核；内核只做机制。
- 试图用 FPGA 给 K3 的固定 X100/A100 核“加指令”；只能外挂 PCIe 加速器，或自研完整 SoC。
- 照搬 CUDA 的 device memory/grid/block/warp；A100 是 8 个 CPU 核 + RVV/IME。
- 允许脏向量状态跨 VLEN 透明迁移；正确性风险远大于收益。
- 保留世界可写的 TCM/DMA 接口；必须先做配额/IOMMU/清零。
- 把单层 CNN 拆到 X100/A100；按 pipeline stage 拆，不按层拆。
- 用 60 TOPS 峰值或官方“含前后处理”数据做“超过 Linux”的结论；必须同 runtime 实测。

---

## 5. 下一步三个实验

1. **A100 class + TCM mmap 微基准**  
   - 验证 A100 线程启动、VLEN=1024 向量状态、TCM 分配/清零、DMA 基本回路。
   - 输出：可重复的微基准与失败模式。

2. **`libavatar_hetero` MVP**  
   - `launch<AI>`/`parallel_for<AI>` + worker pool + future/event；先跑一个矩阵/向量 kernel。
   - 对比 X100 版本；看 launch 开销、TCM scratch、class 选择是否正确。

3. **混合负载原型**  
   - 1kHz 控制线程（X100）+ YOLOv8s（A100）；同 runtime/模型与 Linux HMP 比较。
   - 指标：p50/p99 延迟、最大抖动、FPS、J/frame、TCM 使用。

---

## 6. 关联开放问题

- K3 固件是否允许 Avatar 独立启动/使用 A100 核？
- TCM 的 per-cluster 映射、cacheability、coherency 和 DMA 一致性？
- 厂商 EP/AI SDK 的 Linux ABI 依赖有哪些，能否做用户态兼容层？
- `/dev/tcm`、`/dev/ai_dma` 的厂商 ABI 是否可重实现？
- 8GB 板的模型上限与 32GB 板差异；验收模型选型。
- 官方 8 核 59 FPS 与本板 8-thread 32.8 FPS 的差异来源。

详见 [设计草案](HETEROGENEOUS_AI_DESIGN.md) 的“风险与开放问题”和“待评审决策”。
