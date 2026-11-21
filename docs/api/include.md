# include — C API

[← 返回总览](index.md) · 363 个函数 · 说明覆盖率 90.1%

<a id="mod-include-aarch64-barrier_impl.h"></a>

## include/aarch64/barrier_impl.h — AArch64 内存屏障实现

`include/aarch64/barrier_impl.h` · 7 个函数 · 7 个有说明 · 87 行

使用 DMB, DSB, ISB 指令

<a id="fn-include-aarch64-barrier_impl.h-23"></a>

### `barrier_data`

```c
static inline void barrier_data(void)
```

barrier_data - 完整数据内存屏障

DMB ISH: Inner Shareable 数据内存屏障
确保所有可见的内存访问完成

**位置**：`include/aarch64/barrier_impl.h:23`

<a id="fn-include-aarch64-barrier_impl.h-33"></a>

### `barrier_data_read`

```c
static inline void barrier_data_read(void)
```

barrier_data_read - 读数据屏障

DMB ISHLD: Inner Shareable 读数据屏障

**位置**：`include/aarch64/barrier_impl.h:33`

<a id="fn-include-aarch64-barrier_impl.h-43"></a>

### `barrier_data_write`

```c
static inline void barrier_data_write(void)
```

barrier_data_write - 写数据屏障

DMB ISHST: Inner Shareable 写数据屏障

**位置**：`include/aarch64/barrier_impl.h:43`

<a id="fn-include-aarch64-barrier_impl.h-55"></a>

### `barrier_instr_full`

```c
static inline void barrier_instr_full(void)
```

barrier_instr_full - 完整指令同步屏障

ISB: 指令同步屏障, 刷新流水线

**位置**：`include/aarch64/barrier_impl.h:55`

<a id="fn-include-aarch64-barrier_impl.h-60"></a>

### `barrier_sync`

```c
static inline void barrier_sync(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

执行 dsb sy 指令完成数据同步屏障，并带 memory 约束阻止编译器重排。

**位置**：`include/aarch64/barrier_impl.h:60`

<a id="fn-include-aarch64-barrier_impl.h-72"></a>

### `barrier_acquire`

```c
static inline void barrier_acquire(void)
```

barrier_acquire - 获取语义屏障

使用 LDAR 加载指令的隐含屏障

**位置**：`include/aarch64/barrier_impl.h:72`

<a id="fn-include-aarch64-barrier_impl.h-82"></a>

### `barrier_release`

```c
static inline void barrier_release(void)
```

barrier_release - 释放语义屏障

使用 STLR 存储指令的隐含屏障

**位置**：`include/aarch64/barrier_impl.h:82`

<a id="mod-include-aarch64-cache_impl.h"></a>

## include/aarch64/cache_impl.h — AArch64 缓存操作底层实现

`include/aarch64/cache_impl.h` · 7 个函数 · 7 个有说明 · 118 行

架构特定的单个缓存行操作

<a id="fn-include-aarch64-cache_impl.h-37"></a>

### `__clean_dcache_one`

```c
static inline void __clean_dcache_one(const void *addr)
```

__clean_dcache_one - 清理单个缓存行
@addr: 缓存行对齐的地址

**位置**：`include/aarch64/cache_impl.h:37`

<a id="fn-include-aarch64-cache_impl.h-46"></a>

### `__invalidate_dcache_one`

```c
static inline void __invalidate_dcache_one(const void *addr)
```

__invalidate_dcache_one - 使单个缓存行失效
@addr: 缓存行对齐的地址

**位置**：`include/aarch64/cache_impl.h:46`

<a id="fn-include-aarch64-cache_impl.h-55"></a>

### `__clean_and_invalidate_dcache_one`

```c
static inline void __clean_and_invalidate_dcache_one(const void *addr)
```

__clean_and_invalidate_dcache_one - 清理并使单个缓存行失效
@addr: 缓存行对齐的地址

**位置**：`include/aarch64/cache_impl.h:55`

<a id="fn-include-aarch64-cache_impl.h-65"></a>

### `get_cache_line_size`

```c
static inline size_t get_cache_line_size(void)
```

get_cache_line_size - 获取缓存行大小

**位置**：`include/aarch64/cache_impl.h:65`

<a id="fn-include-aarch64-cache_impl.h-73"></a>

### `init_cache`

```c
static inline void init_cache(void)
```

init_cache - 初始化缓存子系统

<details>
<summary>函数内备注（5 条）</summary>

**第 79 行**

> 读取 CTR_EL0 寄存器

**第 82 行**

> 提取 DminLine 字段 (bits [19:16])

**第 85 行**

> 计算缓存行大小: WORD_SIZE * 2^DminLine

**第 88 行**

> 验证缓存行大小的合理性

**第 90 行**

> 无效的缓存行大小, 使用默认值

</details>

**位置**：`include/aarch64/cache_impl.h:73`

<a id="fn-include-aarch64-cache_impl.h-108"></a>

### `sync_icache_all`

```c
static inline void sync_icache_all(void)
```

sync_icache_all - 让刚写入的指令对本核可见

`ic iallu` 使本核 I-cache 全失效(另一颗核要看到得用 ialluis),
dsb 保证失效完成, isb 保证后续取指看到新指令 —— 三者缺一不可。

**位置**：`include/aarch64/cache_impl.h:108`

<a id="fn-include-aarch64-cache_impl.h-113"></a>

### `sync_caches`

```c
static inline void sync_caches(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

仅调用 barrier_data()，为缓存同步提供内存排序语义，不发具体缓存维护指令。

**位置**：`include/aarch64/cache_impl.h:113`

<a id="mod-include-aarch64-cpu_impl.h"></a>

## include/aarch64/cpu_impl.h — AArch64 per-CPU 指针 / 硬件 ID 实现

`include/aarch64/cpu_impl.h` · 3 个函数 · 3 个有说明 · 42 行

在 VHE 下 TPIDR_EL1 别名为 TPIDR_EL2, 与 EL2 host 内核透明兼容。

<a id="fn-include-aarch64-cpu_impl.h-21"></a>

### `arch_cpu_self_set`

```c
static inline void arch_cpu_self_set(struct cpu *self)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 cpu 结构指针写入 TPIDR_EL1，随后执行 isb 确保后续读取立即生效。

**位置**：`include/aarch64/cpu_impl.h:21`

<a id="fn-include-aarch64-cpu_impl.h-27"></a>

### `arch_cpu_self_get`

```c
static inline struct cpu *arch_cpu_self_get(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取 TPIDR_EL1 寄存器并转换为 struct cpu 指针返回，用于获取当前 CPU。

**位置**：`include/aarch64/cpu_impl.h:27`

<a id="fn-include-aarch64-cpu_impl.h-35"></a>

### `arch_cpu_hw_id`

```c
static inline uint64_t arch_cpu_hw_id(void)
```

MPIDR_EL1 低 24 位为 Affinity 0/1/2；QEMU virt 单簇时 Aff0 即 CPU 序号

**位置**：`include/aarch64/cpu_impl.h:35`

<a id="mod-include-aarch64-exception_impl.h"></a>

## include/aarch64/exception_impl.h

`include/aarch64/exception_impl.h` · 6 个函数 · 6 个有说明 · 87 行

<a id="fn-include-aarch64-exception_impl.h-26"></a>

### `arch_irq_flags`

```c
static inline uint64_t arch_irq_flags(void)
```

读当前 DAIF(不改动状态)

**位置**：`include/aarch64/exception_impl.h:26`

<a id="fn-include-aarch64-exception_impl.h-36"></a>

### `arch_irq_is_enabled`

```c
static inline int arch_irq_is_enabled(void)
```

读是否允许 IRQ(抢占判断用：处于 irqsave 临界区时为 0)

**位置**：`include/aarch64/exception_impl.h:36`

<a id="fn-include-aarch64-exception_impl.h-42"></a>

### `arch_irq_save`

```c
static inline uint64_t arch_irq_save(void)
```

关中断并返回旧状态(必须用 arch_irq_restore 配对恢复)

**位置**：`include/aarch64/exception_impl.h:42`

<a id="fn-include-aarch64-exception_impl.h-60"></a>

### `arch_irq_restore`

```c
static inline void arch_irq_restore(uint64_t flags)
```

恢复到 arch_irq_save()/arch_irq_flags() 取到的状态

**位置**：`include/aarch64/exception_impl.h:60`

<a id="fn-include-aarch64-exception_impl.h-70"></a>

### `arch_irq_disable`

```c
static inline void arch_irq_disable(void)
```

无条件关中断(不需要旧状态时用)

**位置**：`include/aarch64/exception_impl.h:70`

<a id="fn-include-aarch64-exception_impl.h-79"></a>

### `arch_irq_enable`

```c
static inline void arch_irq_enable(void)
```

无条件开中断(新任务首次运行时使用)

**位置**：`include/aarch64/exception_impl.h:79`

<a id="mod-include-aarch64-halt_arch.h"></a>

## include/aarch64/halt_arch.h — AArch64 架构特定的 halt 实现

`include/aarch64/halt_arch.h` · 1 个函数 · 1 个有说明 · 41 行

<a id="fn-include-aarch64-halt_arch.h-22"></a>

### `arch_halt`

```c
static inline void arch_halt(void)
```

arch_halt - 停止 AArch64 处理器
使用 WFI (Wait For Interrupt) 指令进入低功耗状态
在中断禁用的情况下, WFI 会永久停止

<details>
<summary>函数内备注（5 条）</summary>

**第 24 行**

> 内存屏障, 确保所有操作完成

**第 28 行**

> 禁用中断(统一接口；这里只掩 IRQ, 本内核不使用 FIQ/SError)

**第 31 行**

> WFI - Wait For Interrupt

**第 32 行**

> 在中断禁用的情况下, 处理器会永久停止

**第 35 行**

> 如果 WFI 被唤醒(不应该发生), 死循环

</details>

**位置**：`include/aarch64/halt_arch.h:22`

<a id="mod-include-aarch64-mm_vm.h"></a>

## include/aarch64/mm_vm.h — AArch64 现有 VM 实现导出符号

`include/aarch64/mm_vm.h` · 5 个函数 · 5 个有说明 · 73 行

<a id="fn-include-aarch64-mm_vm.h-26"></a>

### `mm_vm_map_pages`

```c
static inline int32_t mm_vm_map_pages(void *page_dir, uint64_t vaddr, uint64_t paddr, int32_t count, vm_perm_t perm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

直接转发给 memory_create_map，按 count 页建立虚拟地址到物理地址的映射。

**位置**：`include/aarch64/mm_vm.h:26`

<a id="fn-include-aarch64-mm_vm.h-31"></a>

### `mm_vm_get_paddr`

```c
static inline uint64_t mm_vm_get_paddr(void *page_dir, uint64_t vaddr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

直接转发给 memory_get_paddr，查询页表中虚拟地址对应的物理地址。

**位置**：`include/aarch64/mm_vm.h:31`

<a id="fn-include-aarch64-mm_vm.h-36"></a>

### `mm_vm_copy_user_space`

```c
static inline int32_t mm_vm_copy_user_space(void *dst_pgd, void *src_pgd)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

直接转发给 memory_copy_uvm_4level，复制用户地址空间的四级页表。

**位置**：`include/aarch64/mm_vm.h:36`

<a id="fn-include-aarch64-mm_vm.h-41"></a>

### `mm_vm_copy_to_uva`

```c
static inline void mm_vm_copy_to_uva(void *pgd, uint64_t user_vaddr, uint64_t src_paddr, uint64_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

调用 copydata_to_uvm 把物理地址处 size 字节数据复制到用户虚拟地址。

**位置**：`include/aarch64/mm_vm.h:41`

<a id="fn-include-aarch64-mm_vm.h-51"></a>

### `mm_vm_set_pte_keep`

```c
static inline void mm_vm_set_pte_keep(void *pgd, uint64_t vaddr)
```

把一页标成"设备缓冲"：unmap 时只拆映射, 不释放页也不碰引用计数。
用于把 dma_buf / ION 的缓冲区映射进用户态 —— 页归句柄所有, 不归映射。
语义与 NOFREE 的区别见 mmu.h 里 PTE_KEEP 的说明。

**位置**：`include/aarch64/mm_vm.h:51`

<a id="mod-include-aarch64-neon.h"></a>

## include/aarch64/neon.h — aarch64_enable_neon - 启用 NEON/浮点指令

`include/aarch64/neon.h` · 4 个函数 · 4 个有说明 · 79 行

在 CPACR_EL1 寄存器中设置 FPEN 位, 允许在 EL0 使用 NEON 和浮点指令。

CPACR_EL1.FPEN [21:20]:
- 0b00: 根据 CPACR_EL1.TTA 决定
- 0b01: 执行 NEON/FP 时 Trap 到 EL1
- 0b10: 执行 NEON/FP 时 Trap 到 EL2
- 0b11: 允许在 EL0 使用 NEON/FP

注意：必须在内核初始化时调用, 否则 NEON 指令会触发异常

<a id="fn-include-aarch64-neon.h-25"></a>

### `aarch64_enable_neon`

```c
static inline void aarch64_enable_neon(void)
```

aarch64_enable_neon - 启用 NEON/浮点指令

在 CPACR_EL1 寄存器中设置 FPEN 位, 允许在 EL0 使用 NEON 和浮点指令。

CPACR_EL1.FPEN [21:20]:
- 0b00: 根据 CPACR_EL1.TTA 决定
- 0b01: 执行 NEON/FP 时 Trap 到 EL1
- 0b10: 执行 NEON/FP 时 Trap 到 EL2
- 0b11: 允许在 EL0 使用 NEON/FP

注意：必须在内核初始化时调用, 否则 NEON 指令会触发异常

<details>
<summary>函数内备注（4 条）</summary>

**第 28 行（行尾）**

> 读取 CPACR_EL1

**第 29 行（行尾）**

> 设置 FPEN[21:20] = 0b11

**第 30 行（行尾）**

> 写回 CPACR_EL1

**第 31 行（行尾）**

> 指令同步屏障, 确保修改生效

</details>

**位置**：`include/aarch64/neon.h:25`

<a id="fn-include-aarch64-neon.h-42"></a>

### `aarch64_get_cpacr`

```c
static inline uint64_t aarch64_get_cpacr(void)
```

aarch64_get_cpacr - 读取 CPACR_EL1 寄存器

返回值: CPACR_EL1 寄存器的当前值

**位置**：`include/aarch64/neon.h:42`

<a id="fn-include-aarch64-neon.h-54"></a>

### `aarch64_is_neon_enabled`

```c
static inline int aarch64_is_neon_enabled(void)
```

aarch64_is_neon_enabled - 检查 NEON 是否已启用

返回值: 1 表示已启用, 0 表示未启用

**位置**：`include/aarch64/neon.h:54`

<a id="fn-include-aarch64-neon.h-67"></a>

### `aarch64_disable_neon`

```c
static inline void aarch64_disable_neon(void)
```

aarch64_disable_neon - 禁用 NEON/浮点指令

将 CPACR_EL1.FPEN 设置为 0b01, 使 NEON/FP 指令触发异常

警告：禁用后执行 NEON 指令会导致异常

<details>
<summary>函数内备注（2 条）</summary>

**第 70 行（行尾）**

> 清除 FPEN 位

**第 71 行（行尾）**

> 设置 FPEN = 0b01

</details>

**位置**：`include/aarch64/neon.h:67`

<a id="mod-include-aarch64-psci.h"></a>

## include/aarch64/psci.h — PSCI 0.2+ 调用封装(SMC64 conduit)

`include/aarch64/psci.h` · 2 个函数 · 2 个有说明 · 76 行

只提供 CPU bring-up 需要的两个功能:
psci_cpu_on          拉起指定 MPIDR 的核
psci_affinity_info   查询核当前状态(超时诊断用)

为什么放在头文件里而不是 .c：两个调用都是"固定寄存器 + 一条 smc"的纯
内联汇编, 没有任何全局状态, 和 arch_cpu_self_get() 一样按架构单独提供
最自然; 放进 .c 只会让每个使用者各写一份 extern。

conduit 约定：本内核用 SMC(EL2/VHE 下由 ATF 转发; QEMU virt 也是 SMC),
功能号取 SMC64 的 0xC40000xx。若以后出现 HVC conduit 的平台, 在这里加
一个 conduit 选择, 不要把 smc/hvc 散到调用点。

<a id="fn-include-aarch64-psci.h-43"></a>

### `psci_cpu_on`

```c
static inline int64_t psci_cpu_on(uint64_t mpidr, uint64_t entry_phys, uint64_t context_id)
```

psci_cpu_on - 拉起一颗核
@mpidr:       目标核的 MPIDR
@entry_phys:  入口的**物理**地址(PSCI 在目标核 MMU 打开前进入)
@context_id:  透传给入口的 context_id(本内核用它传逻辑 CPU 号)

返回: PSCI 返回码, 0 = 成功, 负数 = 错误

**位置**：`include/aarch64/psci.h:43`

<a id="fn-include-aarch64-psci.h-66"></a>

### `psci_affinity_info`

```c
static inline int64_t psci_affinity_info(uint64_t mpidr)
```

psci_affinity_info - 查询核状态
@mpidr: 目标核的 MPIDR

返回:
0   = ON
1   = OFF
2   = ON_PENDING
<0  = 错误(例如 -2 INVALID_PARAMS 表示 ATF 不认该 mpidr)

lowest_affinity_level 固定传 0：本内核按完整的 core 粒度查询。

**位置**：`include/aarch64/psci.h:66`

<a id="mod-include-aarch64-spin_lock_impl.h"></a>

## include/aarch64/spin_lock_impl.h — AArch64 架构的 spinlock 实现

`include/aarch64/spin_lock_impl.h` · 6 个函数 · 6 个有说明 · 96 行

使用 LDAXR/STLXR(acquire/release 语义)实现原子 CAS。
irqsave 变体与 RISC-V/x86_64 保持一致：
先用 C 辅助函数保存并关中断, 再调用 spin_lock。

<a id="fn-include-aarch64-spin_lock_impl.h-22"></a>

### `spin_lock`

```c
static inline void spin_lock(spinlock_t *lock)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先关抢占，用普通 ldr 自旋等待锁空，再用 ldaxr/stlxr 原子取锁，失败重试。

<details>
<summary>函数内备注（4 条）</summary>

**第 27 行**

> 先用普通 ldr 等待锁可用(避免总线锁事务持续占用)

**第 30 行**

> 再用 ldaxr/stlxr 原子取锁

**第 32 行（行尾）**

> 锁被抢走, 回到等待

**第 34 行（行尾）**

> stlxr 失败, 重试

</details>

**位置**：`include/aarch64/spin_lock_impl.h:22`

<a id="fn-include-aarch64-spin_lock_impl.h-40"></a>

### `spin_trylock`

```c
static inline int spin_trylock(spinlock_t *lock)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

关抢占后尝试原子取锁，失败则恢复抢占并返回 1，成功返回 0。

<details>
<summary>函数内备注（3 条）</summary>

**第 47 行（行尾）**

> store 失败 → 返回 1(失败)

**第 48 行（行尾）**

> 成功 → 返回 0

**第 50 行（行尾）**

> 失败 → 返回 1

</details>

**位置**：`include/aarch64/spin_lock_impl.h:40`

<a id="fn-include-aarch64-spin_lock_impl.h-60"></a>

### `spin_unlock`

```c
static inline void spin_unlock(spinlock_t *lock)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 stlr 向锁字写入 0 释放锁（自带 release 语义），随后恢复抢占。

<details>
<summary>函数内备注（1 条）</summary>

**第 62 行**

> stlr 已含 release 语义, 无需额外 dmb

</details>

**位置**：`include/aarch64/spin_lock_impl.h:60`

<a id="fn-include-aarch64-spin_lock_impl.h-75"></a>

### `spin_lock_irqsave`

```c
static inline void spin_lock_irqsave(spinlock_t *lock, uint64_t *flags)
```

中断状态存在调用点的局部变量里(由 *flags 带回), 不再存进锁对象 ——
存进锁对象时, SMP 下争锁的另一颗 CPU 会把它的 flags 覆盖上去, 解锁时
恢复的就是别人的中断状态。中断原语本身统一来自
include/aarch64/exception_impl.h(arch_irq_save/restore)。

<details>
<summary>函数内备注（1 条）</summary>

**第 77 行（行尾）**

> 先关中断, 再自旋

</details>

**位置**：`include/aarch64/spin_lock_impl.h:75`

<a id="fn-include-aarch64-spin_lock_impl.h-81"></a>

### `spin_trylock_irqsave`

```c
static inline int spin_trylock_irqsave(spinlock_t *lock, uint64_t *flags)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先保存并关中断，再尝试取锁；失败则恢复中断并返回 1，成功返回 0。

<details>
<summary>函数内备注（2 条）</summary>

**第 85 行（行尾）**

> 成功

**第 86 行（行尾）**

> 失败则恢复中断

</details>

**位置**：`include/aarch64/spin_lock_impl.h:81`

<a id="fn-include-aarch64-spin_lock_impl.h-90"></a>

### `spin_unlock_irqrestore`

```c
static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先调用 spin_unlock 释放锁，再按 flags 恢复先前的中断状态。

<details>
<summary>函数内备注（2 条）</summary>

**第 92 行（行尾）**

> 先释放锁

**第 93 行（行尾）**

> 再恢复中断

</details>

**位置**：`include/aarch64/spin_lock_impl.h:90`

<a id="mod-include-aarch64-string_impl.h"></a>

## include/aarch64/string_impl.h — AArch64 架构的字符串函数优化实现(NEON)

`include/aarch64/string_impl.h` · 4 个函数 · 4 个有说明 · 146 行

本文件只提供具名实现(memcpy_neon / memset_neon)与架构分派
(memcpy_arch / memset_arch)。公开的 memcpy/memset 名字在 string.h,
外部符号在 lib/string.c —— 两者都转发到这里, 见 string_internal.h 顶部。

前置条件：CPACR.FPEN 已打开。boot/aarch64/boot.S 的 init_el2_vhe 在
装向量表之前就打开了, 早于任何 C 代码, 所以正常路径无需关心。

<a id="fn-include-aarch64-string_impl.h-55"></a>

### `memcpy_neon`

```c
static inline void *memcpy_neon(void *dest, const void *src, size_t n)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先把目标地址对齐到 16 字节，再用 NEON ld1/st1 按 16 字节拷贝，末尾逐字节补齐。

<details>
<summary>函数内备注（5 条）</summary>

**第 61 行**

> 1. 先把目的地址推到 16 字节边界(最多 15 字节)

**第 67 行**

> 2. NEON 128-bit 拷贝

**第 69 行（行尾）**

> 加载 16 字节到 NEON v0

**第 70 行（行尾）**

> 存储 16 字节到 dest

**第 76 行**

> 3. 剩余不足 16 字节拷贝

</details>

**位置**：`include/aarch64/string_impl.h:55`

<a id="fn-include-aarch64-string_impl.h-95"></a>

### `memset_neon`

```c
static inline void *memset_neon(void *s, int c, size_t n)
```

memset_neon - ARM NEON 优化版本
@s: 目标地址
@c: 填充字节
@n: 填充字节数

dup 把字节广播到 16 个 lane, 再用 st1 一次写 16 字节。
与 memcpy_neon 同理, st1 {v0.16b} 按字节访问, 只需目的地址对齐。

<details>
<summary>函数内备注（4 条）</summary>

**第 101 行**

> 1. 先把目的地址推到 16 字节边界

**第 105 行**

> 2. NEON 128-bit 填充

**第 107 行（行尾）**

> 字节广播到 16 个 lane

**第 114 行**

> 3. 剩余不足 16 字节

</details>

**位置**：`include/aarch64/string_impl.h:95`

<a id="fn-include-aarch64-string_impl.h-132"></a>

### `memcpy_arch`

```c
static inline void *memcpy_arch(void *dest, const void *src, size_t n)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

长度超过 MEMCPY_NEON_THRESHOLD 时调用 memcpy_neon，否则调用 memcpy_generic。

**位置**：`include/aarch64/string_impl.h:132`

<a id="fn-include-aarch64-string_impl.h-139"></a>

### `memset_arch`

```c
static inline void *memset_arch(void *s, int c, size_t n)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

长度超过 MEMSET_NEON_THRESHOLD 时调用 memset_neon，否则调用 memset_generic。

**位置**：`include/aarch64/string_impl.h:139`

<a id="mod-include-aarch64-syscall_abi.h"></a>

## include/aarch64/syscall_abi.h

`include/aarch64/syscall_abi.h` · 6 个函数 · 6 个有说明 · 61 行

<a id="fn-include-aarch64-syscall_abi.h-13"></a>

### `syscall_abi_nr`

```c
static inline uint64_t syscall_abi_nr(const trap_frame_t *frame)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从陷阱帧的 x8 寄存器读取系统调用号。

<details>
<summary>函数内备注（1 条）</summary>

**第 15 行（行尾）**

> x8 = syscall number

</details>

**位置**：`include/aarch64/syscall_abi.h:13`

<a id="fn-include-aarch64-syscall_abi.h-18"></a>

### `syscall_abi_arg`

```c
static inline uint64_t syscall_abi_arg(const trap_frame_t *frame, int idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按索引从陷阱帧取 x0~x5 之一作为参数，索引不在 0~5 范围则返回 0。

**位置**：`include/aarch64/syscall_abi.h:18`

<a id="fn-include-aarch64-syscall_abi.h-23"></a>

### `syscall_abi_set_ret`

```c
static inline void syscall_abi_set_ret(trap_frame_t *frame, uint64_t ret)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把返回值写入陷阱帧的 x0 寄存器，作为系统调用返回给用户态。

<details>
<summary>函数内备注（1 条）</summary>

**第 25 行（行尾）**

> x0 = return value

</details>

**位置**：`include/aarch64/syscall_abi.h:23`

<a id="fn-include-aarch64-syscall_abi.h-28"></a>

### `syscall_abi_ip`

```c
static inline uint64_t syscall_abi_ip(const trap_frame_t *frame)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回陷阱帧中保存的 ELR，即系统调用发生时的用户态指令地址。

**位置**：`include/aarch64/syscall_abi.h:28`

<a id="fn-include-aarch64-syscall_abi.h-33"></a>

### `syscall_abi_user_sp`

```c
static inline uint64_t syscall_abi_user_sp(const trap_frame_t *frame)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回陷阱帧中保存的用户栈指针 usp。

**位置**：`include/aarch64/syscall_abi.h:33`

<a id="fn-include-aarch64-syscall_abi.h-46"></a>

### `syscall_abi_reset_frame_for_exec`

```c
static inline void syscall_abi_reset_frame_for_exec(trap_frame_t *frame, uint64_t entry, uint64_t sp)
```

syscall_abi_reset_frame_for_exec - execve 成功后把 trap frame 重置成
新程序的初始用户现场

从 syscall 返回时会整体恢复 trap frame，所以原地 exec 只需要改这里：
入口、栈、通用寄存器、TLS、FP/SIMD 状态全部换成"刚 exec 的新进程"。
SPSR 用 EL0t + IRQ 使能，与 arch_switch_to_user 首次进入保持一致。

<details>
<summary>函数内备注（2 条）</summary>

**第 52 行（行尾）**

> EL0t, IRQ 使能

**第 53 行（行尾）**

> exec 不继承 TLS

</details>

**位置**：`include/aarch64/syscall_abi.h:46`

<a id="mod-include-bitmap.h"></a>

## include/bitmap.h — 位图管理

`include/bitmap.h` · 6 个函数 · 6 个有说明 · 149 行

用于物理内存管理的位图数据结构

<a id="fn-include-bitmap.h-35"></a>

### `bitmap_init`

```c
static inline void bitmap_init(bitmap_t *bitmap, uint8_t *buffer, size_t size)
```

bitmap_init - 初始化位图
@bitmap: 位图结构指针
@buffer: 位图缓冲区
@size:   位图大小(bit 数)

使用前必须调用此函数初始化位图。

<details>
<summary>函数内备注（1 条）</summary>

**第 40 行**

> 清空所有位

</details>

**位置**：`include/bitmap.h:35`

<a id="fn-include-bitmap.h-54"></a>

### `bitmap_set`

```c
static inline void bitmap_set(bitmap_t *bitmap, size_t index)
```

bitmap_set - 设置指定索引的位(标记为已分配)
@bitmap: 位图结构指针
@index:  位索引

注意：假设该位当前为 0(未分配)

**位置**：`include/bitmap.h:54`

<a id="fn-include-bitmap.h-69"></a>

### `bitmap_set_range`

```c
static inline void bitmap_set_range(bitmap_t *bitmap, size_t start, size_t count)
```

bitmap_set_range - 设置指定范围的位
@bitmap: 位图结构指针
@start:  起始索引
@count:  位数量

标记多个连续的位为已分配。

<details>
<summary>函数内备注（1 条）</summary>

**第 72 行（行尾）**

> 超出范围

</details>

**位置**：`include/bitmap.h:69`

<a id="fn-include-bitmap.h-85"></a>

### `bitmap_clear`

```c
static inline void bitmap_clear(bitmap_t *bitmap, size_t index)
```

bitmap_clear - 清除指定索引的位(标记为未分配)
@bitmap: 位图结构指针
@index:  位索引

**位置**：`include/bitmap.h:85`

<a id="fn-include-bitmap.h-98"></a>

### `bitmap_clear_range`

```c
static inline void bitmap_clear_range(bitmap_t *bitmap, size_t start, size_t count)
```

bitmap_clear_range - 清除指定范围的位
@bitmap: 位图结构指针
@start:  起始索引
@count:  位数量

<details>
<summary>函数内备注（1 条）</summary>

**第 101 行（行尾）**

> 超出范围

</details>

**位置**：`include/bitmap.h:98`

<a id="fn-include-bitmap.h-116"></a>

### `bitmap_test`

```c
static inline uint8_t bitmap_test(const bitmap_t *bitmap, size_t index)
```

bitmap_test - 测试指定索引的位
@bitmap: 位图结构指针
@index:  位索引

返回：非零表示已分配, 0 表示未分配

**位置**：`include/bitmap.h:116`

<a id="mod-include-cache.h"></a>

## include/cache.h — 跨架构缓存操作抽象层

`include/cache.h` · 3 个函数 · 3 个有说明 · 175 行

支持数据缓存(D-cache)的清理, 失效等操作

<a id="fn-include-cache.h-110"></a>

### `clean_dcache_range`

```c
static inline void clean_dcache_range(const void *addr, size_t size)
```

clean_dcache_range - 清理地址范围的缓存

<details>
<summary>函数内备注（2 条）</summary>

**第 116 行（行尾）**

> 确保之前的所有内存访问完成

**第 121 行（行尾）**

> 确保 clean 完成

</details>

**位置**：`include/cache.h:110`

<a id="fn-include-cache.h-127"></a>

### `invalidate_dcache_range`

```c
static inline void invalidate_dcache_range(const void *addr, size_t size)
```

invalidate_dcache_range - 使地址范围的缓存失效

<details>
<summary>函数内备注（3 条）</summary>

**第 134 行（行尾）**

> CPU 发出所有写入到范围

**第 136 行**

> 对齐到缓存行边界

**第 153 行（行尾）**

> 确保失效完成

</details>

**位置**：`include/cache.h:127`

<a id="fn-include-cache.h-159"></a>

### `clean_and_invalidate_dcache_range`

```c
static inline void clean_and_invalidate_dcache_range(const void *addr, size_t size)
```

clean_and_invalidate_dcache_range - 清理并使地址范围的缓存失效

**位置**：`include/cache.h:159`

<a id="mod-include-driver-dma_buf.h"></a>

## include/driver/dma_buf.h — dma-buf / dma-heap

`include/driver/dma_buf.h` · 16 个函数 · 0 个有说明 · 217 行

背景：官方 `librknnrt.so` 的每个缓冲区都走这条路 ——

open("/dev/dma_heap/<heap>") → DMA_HEAP_IOCTL_ALLOC  → dmabuf fd
DMA_BUF_IOCTL_SYNC(打在那个 fd 上)              → 缓存维护
mmap(dmabuf fd)                                      → CPU 访问

所以这不是"给 NPU 用的内部工具", 而是一条用户态可见的 ABI。

── 分层(与本项目 ion 的写法一致)────────────────────────────────
driver/dma/dma_heap.c   内存从哪来(P0: pmm_alloc_pages；P1: 预留区)
driver/dma/dma_buf.c    缓冲区对象 · 引用计数 · 缓存维护 · fd 绑定
kernel/fs/pseudofs/dev.c  ABI 处理(用户指针 ← 一律走 copy_*_user)

── 设备地址只有这一个出口 ────────────────────────────────────────
`dma_buf_dev_addr()` 是全系统唯一把缓冲区变成"给设备看的地址"的地方。
现在它返回物理地址(无 IOMMU)；将来上 IOMMU 时改成返回 IOVA 即可,
上层(NPU 作业描述符, DMA 基址寄存器)一行都不用动。
绕过它直接用 phys_to_virt / paddr 的地方, 将来都要改两遍。

<a id="fn-include-driver-dma_buf.h-69"></a>

### `dma_buf_present`

```c
static inline bool dma_buf_present(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/driver/dma_buf.h:69`

<a id="fn-include-driver-dma_buf.h-76"></a>

### `dma_buf_alloc`

```c
static inline dma_buf_t *dma_buf_alloc(size_t size)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/driver/dma_buf.h:76`

<a id="fn-include-driver-dma_buf.h-81"></a>

### `dma_buf_get`

```c
static inline void dma_buf_get(dma_buf_t *buf)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/driver/dma_buf.h:81`

<a id="fn-include-driver-dma_buf.h-87"></a>

### `dma_buf_put`

```c
static inline void dma_buf_put(dma_buf_t *buf)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/driver/dma_buf.h:87`

<a id="fn-include-driver-dma_buf.h-95"></a>

### `dma_buf_dev_addr`

```c
static inline uint64_t dma_buf_dev_addr(const dma_buf_t *buf)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/driver/dma_buf.h:95`

<a id="fn-include-driver-dma_buf.h-100"></a>

### `dma_buf_kva`

```c
static inline void *dma_buf_kva(const dma_buf_t *buf)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/driver/dma_buf.h:100`

<a id="fn-include-driver-dma_buf.h-105"></a>

### `dma_buf_size`

```c
static inline size_t dma_buf_size(const dma_buf_t *buf)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/driver/dma_buf.h:105`

<a id="fn-include-driver-dma_buf.h-110"></a>

### `dma_buf_find_by_phys`

```c
static inline dma_buf_t *dma_buf_find_by_phys(uint64_t pa, uint64_t *off)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/driver/dma_buf.h:110`

<a id="fn-include-driver-dma_buf.h-116"></a>

### `dma_buf_from_index`

```c
static inline dma_buf_t *dma_buf_from_index(uint32_t idx)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/driver/dma_buf.h:116`

<a id="fn-include-driver-dma_buf.h-121"></a>

### `dma_buf_index`

```c
static inline uint32_t dma_buf_index(const dma_buf_t *buf)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/driver/dma_buf.h:121`

<a id="fn-include-driver-dma_buf.h-126"></a>

### `dma_buf_get_index`

```c
static inline void dma_buf_get_index(uint32_t idx)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/driver/dma_buf.h:126`

<a id="fn-include-driver-dma_buf.h-132"></a>

### `dma_buf_put_index`

```c
static inline void dma_buf_put_index(uint32_t idx)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/driver/dma_buf.h:132`

<a id="fn-include-driver-dma_buf.h-140"></a>

### `dma_buf_sync_region`

```c
static inline void dma_buf_sync_region(dma_buf_t *buf, uint64_t off, uint64_t len, bool to_device)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/driver/dma_buf.h:140`

<a id="fn-include-driver-dma_buf.h-148"></a>

### `dma_buf_alloc_fd_current`

```c
static inline int dma_buf_alloc_fd_current(dma_buf_t *buf)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/driver/dma_buf.h:148`

<a id="fn-include-driver-dma_buf.h-153"></a>

### `dma_buf_from_user_fd`

```c
static inline dma_buf_t *dma_buf_from_user_fd(int fd)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/driver/dma_buf.h:153`

<a id="fn-include-driver-dma_buf.h-158"></a>

### `dma_buf_apply_sync`

```c
static inline int dma_buf_apply_sync(uint32_t idx, uint64_t flags)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/driver/dma_buf.h:158`

<a id="mod-include-driver-ion.h"></a>

## include/driver/ion.h — 裸机 Ion 内存分配器公共接口

`include/driver/ion.h` · 6 个函数 · 6 个有说明 · 86 行

参考：Android ION + ref: rcore-os/tgoskits sg2002-tpu/src/ion/

功能：从 PMM 分配物理连续页, 映射到内核虚拟地址, 返回句柄。
典型用途：为 TPU DMA 缓冲区分配物理连续内存。

使用：
void *va; uint64_t pa; ion_handle_t h;
if (ion_alloc(size, &va, &pa, &h) == 0) {
DMA 操作使用 pa, CPU 访问使用 va
cvi_tpu_run_dmabuf(va, pa);
ion_free(h);
}

<a id="fn-include-driver-ion.h-56"></a>

### `ion_present`

```c
static inline bool ion_present(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断全局 ion 操作表 g_ion_ops 是否已注册。

**位置**：`include/driver/ion.h:56`

<a id="fn-include-driver-ion.h-61"></a>

### `ion_alloc`

```c
static inline int ion_alloc(size_t size, void **vaddr, uint64_t *paddr, ion_handle_t *handle)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

ion 操作表存在时调用其 alloc 分配内存并输出虚拟/物理地址与句柄，否则返回 -1。

**位置**：`include/driver/ion.h:61`

<a id="fn-include-driver-ion.h-66"></a>

### `ion_free`

```c
static inline int ion_free(ion_handle_t handle)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

ion 操作表存在时调用 free 释放指定句柄，否则返回 -1。

**位置**：`include/driver/ion.h:66`

<a id="fn-include-driver-ion.h-71"></a>

### `ion_ref`

```c
static inline int ion_ref(ion_handle_t handle)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

ion 操作表存在时调用 ref 增加句柄引用，否则返回 -1。

**位置**：`include/driver/ion.h:71`

<a id="fn-include-driver-ion.h-76"></a>

### `ion_get_buf`

```c
static inline int ion_get_buf(ion_handle_t handle, void **vaddr, uint64_t *paddr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

ion 操作表存在时调用 get_buf 取出句柄的虚拟与物理地址，否则返回 -1。

**位置**：`include/driver/ion.h:76`

<a id="fn-include-driver-ion.h-81"></a>

### `ion_get_size`

```c
static inline size_t ion_get_size(ion_handle_t handle)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

ion 操作表存在时返回 get_size 给出的缓冲区大小，否则返回 0。

**位置**：`include/driver/ion.h:81`

<a id="mod-include-driver-irq.h"></a>

## include/driver/irq.h — 中断控制器公共接口

`include/driver/irq.h` · 1 个函数 · 1 个有说明 · 111 行

驱动/内核只依赖这里的接口, 不再包含 irq/gicv3.h、irq/gicv2.h、
irq/plic.h、irq/lapic.h 等具体控制器头文件。

分工：
- 控制器后端(GICv2/GICv3/PLIC/LAPIC)在 probe 时 irq_bind(&ops)；
- 设备驱动用 irq_register()/irq_enable()/irq_set_priority() 注册自己的
中断线，号空间就是硬件号(GIC INTID / PLIC source / IDT vector)；
- 架构异常入口只调用 irq_dispatch_frame(frame)，由后端完成
ack → 查表 → handler → eoi/complete 的全过程。

处理器签名统一为 (frame, ctx)：frame 是架构 trap frame，设备驱动一般
忽略；ctx 是注册时传入的私有指针，免去每根中断线写一个薄包装。

<a id="fn-include-driver-irq.h-80"></a>

### `irq_available`

```c
static inline bool irq_available(void)
```

是否有中断控制器后端；未绑定时所有接口返回 -1。

**位置**：`include/driver/irq.h:80`

<a id="mod-include-driver-npu.h"></a>

## include/driver/npu.h — NPU 平台配置与公共接口

`include/driver/npu.h` · 14 个函数 · 14 个有说明 · 151 行

这里只放 VMM 透传和通用内核路径需要的东西：核数、寄存器偏移、
宿主让出/收回、地址翻译。具体驱动的任务描述符/提交路径仍在
driver/npu/rknpu.h（私有实现细节）。

<a id="fn-include-driver-npu.h-71"></a>

### `npu_present`

```c
static inline bool npu_present(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断全局 NPU 操作表 g_npu_ops 是否已注册。

**位置**：`include/driver/npu.h:71`

<a id="fn-include-driver-npu.h-76"></a>

### `rknpu_host_release`

```c
static inline void rknpu_host_release(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

NPU 操作表存在时调用 host_release 回调释放宿主侧占用，否则直接返回。

**位置**：`include/driver/npu.h:76`

<a id="fn-include-driver-npu.h-82"></a>

### `rknpu_host_reclaim`

```c
static inline void rknpu_host_reclaim(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

NPU 操作表存在时调用 host_reclaim 回调回收宿主侧资源。

**位置**：`include/driver/npu.h:82`

<a id="fn-include-driver-npu.h-88"></a>

### `npu_mmu_init`

```c
static inline int npu_mmu_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

NPU 操作表存在时调用 mmu_init 初始化 NPU MMU，否则返回 -1。

**位置**：`include/driver/npu.h:88`

<a id="fn-include-driver-npu.h-93"></a>

### `npu_mmu_enable`

```c
static inline int npu_mmu_enable(uint64_t ipa_base, uint64_t ipa_size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

操作表存在时以 IPA 基址和大小调用 mmu_enable 启用 NPU MMU，否则返回 -1。

**位置**：`include/driver/npu.h:93`

<a id="fn-include-driver-npu.h-98"></a>

### `npu_mmu_disable`

```c
static inline void npu_mmu_disable(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

操作表存在时调用 mmu_disable 关闭 NPU MMU。

**位置**：`include/driver/npu.h:98`

<a id="fn-include-driver-npu.h-104"></a>

### `npu_mmu_sync`

```c
static inline int npu_mmu_sync(struct vm *vm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

操作表存在时调用 mmu_sync 同步指定 vm 的映射，否则返回 0。

**位置**：`include/driver/npu.h:104`

<a id="fn-include-driver-npu.h-109"></a>

### `npu_mmu_clear_fault`

```c
static inline void npu_mmu_clear_fault(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

操作表存在时调用 mmu_clear_fault 清除 NPU MMU 故障状态。

**位置**：`include/driver/npu.h:109`

<a id="fn-include-driver-npu.h-115"></a>

### `npu_mmu_dump`

```c
static inline void npu_mmu_dump(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

操作表存在时调用 mmu_dump 打印 NPU MMU 状态。

**位置**：`include/driver/npu.h:115`

<a id="fn-include-driver-npu.h-121"></a>

### `npu_mmu_any_fault`

```c
static inline bool npu_mmu_any_fault(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

操作表存在时返回 mmu_any_fault 的查询结果，否则返回 false。

**位置**：`include/driver/npu.h:121`

<a id="fn-include-driver-npu.h-126"></a>

### `npu_mmu_probe`

```c
static inline void npu_mmu_probe(struct vm *vm, uint64_t ipa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

操作表存在时以 vm 和 IPA 调用 mmu_probe 探测单条映射。

**位置**：`include/driver/npu.h:126`

<a id="fn-include-driver-npu.h-136"></a>

### `rknpu_drm_ioctl_file`

```c
static inline int rknpu_drm_ioctl_file(void *file, uint64_t req, void *argp)
```

DRM 兼容入口。pseudofs 的静态节点表和 sys_mmap 直接引用这三个名字；
没编 NPU 时它们是普通内联函数（返回 ENOSYS/-1），没有额外符号。

**位置**：`include/driver/npu.h:136`

<a id="fn-include-driver-npu.h-141"></a>

### `rknpu_drm_close_file`

```c
static inline int rknpu_drm_close_file(void *file)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

操作表存在时调用 drm_close_file 处理文件关闭，否则返回 0。

**位置**：`include/driver/npu.h:141`

<a id="fn-include-driver-npu.h-146"></a>

### `rknpu_drm_mmap_lookup`

```c
static inline int rknpu_drm_mmap_lookup(void *file, uint64_t offset, uint64_t *paddr_out, uint64_t *size_out)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

操作表存在时通过 drm_mmap_lookup 由文件与偏移查物理地址和大小，否则返回 -1。

**位置**：`include/driver/npu.h:146`

<a id="mod-include-driver-serial.h"></a>

## include/driver/serial.h — 串口公共接口

`include/driver/serial.h` · 5 个函数 · 5 个有说明 · 65 行

内核只依赖本文件的 serial_* API；具体后端（pl011/dw/x86）在 probe 时
通过 serial_bind() 交出 ops。未绑定前 g_serial_ops 指向 null 实现，
早期 boot 不会因为空指针而崩。

<a id="fn-include-driver-serial.h-40"></a>

### `serial_putc`

```c
static inline void serial_putc(char c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

不经空检查直接调用全局串口操作的 putc 输出一个字符。

**位置**：`include/driver/serial.h:40`

<a id="fn-include-driver-serial.h-45"></a>

### `serial_puts`

```c
static inline void serial_puts(const char *s)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

不经空检查直接调用全局串口操作的 puts 输出字符串。

**位置**：`include/driver/serial.h:45`

<a id="fn-include-driver-serial.h-50"></a>

### `serial_getc`

```c
static inline char serial_getc(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

不经空检查直接调用全局串口操作的 getc 读取一个字符并返回。

**位置**：`include/driver/serial.h:50`

<a id="fn-include-driver-serial.h-55"></a>

### `serial_getc_nb`

```c
static inline bool serial_getc_nb(char *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

直接调用全局串口操作的 getc_nb，非阻塞读字符并返回是否成功。

**位置**：`include/driver/serial.h:55`

<a id="fn-include-driver-serial.h-60"></a>

### `serial_rx_ready`

```c
static inline bool serial_rx_ready(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

直接调用全局串口操作的 rx_ready，返回接收数据是否就绪。

**位置**：`include/driver/serial.h:60`

<a id="mod-include-driver-time.h"></a>

## include/driver/time.h — 时间基准公共接口

`include/driver/time.h` · 1 个函数 · 1 个有说明 · 163 行

内核只依赖本头：tick/uptime/延时/硬件计数器读取都从这里取。
具体实现由 driver/time/timer.c 按 ARCH 选择，驱动 probe 负责初始化。

<a id="fn-include-driver-time.h-158"></a>

### `timer_get_ns`

```c
static inline uint64_t timer_get_ns(void)
```

timer_get_ns - 获取当前单调时间(纳秒)

使用硬件计数器(RISC-V rdtime / AArch64 cntpct_el0 / x86_64 TSC)。
可在中断、syscall 内核路径等任意上下文中安全调用。

**位置**：`include/driver/time.h:158`

<a id="mod-include-driver-tpu.h"></a>

## include/driver/tpu.h — TPU 的平台配置

`include/driver/tpu.h` · 4 个函数 · 4 个有说明 · 56 行

<a id="fn-include-driver-tpu.h-35"></a>

### `tpu_present`

```c
static inline bool tpu_present(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断全局 TPU 操作表 g_tpu_ops 是否已注册。

**位置**：`include/driver/tpu.h:35`

<a id="fn-include-driver-tpu.h-40"></a>

### `cvi_tpu_init`

```c
static inline void cvi_tpu_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

TPU 操作表存在时调用 init 回调初始化 TPU。

**位置**：`include/driver/tpu.h:40`

<a id="fn-include-driver-tpu.h-46"></a>

### `cvi_tpu_is_ready`

```c
static inline bool cvi_tpu_is_ready(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

TPU 操作表存在时返回 is_ready 结果，否则返回 false。

**位置**：`include/driver/tpu.h:46`

<a id="fn-include-driver-tpu.h-51"></a>

### `cvi_tpu_run_dmabuf`

```c
static inline int cvi_tpu_run_dmabuf(void *dmabuf_v, uint64_t dmabuf_p)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

TPU 操作表存在时以虚拟/物理地址调用 run_dmabuf 提交推理，否则返回 -1。

**位置**：`include/driver/tpu.h:51`

<a id="mod-include-elf.h"></a>

## include/elf.h — ELF 文件格式定义

`include/elf.h` · 4 个函数 · 4 个有说明 · 166 行

<a id="fn-include-elf.h-138"></a>

### `elf_check_magic`

```c
static inline int elf_check_magic(elf64_ehdr_t *ehdr)
```

工具函数

**位置**：`include/elf.h:138`

<a id="fn-include-elf.h-143"></a>

### `elf_check_class`

```c
static inline int elf_check_class(elf64_ehdr_t *ehdr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

检查 ELF 头的 e_ident[4] 是否等于 ELFCLASS64。

**位置**：`include/elf.h:143`

<a id="fn-include-elf.h-148"></a>

### `elf_check_data`

```c
static inline int elf_check_data(elf64_ehdr_t *ehdr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

检查 ELF 头的 e_ident[5] 是否等于 ELFDATA2LSB，即小端格式。

**位置**：`include/elf.h:148`

<a id="fn-include-elf.h-153"></a>

### `elf_check_machine`

```c
static inline int elf_check_machine(elf64_ehdr_t *ehdr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按编译架构比较 e_machine：AArch64 用 EM_AARCH64，x86_64 用 62，RISC-V 用 243。

<details>
<summary>函数内备注（2 条）</summary>

**第 158 行（行尾）**

> EM_X86_64

**第 160 行（行尾）**

> EM_RISCV

</details>

**位置**：`include/elf.h:153`

<a id="mod-include-fs-vfs.h"></a>

## include/fs/vfs.h — small kernel VFS layer

`include/fs/vfs.h` · 2 个函数 · 0 个有说明 · 124 行

First-stage VFS abstraction: fd objects point at struct vfs_file, and
operations dispatch through file_ops. Path routing uses a small mount table:
/dev, /proc and /sys route to pseudofs, / routes to ext4, and an optional
MBR FAT partition can be registered at FAT_MOUNT_POINT (default /boot).

<a id="fn-include-fs-vfs.h-114"></a>

### `vfs_file_is_regular`

```c
static inline bool vfs_file_is_regular(const vfs_file_t *file)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/fs/vfs.h:114`

<a id="fn-include-fs-vfs.h-119"></a>

### `vfs_file_is_dir`

```c
static inline bool vfs_file_is_dir(const vfs_file_t *file)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/fs/vfs.h:119`

<a id="mod-include-klog.h"></a>

## include/klog.h — 内核日志系统

`include/klog.h` · 7 个函数 · 7 个有说明 · 640 行

支持全局日志级别和模块级别的日志控制

两种控制手段, 配合使用：
1. 级别(编译期 Makefile 的 LOG=, 运行期 set_log_level())
—— 决定 DEBUG/TRACE 这些宏体是否存在, 初始门槛多高；
2. 模块掩码(编译期 Makefile 的 LOG_MODULES=, 运行期 log_set_modules_by_name())
—— 在 DEBUG/TRACE 级别上再筛一层, 只放行你关心的模块。

典型用法：定位 GIC 相关问题时
make PLATFORM=qemu-virt-aarch64 run LOG=debug LOG_MODULES=gic
这样只有打了 GIC 模块标签的日志会输出。

<a id="fn-include-klog.h-54"></a>

### `set_log_level`

```c
static inline void set_log_level(log_level_t level)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把全局日志级别 g_log_level 设置为给定级别。

**位置**：`include/klog.h:54`

<a id="fn-include-klog.h-59"></a>

### `get_log_level`

```c
static inline log_level_t get_log_level(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回当前全局日志级别 g_log_level。

**位置**：`include/klog.h:59`

<a id="fn-include-klog.h-103"></a>

### `log_set_modules`

```c
static inline void log_set_modules(uint64_t mask)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用给定掩码整体替换全局日志模块掩码 g_log_module_mask。

**位置**：`include/klog.h:103`

<a id="fn-include-klog.h-108"></a>

### `log_add_module`

```c
static inline void log_add_module(uint64_t module)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把指定模块位或入全局日志模块掩码 g_log_module_mask。

**位置**：`include/klog.h:108`

<a id="fn-include-klog.h-113"></a>

### `log_remove_module`

```c
static inline void log_remove_module(uint64_t module)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从全局日志模块掩码 g_log_module_mask 中清除指定模块位。

**位置**：`include/klog.h:113`

<a id="fn-include-klog.h-118"></a>

### `log_is_module_enabled`

```c
static inline int log_is_module_enabled(uint64_t module)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

检查指定模块位是否包含在当前日志模块掩码中。

**位置**：`include/klog.h:118`

<a id="fn-include-klog.h-152"></a>

### `klog_sample_hit`

```c
static inline int klog_sample_hit(uint32_t n)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断序号 n 是否命中采样：n 不超过 KLOG_SAMPLE_FIRST，或不超过上限且为 2 的幂。

**位置**：`include/klog.h:152`

<a id="mod-include-libc-string.h"></a>

## include/libc/string.h — 实现层(memcpy_generic / memcpy_arch / memcpy_neon …)在这里。

`include/libc/string.h` · 14 个函数 · 1 个有说明 · 166 行

与公开 API 分开的原因见 string_internal.h 顶部：lib/string.c 要用同样的
名字定义外部符号, 实现不能占用公开名字。

<a id="fn-include-libc-string.h-22"></a>

### `strlen`

```c
static inline size_t strlen(const char *buf)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/libc/string.h:22`

<a id="fn-include-libc-string.h-30"></a>

### `strcat`

```c
static inline char *strcat(char *dest, const char *src)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/libc/string.h:30`

<a id="fn-include-libc-string.h-40"></a>

### `strcpy`

```c
static inline char *strcpy(char *dest, const char *src)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/libc/string.h:40`

<a id="fn-include-libc-string.h-48"></a>

### `strncpy`

```c
static inline char *strncpy(char *dest, const char *src, size_t n)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/libc/string.h:48`

<a id="fn-include-libc-string.h-58"></a>

### `strncmp`

```c
static inline int strncmp(const char *a, const char *b, size_t n)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/libc/string.h:58`

<a id="fn-include-libc-string.h-66"></a>

### `strcmp`

```c
static inline int strcmp(const char *a, const char *b)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/libc/string.h:66`

<a id="fn-include-libc-string.h-71"></a>

### `strchr`

```c
static inline char *strchr(const char *s, int c)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/libc/string.h:71`

<a id="fn-include-libc-string.h-79"></a>

### `memcmp`

```c
static inline int memcmp(const void *s1, const void *s2, size_t n)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/libc/string.h:79`

<a id="fn-include-libc-string.h-92"></a>

### `strstr`

```c
static inline char *strstr(const char *s1, const char *s2)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/libc/string.h:92`

<a id="fn-include-libc-string.h-108"></a>

### `memset`

```c
static inline void *memset(void *s, int c, size_t n)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/libc/string.h:108`

<a id="fn-include-libc-string.h-113"></a>

### `memmove`

```c
static inline void *memmove(void *dest, const void *src, size_t n)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/libc/string.h:113`

<a id="fn-include-libc-string.h-118"></a>

### `memchr`

```c
static inline void *memchr(const void *s, int c, size_t n)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/libc/string.h:118`

<a id="fn-include-libc-string.h-127"></a>

### `atol`

```c
static inline long atol(const char *ptr)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/libc/string.h:127`

<a id="fn-include-libc-string.h-161"></a>

### `memcpy`

```c
static inline void *memcpy(void *dest, const void *src, size_t n)
```

公开 API 一律转发到实现层(string_internal.h 里的 *_arch)。
lib/string.c 用同样的 *_arch 定义外部符号, 所以源码里的调用和编译器自动
生成的调用(大结构体赋值, 循环→memcpy 变换)走的是同一份实现。

**位置**：`include/libc/string.h:161`

<a id="mod-include-libc-string_internal.h"></a>

## include/libc/string_internal.h — 字符串/内存函数的实现层

`include/libc/string_internal.h` · 5 个函数 · 4 个有说明 · 196 行

为什么实现要和 string.h 分家：

公开 API(string.h 里的 `static inline memcpy`/`memset`)占用了这两个名字,
而且是内部链接。lib/string.c 必须把同样的名字定义成外部符号, 供编译器
自动生成的调用使用(大结构体整体赋值, 循环→memcpy 变换等, GCC 会自己发
`bl memcpy`)。同一个翻译单元里同名标识符既有内部链接又有外部链接是未定义
行为(C11 6.2.2p7), 所以只能把"实现"和"公开名字"拆开：

string_internal.h  实现：memcpy_generic / memcpy_arch / memcpy_neon …
string.h           公开 API：static inline 转发到 *_arch
lib/string.c       外部符号：同样转发到 *_arch

这样"源码里写的 memcpy"和"编译器自己生成的 memcpy 调用"落到同一份实现。
在此之前两者是分裂的：前者拿到内联的优化版本, 后者落到 lib/string.c 里那个
逐字节循环(实测 fork/clone 路径的 816 字节 trap frame 拷贝就是后者)。

架构约定 —— 每个架构在自己的 string_impl.h 里提供两个函数：
memcpy_arch(dst, src, n)    本架构的 memcpy(可以是阈值分派, 也可以是通用实现)
memset_arch(dst, c, n)      同上
名字刻意不叫 memcpy/memset：那样又会和公开 API 撞名, 回到老问题。

<a id="fn-include-libc-string_internal.h-53"></a>

### `memcpy_generic`

```c
static inline void *memcpy_generic(void *dest, const void *src, size_t n)
```

memcpy_generic - 通用 memcpy

为什么宽访问要求 src/dest 同时对齐：这里用的是普通 GPR 的 8/4/2 字节
访问, 而不是 NEON 那种按字节访问的 ld1/st1。RISC-V(尤其 SG2002/C906 这类
真实硬件)不保证支持非对齐访问, 所以只能两边都对齐才敢加宽。

代价：src/dest 的奇偶不同时(相对偏移是奇数), 无论推进多少字节两者都
不可能同时对齐 —— 这种情况会退化成逐字节拷贝。这是该约束下的必然结果, 不是
缺陷；要再快就得做"两次对齐读取 + 移位拼合", 或者确认目标平台允许非对齐访问。
(AArch64 的 memcpy_neon 没有这个问题：ld1/st1 {v0.16b} 按字节访问, 只需要
对齐目的地址。)

<details>
<summary>函数内备注（5 条）</summary>

**第 67 行**

> 逐字节拷贝直到两边都 2 字节对齐

**第 73 行**

> 8 字节拷贝

**第 82 行**

> 4 字节拷贝

**第 91 行**

> 2 字节拷贝

**第 100 行**

> 剩余逐字节拷贝

</details>

**位置**：`include/libc/string_internal.h:53`

<a id="fn-include-libc-string_internal.h-112"></a>

### `memset_generic`

```c
static inline void *memset_generic(void *s, int c, size_t n)
```

memset_generic - 通用 memset(按字宽填充, 同样要求目的地址对齐后才加宽)

<details>
<summary>函数内备注（2 条）</summary>

**第 124 行**

> 先把目的地址推到 2 字节边界

**第 128 行**

> 8 字节填充(源是常量, 不存在&quot;两边对齐&quot;问题, 只需目的对齐)

</details>

**位置**：`include/libc/string_internal.h:112`

<a id="fn-include-libc-string_internal.h-156"></a>

### `memmove_generic`

```c
static inline void *memmove_generic(void *dest, const void *src, size_t n)
```

memmove_generic - 通用 memmove(重叠时反向拷贝)

<details>
<summary>函数内备注（2 条）</summary>

**第 164 行**

> 目的在源之前(或不相交)：正向拷贝, 可复用 memcpy 的加宽路径

**第 168 行**

> 目的在源之后：从尾部反向拷贝, 避免自己覆盖自己

</details>

**位置**：`include/libc/string_internal.h:156`

<a id="fn-include-libc-string_internal.h-185"></a>

### `memcpy_arch`

```c
static inline void *memcpy_arch(void *dest, const void *src, size_t n)
```

未知架构：直接用通用实现

**位置**：`include/libc/string_internal.h:185`

<a id="fn-include-libc-string_internal.h-190"></a>

### `memset_arch`

```c
static inline void *memset_arch(void *s, int c, size_t n)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/libc/string_internal.h:190`

<a id="mod-include-list.h"></a>

## include/list.h — 双向链表实现

`include/list.h` · 14 个函数 · 14 个有说明 · 286 行

所有函数都是内联的, 无链接冲突

<a id="fn-include-list.h-34"></a>

### `list_node_init`

```c
static inline void list_node_init(list_node_t *node)
```

list_node_init - 初始化链表节点
@node: 节点指针

**位置**：`include/list.h:34`

<a id="fn-include-list.h-45"></a>

### `list_node_pre`

```c
static inline list_node_t *list_node_pre(list_node_t *node)
```

list_node_pre - 获取节点的前驱
@node: 节点指针

返回值: 前驱节点指针

**位置**：`include/list.h:45`

<a id="fn-include-list.h-56"></a>

### `list_node_next`

```c
static inline list_node_t *list_node_next(list_node_t *node)
```

list_node_next - 获取节点的后继
@node: 节点指针

返回值: 后继节点指针

**位置**：`include/list.h:56`

<a id="fn-include-list.h-69"></a>

### `list_is_empty`

```c
static inline int32_t list_is_empty(list_t *list)
```

list_is_empty - 检查链表是否为空
@list: 链表指针

返回值: 1 表示空, 0 表示非空

**位置**：`include/list.h:69`

<a id="fn-include-list.h-80"></a>

### `list_count`

```c
static inline int32_t list_count(list_t *list)
```

list_count - 获取链表节点数量
@list: 链表指针

返回值: 节点数量

**位置**：`include/list.h:80`

<a id="fn-include-list.h-91"></a>

### `list_first`

```c
static inline list_node_t *list_first(list_t *list)
```

list_first - 获取链表首节点
@list: 链表指针

返回值: 首节点指针, 空链表返回 NULL

**位置**：`include/list.h:91`

<a id="fn-include-list.h-102"></a>

### `list_last`

```c
static inline list_node_t *list_last(list_t *list)
```

list_last - 获取链表尾节点
@list: 链表指针

返回值: 尾节点指针, 空链表返回 NULL

**位置**：`include/list.h:102`

<a id="fn-include-list.h-113"></a>

### `list_init`

```c
static inline void list_init(list_t *list)
```

list_init - 初始化链表
@list: 链表指针

**位置**：`include/list.h:113`

<a id="fn-include-list.h-124"></a>

### `list_insert_first`

```c
static inline void list_insert_first(list_t *list, list_node_t *node)
```

list_insert_first - 在链表头部插入节点
@list: 链表指针
@node: 要插入的节点(必须已初始化)

**位置**：`include/list.h:124`

<a id="fn-include-list.h-144"></a>

### `list_insert_last`

```c
static inline void list_insert_last(list_t *list, list_node_t *node)
```

list_insert_last - 在链表尾部插入节点
@list: 链表指针
@node: 要插入的节点(必须已初始化, 且不在链表中)

**位置**：`include/list.h:144`

<a id="fn-include-list.h-165"></a>

### `list_insert_before`

```c
static inline void list_insert_before(list_t *list, list_node_t *pos, list_node_t *node)
```

list_insert_before - 在指定节点前插入
@list: 链表指针
@pos: 插入位置；NULL 等价于 list_insert_last()
@node: 要插入的节点(必须已初始化, 且不在链表中)

**位置**：`include/list.h:165`

<a id="fn-include-list.h-188"></a>

### `list_delete_first`

```c
static inline list_node_t *list_delete_first(list_t *list)
```

list_delete_first - 删除链表首节点
@list: 链表指针

返回值: 被删除的节点指针, 空链表返回 NULL

**位置**：`include/list.h:188`

<a id="fn-include-list.h-216"></a>

### `list_delete`

```c
static inline list_node_t *list_delete(list_t *list, list_node_t *node)
```

list_delete - 从链表中删除指定节点
@list: 链表指针
@node: 要删除的节点

返回值: 被删除的节点指针

**位置**：`include/list.h:216`

<a id="fn-include-list.h-243"></a>

### `list_contains`

```c
static inline int32_t list_contains(list_t *list, list_node_t *node)
```

list_contains - 检查节点是否在链表中
@list: 链表指针
@node: 要检查的节点

返回值: 1 表示在链表中, 0 表示不在

**位置**：`include/list.h:243`

<a id="mod-include-mm-pmm.h"></a>

## include/mm/pmm.h — ── 物理内存配置 ─────────────────────────────────────────────────────

`include/mm/pmm.h` · 3 个函数 · 3 个有说明 · 184 行

<a id="fn-include-mm-pmm.h-83"></a>

### `pmm_is_initialized`

```c
static inline bool pmm_is_initialized(void)
```

pmm_is_initialized - 全局 PMM 是否已完成 pmm_init()

静态对象初值为 0；pmm_init() 断言 size > 0 并设置 total_pages,
所以 total_pages 非 0 是可靠的"已初始化"判据。

**位置**：`include/mm/pmm.h:83`

<a id="fn-include-mm-pmm.h-146"></a>

### `pmm_get_free_pages`

```c
static inline uint64_t pmm_get_free_pages(pmm_t *pmm)
```

pmm_get_free_pages - 获取空闲页面数
@pmm: 物理内存管理器

返回：空闲页面数

**位置**：`include/mm/pmm.h:146`

<a id="fn-include-mm-pmm.h-157"></a>

### `pmm_get_total_pages`

```c
static inline uint64_t pmm_get_total_pages(pmm_t *pmm)
```

pmm_get_total_pages - 获取总页面数
@pmm: 物理内存管理器

返回：总页面数

**位置**：`include/mm/pmm.h:157`

<a id="mod-include-mmio.h"></a>

## include/mmio.h — MMIO (Memory-Mapped I/O) 操作

`include/mmio.h` · 20 个函数 · 20 个有说明 · 215 行

提供带内存屏障和不带屏障的 MMIO 访问函数

<a id="fn-include-mmio.h-28"></a>

### `read8_relaxed`

```c
static inline uint8_t read8_relaxed(const volatile void *addr)
```

read8_relaxed - 读取 8 位 MMIO 寄存器(无屏障)
@addr: MMIO 地址

返回值: 读取的值

注意：不保证内存排序, 仅在明确不需要屏障时使用

**位置**：`include/mmio.h:28`

<a id="fn-include-mmio.h-40"></a>

### `write8_relaxed`

```c
static inline void write8_relaxed(uint8_t value, volatile void *addr)
```

write8_relaxed - 写入 8 位 MMIO 寄存器(无屏障)
@value: 要写入的值
@addr: MMIO 地址

注意：不保证内存排序和写入可见性

**位置**：`include/mmio.h:40`

<a id="fn-include-mmio.h-45"></a>

### `read16_relaxed`

```c
static inline uint16_t read16_relaxed(const volatile void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 volatile 指针直接读取 16 位寄存器值，不附加任何屏障。

**位置**：`include/mmio.h:45`

<a id="fn-include-mmio.h-50"></a>

### `write16_relaxed`

```c
static inline void write16_relaxed(uint16_t value, volatile void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 volatile 指针直接写入 16 位值，不附加任何屏障。

**位置**：`include/mmio.h:50`

<a id="fn-include-mmio.h-55"></a>

### `read32_relaxed`

```c
static inline uint32_t read32_relaxed(const volatile void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 volatile 指针直接读取 32 位寄存器值，不附加任何屏障。

**位置**：`include/mmio.h:55`

<a id="fn-include-mmio.h-60"></a>

### `write32_relaxed`

```c
static inline void write32_relaxed(uint32_t value, volatile void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 volatile 指针直接写入 32 位值，不附加任何屏障。

**位置**：`include/mmio.h:60`

<a id="fn-include-mmio.h-65"></a>

### `read64_relaxed`

```c
static inline uint64_t read64_relaxed(const volatile void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 volatile 指针直接读取 64 位寄存器值，不附加任何屏障。

**位置**：`include/mmio.h:65`

<a id="fn-include-mmio.h-70"></a>

### `write64_relaxed`

```c
static inline void write64_relaxed(uint64_t value, volatile void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 volatile 指针直接写入 64 位值，不附加任何屏障。

**位置**：`include/mmio.h:70`

<a id="fn-include-mmio.h-85"></a>

### `read8`

```c
static inline uint8_t read8(const volatile void *addr)
```

read8 - 读取 8 位 MMIO 寄存器(带屏障)
@addr: MMIO 地址

返回值: 读取的值

确保读取操作完成, 且后续操作不会被重排到读取之前

**位置**：`include/mmio.h:85`

<a id="fn-include-mmio.h-101"></a>

### `write8`

```c
static inline void write8(uint8_t value, volatile void *addr)
```

write8 - 写入 8 位 MMIO 寄存器(带屏障)
@value: 要写入的值
@addr: MMIO 地址

确保之前的操作完成, 且写入立即对设备可见

**位置**：`include/mmio.h:101`

<a id="fn-include-mmio.h-108"></a>

### `read16`

```c
static inline uint16_t read16(const volatile void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取前后各加 barrier_data，再以 volatile 读取 16 位 MMIO 寄存器。

**位置**：`include/mmio.h:108`

<a id="fn-include-mmio.h-117"></a>

### `write16`

```c
static inline void write16(uint16_t value, volatile void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

写入前后各加 barrier_data，再以 volatile 写入 16 位 MMIO 寄存器。

**位置**：`include/mmio.h:117`

<a id="fn-include-mmio.h-124"></a>

### `read32`

```c
static inline uint32_t read32(const volatile void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取前后各加 barrier_data，再以 volatile 读取 32 位 MMIO 寄存器并返回。

**位置**：`include/mmio.h:124`

<a id="fn-include-mmio.h-133"></a>

### `write32`

```c
static inline void write32(uint32_t value, volatile void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

写入前后各加 barrier_data，再以 volatile 写入 32 位 MMIO 寄存器。

**位置**：`include/mmio.h:133`

<a id="fn-include-mmio.h-140"></a>

### `read64`

```c
static inline uint64_t read64(const volatile void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取前后各加 barrier_data，再以 volatile 读取 64 位 MMIO 寄存器并返回。

**位置**：`include/mmio.h:140`

<a id="fn-include-mmio.h-149"></a>

### `write64`

```c
static inline void write64(uint64_t value, volatile void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

写入前后各加 barrier_data，再以 volatile 写入 64 位 MMIO 寄存器。

**位置**：`include/mmio.h:149`

<a id="fn-include-mmio.h-166"></a>

### `write8_post`

```c
static inline void write8_post(uint8_t value, volatile void *addr)
```

write8_post - 写入 8 位 MMIO 寄存器(仅后屏障)
@value: 要写入的值
@addr: MMIO 地址

只在写入后添加屏障, 确保写入对设备可见
适用于：写入后立即需要设备响应的场景

**位置**：`include/mmio.h:166`

<a id="fn-include-mmio.h-172"></a>

### `write16_post`

```c
static inline void write16_post(uint16_t value, volatile void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 volatile 写入 16 位值，随后加 barrier_data_write 保证写序。

**位置**：`include/mmio.h:172`

<a id="fn-include-mmio.h-178"></a>

### `write32_post`

```c
static inline void write32_post(uint32_t value, volatile void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 volatile 写入 32 位值，随后加 barrier_data_write 保证写序。

**位置**：`include/mmio.h:178`

<a id="fn-include-mmio.h-184"></a>

### `write64_post`

```c
static inline void write64_post(uint64_t value, volatile void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 volatile 写入 64 位值，随后加 barrier_data_write 保证写序。

**位置**：`include/mmio.h:184`

<a id="mod-include-npu-npu_mmu.h"></a>

## include/npu/npu_mmu.h — include/npu/npu_mmu.h

`include/npu/npu_mmu.h` · 2 个函数 · 2 个有说明 · 171 行

RK3588 NPU 的 IOMMU(`rockchip,iommu-v2`)—— 宿主独占的那一半。

── 为什么要有这个 ────────────────────────────────────────────────────

NPU 是没有 IOMMU 的物理设备, 只认物理地址。guest 的 RAM 是按需分页,
物理不连续, 所以透传给 guest 时只有恒等窗口里 IPA 才等于 PA —— 落在
窗口外的缓冲, NPU 读到的就是别人的页(真机实测 regcmd[126] = 0x78015044)。

RK3588 这块 MMU 是 Rockchip 自研的 单级 IOMMU(不是 ARM SMMU)：
`rknpu_iommu.c` 全文没有 stage 概念 —— 一张表, 谁写谁说了算。所以
不能让 guest 碰它(否则它能把任意物理地址映射进来, 隔离为零),
只能宿主写：

guest 驱动(原样, 仍以为没有 IOMMU)：写"物理地址" = IPA
│
宿主把 IPA 填进这张表, 指向真实的宿主页
│
NPU ──发出 IPA──▶ NPU MMU ──IPA→PA──▶ 宿主物理页(散着也行)

于是 guest 一个字不用改, 恒等窗口不再必要, 隔离也回来了。

── 表的真值来源 ──────────────────────────────────────────────────────

镜像 stage-2, 不另建一套。 宿主已经有一份权威的 IPA→PA, 就是 stage-2；
这张表跟着它走, 就不会出现"CPU 看得到, NPU 看不到"的错位。
同步时机放在提交时(guest 写 PC_OP_EN)而不是每次 stage-2 建映射时 ——
后者要给每页发一次 TLB 失效, 一次作业几千页, 代价不可接受。

── 寄存器与页表格式 ──────────────────────────────────────────────────

全部抄自 mainline `drivers/iommu/rockchip-iommu.c`(它原生支持 v2)。
注意： v2 的地址位域不是连续的, 见下面 npu_mmu_mk_dte() 的说明。

<a id="fn-include-npu-npu_mmu.h-105"></a>

### `npu_mmu_mk_dte`

```c
static inline uint32_t npu_mmu_mk_dte(uint64_t pa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按位段拼接物理地址生成 DTE：取 pa 的 31:12、35:32、39:36，并置 VALID 位。

<details>
<summary>函数内备注（3 条）</summary>

**第 107 行（行尾）**

> bit 31:12 ← pa[31:12]

**第 108 行（行尾）**

> bit 11:8  ← pa[35:32]

**第 109 行（行尾）**

> bit  7:4  ← pa[39:36]

</details>

**位置**：`include/npu/npu_mmu.h:105`

<a id="fn-include-npu-npu_mmu.h-113"></a>

### `npu_mmu_mk_pte`

```c
static inline uint32_t npu_mmu_mk_pte(uint64_t pa, int writable)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在 DTE 基础上置 READABLE 位生成 PTE，writable 非零时再置 WRITABLE 位。

**位置**：`include/npu/npu_mmu.h:113`

<a id="mod-include-riscv64-barrier_impl.h"></a>

## include/riscv64/barrier_impl.h — RISC-V 64位内存屏障实现

`include/riscv64/barrier_impl.h` · 7 个函数 · 7 个有说明 · 87 行

使用 FENCE 指令

<a id="fn-include-riscv64-barrier_impl.h-22"></a>

### `barrier_data`

```c
static inline void barrier_data(void)
```

barrier_data - 完整数据内存屏障

FENCE RW,RW: 读+写屏障

**位置**：`include/riscv64/barrier_impl.h:22`

<a id="fn-include-riscv64-barrier_impl.h-32"></a>

### `barrier_data_read`

```c
static inline void barrier_data_read(void)
```

barrier_data_read - 读数据屏障

FENCE R,R: 读屏障

**位置**：`include/riscv64/barrier_impl.h:32`

<a id="fn-include-riscv64-barrier_impl.h-42"></a>

### `barrier_data_write`

```c
static inline void barrier_data_write(void)
```

barrier_data_write - 写数据屏障

FENCE W,W: 写屏障

**位置**：`include/riscv64/barrier_impl.h:42`

<a id="fn-include-riscv64-barrier_impl.h-55"></a>

### `barrier_instr_full`

```c
static inline void barrier_instr_full(void)
```

barrier_instr_full - 完整指令同步屏障

FENCE IORW,IORW: IO + 读 + 写 屏障
用于同步指令和数据流

**位置**：`include/riscv64/barrier_impl.h:55`

<a id="fn-include-riscv64-barrier_impl.h-60"></a>

### `barrier_sync`

```c
static inline void barrier_sync(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

执行 fence iorw, iorw 指令完成 RISC-V 全序内存屏障。

**位置**：`include/riscv64/barrier_impl.h:60`

<a id="fn-include-riscv64-barrier_impl.h-72"></a>

### `barrier_acquire`

```c
static inline void barrier_acquire(void)
```

barrier_acquire - 获取语义屏障

FENCE R,R: 读屏障确保加载操作完成

**位置**：`include/riscv64/barrier_impl.h:72`

<a id="fn-include-riscv64-barrier_impl.h-82"></a>

### `barrier_release`

```c
static inline void barrier_release(void)
```

barrier_release - 释放语义屏障

FENCE WW,WW: 写屏障确保存储操作对其他核心可见

**位置**：`include/riscv64/barrier_impl.h:82`

<a id="mod-include-riscv64-cache_impl.h"></a>

## include/riscv64/cache_impl.h — RISC-V 64 的 cache 操作实现

`include/riscv64/cache_impl.h` · 7 个函数 · 7 个有说明 · 90 行

本文件只做动词 → 原语的映射, 具体指令由 rvplatform.h 按平台给出
(Zicbom / T-Head 私有 / 无 CMO 三种情况)。这样平台差异不会渗进通用动词：
以前这里用 `#if PLATFORM_SG2002` 把三个 range 动词全部换成了"整 cache 刷",
于是 clean_dcache_range() 在 SG2002 上既丢了区间语义, 也没法按需清理。

<a id="fn-include-riscv64-cache_impl.h-33"></a>

### `__clean_dcache_one`

```c
static inline void __clean_dcache_one(const void *addr)
```

__clean_dcache_one - 清理单个缓存行(写回内存)

**位置**：`include/riscv64/cache_impl.h:33`

<a id="fn-include-riscv64-cache_impl.h-41"></a>

### `__invalidate_dcache_one`

```c
static inline void __invalidate_dcache_one(const void *addr)
```

__invalidate_dcache_one - 使单个缓存行失效(不写回, 脏数据会丢)

**位置**：`include/riscv64/cache_impl.h:41`

<a id="fn-include-riscv64-cache_impl.h-52"></a>

### `__clean_and_invalidate_dcache_one`

```c
static inline void __clean_and_invalidate_dcache_one(const void *addr)
```

__clean_and_invalidate_dcache_one - 写回并使单个缓存行失效

T-Head 上用一条 `dcache.civa`：原来的写法是 cva 之后再 iva,
两条指令之间别的 agent 可能把该行弄脏, 语义上不等价。

**位置**：`include/riscv64/cache_impl.h:52`

<a id="fn-include-riscv64-cache_impl.h-59"></a>

### `get_cache_line_size`

```c
static inline size_t get_cache_line_size(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回全局缓存行大小 g_cache_line_size。

**位置**：`include/riscv64/cache_impl.h:59`

<a id="fn-include-riscv64-cache_impl.h-64"></a>

### `init_cache`

```c
static inline void init_cache(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把全局缓存行大小设为 rv_cmo_block_size() 返回的平台 CMO 块大小。

**位置**：`include/riscv64/cache_impl.h:64`

<a id="fn-include-riscv64-cache_impl.h-74"></a>

### `sync_caches`

```c
static inline void sync_caches(void)
```

sync_caches - 等待之前的 cache 操作完成

设备/CMO 之后的完成屏障；没有 CMO 的平台上这就是唯一的动作。

**位置**：`include/riscv64/cache_impl.h:74`

<a id="fn-include-riscv64-cache_impl.h-85"></a>

### `sync_icache_all`

```c
static inline void sync_icache_all(void)
```

sync_icache_all - 让刚写入的指令对所有核可见

RISC-V 上 `fence.i` 是标准做法；但 C906 这类核上 fence.i 不足以保证
I-cache 可见(厂商私有 icache.iall + sync.i 才是), 由 rvplatform 层处理。

**位置**：`include/riscv64/cache_impl.h:85`

<a id="mod-include-riscv64-cpu_impl.h"></a>

## include/riscv64/cpu_impl.h — RISC-V 64 per-CPU 指针 / 硬件 ID 实现。

`include/riscv64/cpu_impl.h` · 3 个函数 · 3 个有说明 · 40 行

内核态用 tp 保存 cpu_t*。进入用户态前 switch.S 会恢复/清空用户 tp；
U->S trap 的 C 入口会根据 sscratch 中的内核栈恢复内核 tp。

<a id="fn-include-riscv64-cpu_impl.h-22"></a>

### `arch_cpu_self_set`

```c
static inline void arch_cpu_self_set(struct cpu *self)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用内联汇编把 self 指针写入 tp 寄存器，并附 memory 屏障。

**位置**：`include/riscv64/cpu_impl.h:22`

<a id="fn-include-riscv64-cpu_impl.h-27"></a>

### `arch_cpu_self_get`

```c
static inline struct cpu *arch_cpu_self_get(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 tp 寄存器读回 struct cpu 指针并返回。

**位置**：`include/riscv64/cpu_impl.h:27`

<a id="fn-include-riscv64-cpu_impl.h-35"></a>

### `arch_cpu_hw_id`

```c
static inline uint64_t arch_cpu_hw_id(void)
```

mhartid 在 S-mode 不可读；hartid 由 OpenSBI/HSM 传入并存入 cpu_t::hw_id。

**位置**：`include/riscv64/cpu_impl.h:35`

<a id="mod-include-riscv64-exception_impl.h"></a>

## include/riscv64/exception_impl.h

`include/riscv64/exception_impl.h` · 6 个函数 · 6 个有说明 · 84 行

<a id="fn-include-riscv64-exception_impl.h-26"></a>

### `arch_irq_flags`

```c
static inline uint64_t arch_irq_flags(void)
```

读当前 sstatus(不改动状态)

**位置**：`include/riscv64/exception_impl.h:26`

<a id="fn-include-riscv64-exception_impl.h-36"></a>

### `arch_irq_is_enabled`

```c
static inline int arch_irq_is_enabled(void)
```

读是否允许 IRQ(抢占判断用)

**位置**：`include/riscv64/exception_impl.h:36`

<a id="fn-include-riscv64-exception_impl.h-42"></a>

### `arch_irq_save`

```c
static inline uint64_t arch_irq_save(void)
```

关中断并返回旧 sstatus(必须用 arch_irq_restore 配对恢复)

<details>
<summary>函数内备注（1 条）</summary>

**第 46 行**

> csrrci: 读出旧值后清除 bit1 (SIE)

</details>

**位置**：`include/riscv64/exception_impl.h:42`

<a id="fn-include-riscv64-exception_impl.h-57"></a>

### `arch_irq_restore`

```c
static inline void arch_irq_restore(uint64_t flags)
```

恢复到 arch_irq_save()/arch_irq_flags() 取到的状态

**位置**：`include/riscv64/exception_impl.h:57`

<a id="fn-include-riscv64-exception_impl.h-67"></a>

### `arch_irq_disable`

```c
static inline void arch_irq_disable(void)
```

无条件关中断(不需要旧状态时用)

**位置**：`include/riscv64/exception_impl.h:67`

<a id="fn-include-riscv64-exception_impl.h-76"></a>

### `arch_irq_enable`

```c
static inline void arch_irq_enable(void)
```

无条件开中断(新任务首次运行时使用)

**位置**：`include/riscv64/exception_impl.h:76`

<a id="mod-include-riscv64-halt_arch.h"></a>

## include/riscv64/halt_arch.h — RISC-V 64 位架构特定的 halt 实现

`include/riscv64/halt_arch.h` · 1 个函数 · 1 个有说明 · 47 行

<a id="fn-include-riscv64-halt_arch.h-22"></a>

### `arch_halt`

```c
static inline void arch_halt(void)
```

arch_halt - 停止 RISC-V 处理器
使用 WFI (Wait For Interrupt) 指令进入低功耗状态
在中断禁用的情况下, WFI 会永久停止

<details>
<summary>函数内备注（5 条）</summary>

**第 24 行**

> 内存屏障, 确保所有操作完成

**第 27 行**

> 禁用中断。

> 这里原本写的是 `csrci mstatus, 0x8`(MIE)—— 两处都错：
> 1) MIE 是 M 模式的中断使能位, 本内核跑在 S 模式, 该用 sstatus.SIE；
> 2) S 模式访问 mstatus 会触发非法指令异常, &quot;关中断&quot;实际没生效。
> 统一走 arch_irq_disable()。

**第 37 行**

> WFI - Wait For Interrupt

**第 38 行**

> 在中断禁用的情况下, 处理器会永久停止

**第 41 行**

> 如果 WFI 被唤醒(不应该发生), 死循环

</details>

**位置**：`include/riscv64/halt_arch.h:22`

<a id="mod-include-riscv64-hext.h"></a>

## include/riscv64/hext.h — RISC-V Hypervisor Extension (H-ext) CSR 定义

`include/riscv64/hext.h` · 9 个函数 · 9 个有说明 · 322 行

RISC-V Privileged Spec 1.12 §19-20 (H-extension)
当内核运行在 HS-mode 时, 使用这些 CSR 管理 VS-mode guest。

特权级对应关系：
HS-mode  = Hypervisor Supervisor(本内核运行在此)
VS-mode  = Virtual Supervisor(guest OS 运行在此)
VU-mode  = Virtual User(guest 用户进程)
U-mode   = 普通用户进程(非虚拟化, 直接 ecall 到 HS-mode)

<a id="fn-include-riscv64-hext.h-240"></a>

### `hext_read_hvip`

```c
static inline uint64_t hext_read_hvip(void)
```

注：本工具链(gcc/as)的 RISC-V 汇编器不识别 hvip/hie 的符号名
(与 hgatp 同理), 必须用数值 CSR 地址 + 内联汇编；数值定义在 csr.h。

**位置**：`include/riscv64/hext.h:240`

<a id="fn-include-riscv64-hext.h-247"></a>

### `hext_set_hvip_bits`

```c
static inline void hext_set_hvip_bits(uint64_t bits)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 csrs 指令向 0x645（HVIP）寄存器置位给定比特。

**位置**：`include/riscv64/hext.h:247`

<a id="fn-include-riscv64-hext.h-252"></a>

### `hext_clear_hvip_bits`

```c
static inline void hext_clear_hvip_bits(uint64_t bits)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 csrc 指令清除 0x645（HVIP）寄存器中给定比特。

**位置**：`include/riscv64/hext.h:252`

<a id="fn-include-riscv64-hext.h-257"></a>

### `hext_set_hie_bits`

```c
static inline void hext_set_hie_bits(uint64_t bits)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 csrs 指令置位 0x604（HIE）寄存器中给定比特。

**位置**：`include/riscv64/hext.h:257`

<a id="fn-include-riscv64-hext.h-271"></a>

### `hext_set_vs_software_irq`

```c
static inline void hext_set_vs_software_irq(int pending)
```

设置/清除 VS 软件中断挂起(SBI IPI 的落点)。

与 vsip.SSIP 的关系(见 docs/vmm/RISCV64_GUEST_LINUX.md「多 vCPU」一节)：
- HS-mode 置 hvip.VSSIP ⇒ VS-mode 的 sip.SSIP 读到 1；
- guest 在 VS-mode 清 sip.SSIP 会**同时清掉 hvip.VSSIP**(两者是同一个
挂起位的两个视图)，所以 VMM 必须在每次 exit 时回读 hvip 来同步自己的
软件 pending —— 只清不清软件状态会导致 IPI 永远重投(IPI 风暴)。

**位置**：`include/riscv64/hext.h:271`

<a id="fn-include-riscv64-hext.h-280"></a>

### `hext_set_vs_timer_irq`

```c
static inline void hext_set_vs_timer_irq(int pending)
```

设置/清除 VS 定时器中断挂起

**位置**：`include/riscv64/hext.h:280`

<a id="fn-include-riscv64-hext.h-289"></a>

### `hext_set_vs_external_irq`

```c
static inline void hext_set_vs_external_irq(int pending)
```

设置/清除 VS 外部中断挂起

**位置**：`include/riscv64/hext.h:289`

<a id="fn-include-riscv64-hext.h-312"></a>

### `hext_fence_i`

```c
static inline void hext_fence_i(void)
```

远端 fence 的「执行端」(SBI RFENCE 的落点)

SBI 的 remote fence.i / remote sfence.vma 要求**目标 hart**执行对应的
失效指令。本 VMM 的 vCPU 是宿主 hart 上分时运行的内核任务，没法在
收到请求的瞬间替对方执行，所以请求先记在目标 vCPU 上(remote_fence),
由它下一次进 guest 前在这里补执行 —— 那一刻 hgatp 已经切到本 VM
(见 hext_run.c 的 hext_per_hart_csrs_ensure), hfence.vvma 才有正确
的 VMID 上下文。

hfence.vvma 的助记符在 -march=rv64gc 下不被汇编器识别, 与 hfence.gvma
一样用 .insn 数值编码：funct7=0x11, funct3=0, rd/rs1/rs2=x0(全刷)。
这里做了「忽略地址/ASID, 一律全刷」的保守简化 —— 语义上永远安全,
代价只是 guest 换页时的 TLB 命中率。

**位置**：`include/riscv64/hext.h:312`

<a id="fn-include-riscv64-hext.h-317"></a>

### `hext_hfence_vvma_all`

```c
static inline void hext_hfence_vvma_all(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

发出 .insn 编码的 hfence.vvma 全刷指令并附 memory 屏障。

**位置**：`include/riscv64/hext.h:317`

<a id="mod-include-riscv64-mm_vm.h"></a>

## include/riscv64/mm_vm.h — RISC-V Sv39 页表操作接口

`include/riscv64/mm_vm.h` · 3 个函数 · 3 个有说明 · 73 行

本头文件由 mm_vm.h 包含, phys_to_virt/virt_to_phys 已在包含前定义。
实现位于 kernel/mm/riscv64/vmm.c。

<a id="fn-include-riscv64-mm_vm.h-21"></a>

### `rv_pa_to_kva`

```c
static inline void *rv_pa_to_kva(uint64_t pa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

直接转调 phys_to_virt，把物理地址换成内核虚拟地址。

**位置**：`include/riscv64/mm_vm.h:21`

<a id="fn-include-riscv64-mm_vm.h-26"></a>

### `rv_next_level`

```c
static inline uint64_t *rv_next_level(uint64_t pte)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先用 rv_pte_to_pa 取页表项中的物理页基址，再转成内核虚拟地址作为下一级表指针。

**位置**：`include/riscv64/mm_vm.h:26`

<a id="fn-include-riscv64-mm_vm.h-45"></a>

### `mm_vm_set_pte_keep`

```c
static inline void mm_vm_set_pte_keep(void *pgd, uint64_t vaddr)
```

把一页标成"设备缓冲"：unmap 时只拆映射, 不释放页也不碰引用计数。
用于把 dma_buf / ION 的缓冲区映射进用户态 —— 页归句柄所有, 不归映射。
语义与 RV_PTE_NOFREE 的区别见 mmu.h 里 RV_PTE_KEEP 的说明。

**位置**：`include/riscv64/mm_vm.h:45`

<a id="mod-include-riscv64-mmu.h"></a>

## include/riscv64/mmu.h — ── PTE 工具函数(定义在 pte.h；这里保留兼容包装)────────────────

`include/riscv64/mmu.h` · 3 个函数 · 3 个有说明 · 35 行

<a id="fn-include-riscv64-mmu.h-15"></a>

### `rv_pte_to_pa`

```c
static inline uint64_t rv_pte_to_pa(uint64_t pte)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

转调 RV_PTE_TO_PA 宏从页表项中提取物理地址。

**位置**：`include/riscv64/mmu.h:15`

<a id="fn-include-riscv64-mmu.h-20"></a>

### `rv_make_table_pte`

```c
static inline uint64_t rv_make_table_pte(uint64_t pa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 RV_PTE_FROM_PA 把物理地址转成带 V 位的表型页表项。

**位置**：`include/riscv64/mmu.h:20`

<a id="fn-include-riscv64-mmu.h-25"></a>

### `rv_pte_is_leaf`

```c
static inline bool rv_pte_is_leaf(uint64_t pte)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

转调 RV_PTE_IS_LEAF 宏判断页表项是否为叶子项。

**位置**：`include/riscv64/mmu.h:25`

<a id="mod-include-riscv64-rvplatform.h"></a>

## include/riscv64/rvplatform.h — RISC-V 平台上实际可用的 cache 管理指令(CMO)

`include/riscv64/rvplatform.h` · 21 个函数 · 21 个有说明 · 210 行

为什么只有 RISC-V 需要这一层：
aarch64 的 `dc`/`ic` 与 x86_64 的 `clflush`/`wbinvd` 都是架构标准指令,
缓存行大小也都能从 `CTR_EL0` / `CPUID` 读出来, 所以那两套的 cache_impl.h
自己就够了。RISC-V 不一样：基座 ISA 里没有 CMO；标准扩展 Zicbom 是可选的；
而 Zicbom 之前的厂商核(T-Head C906/C910…)用的是 CUSTOM-0 私有编码。
同一份 cache_impl.h 要面对"标准扩展 / 厂商私有 / 什么都没有"三种情况,
于是把"这台机器到底有什么 CMO"收敛到本文件, cache_impl.h 只做
通用动词 → 本文件原语 的映射。

新增平台时在这里加一个 RV_CMO_* 类别与对应分支, cache_impl.h 不用动。

<a id="fn-include-riscv64-rvplatform.h-75"></a>

### `rv_cmo_block_size`

```c
static inline size_t rv_cmo_block_size(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回平台常量 RV_CMO_BLOCK_SIZE 作为缓存块大小。

**位置**：`include/riscv64/rvplatform.h:75`

<a id="fn-include-riscv64-rvplatform.h-80"></a>

### `rv_cmo_clean`

```c
static inline void rv_cmo_clean(void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 .insn 编码的 cbo.clean 指令把该地址所在缓存块写回。

**位置**：`include/riscv64/rvplatform.h:80`

<a id="fn-include-riscv64-rvplatform.h-85"></a>

### `rv_cmo_invalidate`

```c
static inline void rv_cmo_invalidate(void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 .insn 编码的 cbo.inval 指令失效该地址所在缓存块。

**位置**：`include/riscv64/rvplatform.h:85`

<a id="fn-include-riscv64-rvplatform.h-90"></a>

### `rv_cmo_clean_invalidate`

```c
static inline void rv_cmo_clean_invalidate(void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用一条 civa 指令同时完成写回与失效，避免两步之间缓存行被弄脏。

<details>
<summary>函数内备注（1 条）</summary>

**第 92 行**

> civa：一条指令完成&quot;写回 + 失效&quot;。原来的代码发 cva 再发 iva,
> 两次访问之间别的 agent 可能把行弄脏, 语义上不等价。

</details>

**位置**：`include/riscv64/rvplatform.h:90`

<a id="fn-include-riscv64-rvplatform.h-109"></a>

### `rv_cmo_invalidate_all`

```c
static inline void rv_cmo_invalidate_all(void)
```

注意： 整 cache 失效：编码 0x0030000b。

原代码把这个编码命名为 `__c906_dcache_ciall`(清理+失效)并当作"清理"用在
clean_dcache_range() 上。但按上表的 0x0030000b 是 iall(失效, 不写回) ——
若表正确, 那条路径会丢弃脏数据。

目前没有任何调用者：SG2002 的 range 操作已改为按 VA 的 cva/iva/civa。
在查手册或上板确认之前：函数名按表(invalidate), 不要把它接到 clean 语义上；
若确认表有误, 改这里的编码/命名, 并考虑是否需要 *_all 形式的清理。

**位置**：`include/riscv64/rvplatform.h:109`

<a id="fn-include-riscv64-rvplatform.h-114"></a>

### `rv_cmo_sync`

```c
static inline void rv_cmo_sync(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

执行 fence rw,rw 作为缓存维护操作的完成屏障。

**位置**：`include/riscv64/rvplatform.h:114`

<a id="fn-include-riscv64-rvplatform.h-119"></a>

### `rv_cmo_icache_sync`

```c
static inline void rv_cmo_icache_sync(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

依次执行 fence.i、厂商 icache.iall 与 sync.i 指令，确保新写入指令可见。

<details>
<summary>函数内备注（3 条）</summary>

**第 121 行**

> fence.i 在 C906 上不足以让新写入的指令可见(这正是下面两条厂商指令
> 存在的原因, .S 的 exec/fork 返回路径里也是这么写的)。

**第 125 行（行尾）**

> icache.iall

**第 126 行（行尾）**

> sync.i

</details>

**位置**：`include/riscv64/rvplatform.h:119`

<a id="fn-include-riscv64-rvplatform.h-134"></a>

### `rv_cmo_block_size`

```c
static inline size_t rv_cmo_block_size(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回平台常量 RV_CMO_BLOCK_SIZE 作为缓存块大小。

**位置**：`include/riscv64/rvplatform.h:134`

<a id="fn-include-riscv64-rvplatform.h-139"></a>

### `rv_cmo_clean`

```c
static inline void rv_cmo_clean(void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 .insn 编码的 cbo.clean 指令把该地址所在缓存块写回。

**位置**：`include/riscv64/rvplatform.h:139`

<a id="fn-include-riscv64-rvplatform.h-144"></a>

### `rv_cmo_invalidate`

```c
static inline void rv_cmo_invalidate(void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 .insn 编码的 cbo.inval 指令失效该地址所在缓存块。

**位置**：`include/riscv64/rvplatform.h:144`

<a id="fn-include-riscv64-rvplatform.h-149"></a>

### `rv_cmo_clean_invalidate`

```c
static inline void rv_cmo_clean_invalidate(void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用一条 civa 指令同时完成写回与失效，避免两步之间缓存行被弄脏。

**位置**：`include/riscv64/rvplatform.h:149`

<a id="fn-include-riscv64-rvplatform.h-154"></a>

### `rv_cmo_invalidate_all`

```c
static inline void rv_cmo_invalidate_all(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空函数，注释说明 Zicbom 无整缓存失效指令故此处不实现。

<details>
<summary>函数内备注（1 条）</summary>

**第 156 行**

> Zicbom 只有按 block 的形式, 没有&quot;整 cache&quot;指令；真需要时按内存区间循环。
> 这里显式不做(平台层另有 *_all 需求时再实现)。

</details>

**位置**：`include/riscv64/rvplatform.h:154`

<a id="fn-include-riscv64-rvplatform.h-161"></a>

### `rv_cmo_sync`

```c
static inline void rv_cmo_sync(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

执行 fence rw,rw 作为缓存维护操作的完成屏障。

**位置**：`include/riscv64/rvplatform.h:161`

<a id="fn-include-riscv64-rvplatform.h-166"></a>

### `rv_cmo_icache_sync`

```c
static inline void rv_cmo_icache_sync(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

依次执行 fence.i、厂商 icache.iall 与 sync.i 指令，确保新写入指令可见。

**位置**：`include/riscv64/rvplatform.h:166`

<a id="fn-include-riscv64-rvplatform.h-174"></a>

### `rv_cmo_block_size`

```c
static inline size_t rv_cmo_block_size(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回平台常量 RV_CMO_BLOCK_SIZE 作为缓存块大小。

**位置**：`include/riscv64/rvplatform.h:174`

<a id="fn-include-riscv64-rvplatform.h-183"></a>

### `rv_cmo_clean`

```c
static inline void rv_cmo_clean(void *addr)
```

没有 CMO 的平台(QEMU virt)：DMA 由模拟器/硬件保证一致,
这里全部是空操作。以前 clean 分支会发一条 `fence ow, ow` —— 那是访存顺序
屏障, 不是 cache 操作, 对"清理"没有任何作用, 已去掉。

**位置**：`include/riscv64/rvplatform.h:183`

<a id="fn-include-riscv64-rvplatform.h-187"></a>

### `rv_cmo_invalidate`

```c
static inline void rv_cmo_invalidate(void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 .insn 编码的 cbo.inval 指令失效该地址所在缓存块。

**位置**：`include/riscv64/rvplatform.h:187`

<a id="fn-include-riscv64-rvplatform.h-191"></a>

### `rv_cmo_clean_invalidate`

```c
static inline void rv_cmo_clean_invalidate(void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用一条 civa 指令同时完成写回与失效，避免两步之间缓存行被弄脏。

**位置**：`include/riscv64/rvplatform.h:191`

<a id="fn-include-riscv64-rvplatform.h-195"></a>

### `rv_cmo_invalidate_all`

```c
static inline void rv_cmo_invalidate_all(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空函数，注释说明 Zicbom 无整缓存失效指令故此处不实现。

**位置**：`include/riscv64/rvplatform.h:195`

<a id="fn-include-riscv64-rvplatform.h-198"></a>

### `rv_cmo_sync`

```c
static inline void rv_cmo_sync(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

执行 fence rw,rw 作为缓存维护操作的完成屏障。

**位置**：`include/riscv64/rvplatform.h:198`

<a id="fn-include-riscv64-rvplatform.h-203"></a>

### `rv_cmo_icache_sync`

```c
static inline void rv_cmo_icache_sync(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

依次执行 fence.i、厂商 icache.iall 与 sync.i 指令，确保新写入指令可见。

**位置**：`include/riscv64/rvplatform.h:203`

<a id="mod-include-riscv64-satp_utils.h"></a>

## include/riscv64/satp_utils.h — RISC-V Sv39 satp 操作工具

`include/riscv64/satp_utils.h` · 3 个函数 · 3 个有说明 · 66 行

统一封装 satp 寄存器读写, PGD 物理地址转换以及用户页表内核映射复制。
所有需要操作 satp 或初始化用户页表的内核代码必须通过此头文件。

内核高半区映射的唯一来源是 riscv64/mem_layout.h 的
RV_KERNEL_MAP_LIST；这里不再逐条硬编码 0x100..0x103。

<a id="fn-include-riscv64-satp_utils.h-26"></a>

### `satp_read_pgd_phys`

```c
static inline uint64_t satp_read_pgd_phys(void)
```

satp_read_pgd_phys - 读取当前 satp 并返回根页表物理地址

**位置**：`include/riscv64/satp_utils.h:26`

<a id="fn-include-riscv64-satp_utils.h-37"></a>

### `pgd_phys_to_satp`

```c
static inline uint64_t pgd_phys_to_satp(uint64_t pgd_phys)
```

pgd_phys_to_satp - 根据根页表物理地址构造 Sv39 satp 值
@pgd_phys: 根页表的物理地址(必须 4KB 对齐)

**位置**：`include/riscv64/satp_utils.h:37`

<a id="fn-include-riscv64-satp_utils.h-59"></a>

### `riscv64_copy_kernel_mappings`

```c
static inline void riscv64_copy_kernel_mappings(uint64_t *user_l1, const uint64_t *kernel_l1)
```

riscv64_copy_kernel_mappings - 将内核高半区 L1 条目复制到用户页表
@user_l1:   用户根页表(内核虚拟地址, uint64_t[512])
@kernel_l1: 内核根页表(内核虚拟地址, uint64_t[512])

必须在创建任何用户页表时调用：U 态发生 trap 时, CPU 通过 stvec
(内核高半区虚拟地址)跳转, 若用户页表中无内核映射则立即失联。

复制哪些条目完全由 RV_KERNEL_MAP_LIST(mem_layout.h)决定,
mmu.S 的启动映射也用同一份列表, 避免两边漂移。

**位置**：`include/riscv64/satp_utils.h:59`

<a id="mod-include-riscv64-sbi.h"></a>

## include/riscv64/sbi.h — SBI(Supervisor Binary Interface)常量 + 主机侧调用封装

`include/riscv64/sbi.h` · 2 个函数 · 2 个有说明 · 130 行

常量同时服务两类使用者:
- 宿主 bring-up(kernel/task/cpu.c)：HSM/IPI 扩展走这里的内联封装;
- VMM 的 guest 模拟(kernel/vmm/riscv64/hext_run.c)：guest 以
a7=EID, a6=FID, a0..=参数 发 ecall, 宿主按同一组编号分派。

以前这组常量写在 hext.h(那是 H-ext CSR 的头)里, 宿主侧又自己写了一份
功能号 —— 和 csr.h 顶部记录的问题同源: 改一边漏一边不会报错。
现在是唯一定义, hext.h 不再重复。

调用约定: a7=扩展号(EID), a6=功能号(FID), a0..a5=参数; 返回值在 a0。

<a id="fn-include-riscv64-sbi.h-95"></a>

### `sbi_hart_start`

```c
static inline long sbi_hart_start(uint64_t hartid, uint64_t start_addr, uint64_t opaque)
```

sbi_hart_start - HSM 扩展: 启动指定 hart
@hartid:      目标 hart 的硬件号(mhartid 在 S-mode 不可读, 由调用者保存)
@start_addr:  入口的**物理**地址(SBI 在目标核的 MMU 配置前进入)
@opaque:      透传给入口的 a1(本内核用它传逻辑 CPU 号)

返回: SBI 返回码, 0 = 成功

**位置**：`include/riscv64/sbi.h:95`

<a id="fn-include-riscv64-sbi.h-116"></a>

### `sbi_send_ipi`

```c
static inline long sbi_send_ipi(uint64_t hartid)
```

sbi_send_ipi - IPI 扩展: 向单个 hart 发软件中断
@hartid: 目标 hart 的硬件号

只支持 hartid < 64：这里把 "1 << hartid" 直接放在 a0(hart_mask 的低
XLEN 位)。超过 64 个 hart 的平台需要改用 hart_mask_base + 稀疏掩码。

返回: SBI 返回码, 0 = 成功, -1 = hartid 超出本封装范围

**位置**：`include/riscv64/sbi.h:116`

<a id="mod-include-riscv64-spin_lock_impl.h"></a>

## include/riscv64/spin_lock_impl.h — RISC-V 64位架构的spinlock实现

`include/riscv64/spin_lock_impl.h` · 6 个函数 · 6 个有说明 · 106 行

使用LR/SC (Load-Reserved/Store-Conditional) 指令对
使用fence指令进行内存屏障

<a id="fn-include-riscv64-spin_lock_impl.h-29"></a>

### `spin_lock`

```c
static inline void spin_lock(spinlock_t *lock)
```

RISC-V的原子操作使用LR/SC指令对
lr.w: 加载保留 Load-Reserved
sc.w: 条件存储 Store-Conditional
如果在lr到sc之间有其他核心修改了地址, sc会失败

<details>
<summary>函数内备注（6 条）</summary>

**第 33 行（行尾）**

> tmp = 1

**第 34 行（行尾）**

> t0 = lock-&gt;lock (load-reserved)

**第 35 行（行尾）**

> if (t0 != 0) goto 1b (already locked)

**第 36 行（行尾）**

> tmp = atomic_exchange(lock-&gt;lock, 1)

**第 37 行（行尾）**

> if (t0 != 0) goto 1b (store failed, retry)

**第 38 行（行尾）**

> full memory barrier

</details>

**位置**：`include/riscv64/spin_lock_impl.h:29`

<a id="fn-include-riscv64-spin_lock_impl.h-44"></a>

### `spin_trylock`

```c
static inline int spin_trylock(spinlock_t *lock)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先关抢占，用 lr.w/sc.w 原子尝试取锁，成功返回 0，失败恢复抢占并返回 1。

<details>
<summary>函数内备注（8 条）</summary>

**第 48 行（行尾）**

> tmp = 1

**第 49 行（行尾）**

> t0 = lock-&gt;lock

**第 50 行（行尾）**

> if locked, fail

**第 51 行（行尾）**

> try to acquire

**第 52 行（行尾）**

> if failed, retry

**第 53 行（行尾）**

> barrier on success

**第 54 行（行尾）**

> result = 0 (success)

**第 56 行（行尾）**

> result = 1 (failed)

</details>

**位置**：`include/riscv64/spin_lock_impl.h:44`

<a id="fn-include-riscv64-spin_lock_impl.h-66"></a>

### `spin_unlock`

```c
static inline void spin_unlock(spinlock_t *lock)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

执行 fence rw,rw 后把锁字清零释放，并重新开启抢占。

<details>
<summary>函数内备注（2 条）</summary>

**第 68 行（行尾）**

> memory barrier

**第 69 行（行尾）**

> lock-&gt;lock = 0

</details>

**位置**：`include/riscv64/spin_lock_impl.h:66`

<a id="fn-include-riscv64-spin_lock_impl.h-85"></a>

### `spin_lock_irqsave`

```c
static inline void spin_lock_irqsave(spinlock_t *lock, uint64_t *flags)
```

带中断保护的 spinlock。

中断状态存在调用点的局部变量里(由 *flags 带回), 不再存进锁对象 ——
存进锁对象时, SMP 下争锁的另一颗 CPU 会把它的 flags 覆盖上去, 解锁时
恢复的就是别人的中断状态。中断原语本身统一来自
include/riscv64/exception_impl.h(arch_irq_save/restore)。

**位置**：`include/riscv64/spin_lock_impl.h:85`

<a id="fn-include-riscv64-spin_lock_impl.h-91"></a>

### `spin_trylock_irqsave`

```c
static inline int spin_trylock_irqsave(spinlock_t *lock, uint64_t *flags)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

保存并关闭中断后尝试取锁，失败时恢复中断并返回 1，成功返回 0。

**位置**：`include/riscv64/spin_lock_impl.h:91`

<a id="fn-include-riscv64-spin_lock_impl.h-100"></a>

### `spin_unlock_irqrestore`

```c
static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先释放锁，再恢复调用方保存的中断状态。

**位置**：`include/riscv64/spin_lock_impl.h:100`

<a id="mod-include-riscv64-string_impl.h"></a>

## include/riscv64/string_impl.h — RISC-V 64 位架构的字符串函数优化实现

`include/riscv64/string_impl.h` · 2 个函数 · 2 个有说明 · 31 行

目前转发到通用实现。要做 V 扩展优化时, 在这里加 memcpy_vector()/memset_vector()
并让下面两个函数按阈值分派即可(对照 include/aarch64/string_impl.h)。

注意：通用实现用普通 GPR 做 8/4/2 字节访问, 只有在 src/dest 同时对齐时
才加宽 —— SG2002/C906 这类真实硬件不保证支持非对齐访问, 不能想当然放宽。

<a id="fn-include-riscv64-string_impl.h-21"></a>

### `memcpy_arch`

```c
static inline void *memcpy_arch(void *dest, const void *src, size_t n)
```

RISC-V 64 位架构的字符串函数优化实现

目前转发到通用实现。要做 V 扩展优化时, 在这里加 memcpy_vector()/memset_vector()
并让下面两个函数按阈值分派即可(对照 include/aarch64/string_impl.h)。

注意：通用实现用普通 GPR 做 8/4/2 字节访问, 只有在 src/dest 同时对齐时
才加宽 —— SG2002/C906 这类真实硬件不保证支持非对齐访问, 不能想当然放宽。

**位置**：`include/riscv64/string_impl.h:21`

<a id="fn-include-riscv64-string_impl.h-26"></a>

### `memset_arch`

```c
static inline void *memset_arch(void *s, int c, size_t n)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

直接转调 memset_generic 完成内存填充。

**位置**：`include/riscv64/string_impl.h:26`

<a id="mod-include-riscv64-syscall_abi.h"></a>

## include/riscv64/syscall_abi.h

`include/riscv64/syscall_abi.h` · 6 个函数 · 6 个有说明 · 61 行

<a id="fn-include-riscv64-syscall_abi.h-13"></a>

### `syscall_abi_nr`

```c
static inline uint64_t syscall_abi_nr(const trap_frame_t *frame)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 trap frame 的 a7 寄存器 x[17] 读出系统调用号。

<details>
<summary>函数内备注（1 条）</summary>

**第 15 行（行尾）**

> a7 = syscall number

</details>

**位置**：`include/riscv64/syscall_abi.h:13`

<a id="fn-include-riscv64-syscall_abi.h-18"></a>

### `syscall_abi_arg`

```c
static inline uint64_t syscall_abi_arg(const trap_frame_t *frame, int idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

下标 0 到 5 时返回 a0 至 a5（x[10+idx]），越界返回 0。

<details>
<summary>函数内备注（1 条）</summary>

**第 20 行（行尾）**

> a0..a5

</details>

**位置**：`include/riscv64/syscall_abi.h:18`

<a id="fn-include-riscv64-syscall_abi.h-23"></a>

### `syscall_abi_set_ret`

```c
static inline void syscall_abi_set_ret(trap_frame_t *frame, uint64_t ret)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把返回值写入 trap frame 的 a0（x[10]）。

<details>
<summary>函数内备注（1 条）</summary>

**第 25 行（行尾）**

> a0 = return value

</details>

**位置**：`include/riscv64/syscall_abi.h:23`

<a id="fn-include-riscv64-syscall_abi.h-28"></a>

### `syscall_abi_ip`

```c
static inline uint64_t syscall_abi_ip(const trap_frame_t *frame)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 trap frame 的 sepc，即系统调用发生时的用户指令地址。

**位置**：`include/riscv64/syscall_abi.h:28`

<a id="fn-include-riscv64-syscall_abi.h-33"></a>

### `syscall_abi_user_sp`

```c
static inline uint64_t syscall_abi_user_sp(const trap_frame_t *frame)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 trap frame 中保存的用户栈指针 x[2]。

<details>
<summary>函数内备注（1 条）</summary>

**第 35 行（行尾）**

> sp

</details>

**位置**：`include/riscv64/syscall_abi.h:33`

<a id="fn-include-riscv64-syscall_abi.h-47"></a>

### `syscall_abi_reset_frame_for_exec`

```c
static inline void syscall_abi_reset_frame_for_exec(trap_frame_t *frame, uint64_t entry, uint64_t sp)
```

syscall_abi_reset_frame_for_exec - execve 成功后把 trap frame 重置成
新程序的初始用户现场

返回路径是一条普通 sret，所以这里必须自己把 sstatus 摆成"返回 U 模式、
SPIE=1、SIE=0"，与 arch_switch_to_user 的处理保持一致；FP 状态清零，
tp 也清零(exec 不继承 TLS)。x0 随后会被 syscall 返回值的回填覆盖
（值为 0，新程序不依赖它）。

**位置**：`include/riscv64/syscall_abi.h:47`

<a id="mod-include-rt-rt_worker.h"></a>

## include/rt/rt_worker.h — 用户可配置的 RT worker（模板）控制面 ABI

`include/rt/rt_worker.h` · 1 个函数 · 0 个有说明 · 265 行

设计：
- 数据面：内核 RT 任务，按 period_ns 周期调用一个模板的 step()。
step 有界、无分配、无睡眠、无 VFS，只读写预分配状态和参数。
- 控制面：用户态通过 /dev/rtctl 的 ioctl 创建/启动/停止/喂输入/取输出。
- 设备：模板的输出可选绑定到一个 rtdev（见 include/rt/rtdev.h）。
设备在 CREATE 时独占绑定，STOP/FAULT 时调用 safe_state()。

注意：apps/c/rtctl.c 里有一份逐字节相同的 ABI 副本。改这里必须同步改
用户态那份（结构体尺寸也被编进 ioctl 号，尺寸错了就是另一个命令）。

<a id="fn-include-rt-rt_worker.h-165"></a>

### `rtw_shm_events`

```c
static inline struct rtw_event_ring *rtw_shm_events(struct rtw_shm *s)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/rt/rt_worker.h:165`

<a id="mod-include-spinlock.h"></a>

## include/spinlock.h — 跨架构 spinlock 抽象层

`include/spinlock.h` · 2 个函数 · 2 个有说明 · 94 行

根据架构自动选择对应的实现

<a id="fn-include-spinlock.h-50"></a>

### `spinlock_init`

```c
static inline void spinlock_init(spinlock_t *lock)
```

通用初始化函数(内联)
所有架构的实现相同

**位置**：`include/spinlock.h:50`

<a id="fn-include-spinlock.h-55"></a>

### `spinlock_irq_init`

```c
static inline void spinlock_irq_init(spinlock_noirq_t *lock)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把锁字置 0 完成初始化，无其它动作。

**位置**：`include/spinlock.h:55`

<a id="mod-include-task-cpu.h"></a>

## include/task/cpu.h — Per-CPU 控制块(SMP 抽象层)

`include/task/cpu.h` · 2 个函数 · 0 个有说明 · 208 行

Phase 0：仅提供数据结构 + BSP 初始化骨架, 行为等价于单核。
- g_num_cpus 始终为 1, cpu_bring_up_all() 为 stub
- cpu_current() 返回 &g_cpus[0](BSP 安装后从 per-CPU 寄存器读取)

后续阶段：
Phase 1: 调度器迁移到 cpu_t::run_queue / current_task / need_resched
Phase 2: aarch64 PSCI 起多核 → cpu_secondary_bootstrap()
Phase 4: IPI / 跨核唤醒

<a id="fn-include-task-cpu.h-154"></a>

### `cpu_current`

```c
static inline cpu_t *cpu_current(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 157 行**

> 若 per-CPU 寄存器尚未安装(早期 boot 或测试代码), 回退 CPU0。

</details>

**位置**：`include/task/cpu.h:154`

<a id="fn-include-task-cpu.h-161"></a>

### `get_current_cpu_id`

```c
static inline uint32_t get_current_cpu_id(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/task/cpu.h:161`

<a id="mod-include-task-switch.h"></a>

## include/task/switch.h — 架构抽象：上下文切换

`include/task/switch.h` · 3 个函数 · 3 个有说明 · 376 行

提供：
arch_task_switch()      — 在 .S 文件中实现, 保存/恢复被调用者寄存器
arch_init_task_stack()  — 为新任务构造初始切换帧

中断屏蔽(arch_irq_save/restore/enable/disable/flags/is_enabled)已统一到
include/<arch>/exception_impl.h, 由本文件顶部 include 的 exception.h 暴露。

上下文切换原理：
仅保存"被调用者保存寄存器"(callee-saved registers)。
调用者保存寄存器由 C 调用约定在调用 sched_schedule() 前
自动保存到栈帧, arch_task_switch() 无需额外处理。

新任务初始化：
arch_init_task_stack() 在新任务的栈顶构造一个伪造的切换帧,
使首次 arch_task_switch() "返回"时跳转到 task_trampoline()。

<a id="fn-include-task-switch.h-118"></a>

### `arch_init_task_stack`

```c
static inline uintptr_t arch_init_task_stack(uint8_t *stack_base, uint32_t stack_size)
```

arch_init_task_stack - 在新任务的栈上构造伪切换帧
@stack_base: 栈底(低地址)
@stack_size: 栈大小(字节)

返回：应写入 task->sp 的初始值。

构造的帧与 arch_task_switch() 保存的帧格式完全一致,
使得首次调度到该任务时 arch_task_switch 的 "ret"
跳转到 task_trampoline()。

<details>
<summary>函数内备注（7 条）</summary>

**第 123 行**

> arch_task_switch 保存顺序(stp x19,x20 [sp,#-176]! ... stp x9,x10 [sp,#160])：
> saved_sp + 0  = x19,  saved_sp + 8  = x20
> saved_sp + 16 = x21,  saved_sp + 24 = x22
> saved_sp + 32 = x23,  saved_sp + 40 = x24
> saved_sp + 48 = x25,  saved_sp + 56 = x26
> saved_sp + 64 = x27,  saved_sp + 72 = x28
> saved_sp + 80 = x29,  saved_sp + 88 = x30 (LR)  ← ret 目标
> saved_sp + 96 .. 152 = d8-d15
> saved_sp + 160 = fpcr, saved_sp + 168 = fpsr
> 共 22 × 8 = 176 字节

> 全部清零：新内核线程以干净的 FP 状态开始(FPCR/FPSR = 0, d8-d15 = 0)。

**第 140 行（行尾）**

> x30 (LR)

**第 143 行**

> arch_task_switch 保存顺序(sd ra 0(sp) ... sd s11 96(sp), fsd fs0..fs11, fcsr)：
> saved_sp + 0  = ra    ← ret 目标
> saved_sp + 8  = s0,  saved_sp + 16 = s1
> ...
> saved_sp + 96 = s11
> saved_sp + 104 = fs0 .. saved_sp + 192 = fs11
> saved_sp + 200 = fcsr
> 共 26 × 8 = 208 字节

**第 154 行（行尾）**

> ra

**第 159 行**

> arch_task_switch 保存顺序(pushq rbx, rbp, r12, r13, r14, r15)：
> 恢复顺序(popq r15, r14, r13, r12, rbp, rbx, ret)：
> saved_sp + 0  = r15
> saved_sp + 8  = r14
> saved_sp + 16 = r13
> saved_sp + 24 = r12
> saved_sp + 32 = rbp
> saved_sp + 40 = rbx
> saved_sp + 48 = 返回地址  ← ret 目标
> 共 7 × 8 = 56 字节

**第 174 行（行尾）**

> return address

**第 176 行（行尾）**

> ARCH_*

</details>

**位置**：`include/task/switch.h:118`

<a id="fn-include-task-switch.h-195"></a>

### `arch_init_user_stack`

```c
static inline uintptr_t arch_init_user_stack(uint8_t *stack_base, uint32_t stack_size, uint64_t user_entry, uint64_t user_sp, uint64_t user_pgd)
```

arch_init_user_stack - 为用户进程初始化内核栈(首次跳转用)
@stack_base: 内核栈底(低地址)
@stack_size: 内核栈大小(字节)
@user_entry: 用户态入口点(虚拟地址)
@user_sp: 用户栈指针(虚拟地址)

返回：应写入 task->sp 的初始内核栈指针。

构造的帧使得首次调度时跳转到 task_trampoline_user,
然后调用 arch_switch_to_user 跳转到用户态。

<details>
<summary>函数内备注（20 条）</summary>

**第 202 行**

> 保存22个被调用者寄存器(176 字节, 含 FP 保存区)
> 布局：
> [0]   x19=user_entry  [8]   x20=user_sp
> [16]  x21=unused       [24]  x22
> [32]  x23              [40]  x24
> [48]  x25              [56]  x26
> [64]  x27              [72]  x28
> [80]  x29              [88]  x30 (LR) → task_trampoline_user
> [96..152] d8-d15, [160] fpcr, [168] fpsr

> FP 区清零：新进程首次进 EL0 时 FPCR/FPSR 与 d8-d15 都是干净的,
> 不会把内核残留的 FP 值泄漏给用户态。

**第 217 行（行尾）**

> x19 = 用户入口

**第 218 行（行尾）**

> x20 = 用户栈

**第 221 行（行尾）**

> x30 (LR)

**第 224 行**

> 保存26个被调用者寄存器(208字节)：
> [0]=ra [1]=s0 [2]=s1 ... [12]=s11 [13..24]=fs0..fs11 [25]=fcsr
> task_trampoline_user 约定：s0=user_entry, s1=user_sp, s2=user_pgd

**第 230 行（行尾）**

> ra

**第 231 行（行尾）**

> s0

**第 232 行（行尾）**

> s1

**第 233 行（行尾）**

> s2

**第 238 行**

> x86_64 栈布局(arch_task_switch 恢复时)：

> [高地址]
> user_sp         &lt;- 会被 task_trampoline_user popq 到 rsi
> user_entry      &lt;- 会被 task_trampoline_user popq 到 rdi
> 返回地址 (LR)   &lt;- 会被 ret 弹出, 跳转到 task_trampoline_user
> r15
> r14
> r13
> r12
> rbp
> rbx             &lt;- sp 指向这里
> [低地址]

> 保存6个被调用者寄存器 + 返回地址 + 2个参数 = 9 个 uint64_t

**第 256 行（行尾）**

> rbx

**第 257 行（行尾）**

> rbp

**第 258 行（行尾）**

> r12

**第 259 行（行尾）**

> r13

**第 260 行（行尾）**

> r14

**第 261 行（行尾）**

> r15

**第 262 行（行尾）**

> 返回地址 (LR)

**第 263 行（行尾）**

> 用户入口

**第 264 行（行尾）**

> 用户栈

**第 266 行（行尾）**

> x86_64 页表通过 task-&gt;mm-&gt;pgd 在调度时切换 CR3

</details>

**位置**：`include/task/switch.h:195`

<a id="fn-include-task-switch.h-291"></a>

### `arch_init_fork_child_stack`

```c
static inline uintptr_t arch_init_fork_child_stack(uint8_t *stack_base, uint32_t stack_size, trap_frame_t *frame, uint64_t child_stack, uint64_t tls)
```

arch_init_fork_child_stack - 为 fork/线程子进程初始化内核栈
@stack_base:      内核栈底(低地址)
@stack_size:      内核栈大小(字节)
@frame:           父进程的完整 trap_frame_t
@child_stack:     子进程/线程的用户栈指针(0 = 继承父进程)
@tls:             线程本地存储指针(0 = 继承父进程)

fork：child_stack=0, tls=0
线程(CLONE_VM+CLONE_SETTLS)：child_stack=新栈, tls=TLS指针

返回：应写入 task->sp 的初始值。

<details>
<summary>函数内备注（26 条）</summary>

**第 297 行**

> 先在内核栈顶放一份 trap_frame_t

**第 300 行**

> 整体拷贝：r[]/usp/elr/spsr/tpidr_el0 与 FP 状态(q0-q31/fpcr/fpsr)
> 全部继承父进程 —— 子进程的 FP 状态在 fork 时必须与父进程一致。
> (早先这里逐字段拷贝, 扩 FP 时容易漏字段。)

**第 308 行**

> 子进程 fork/线程 返回 0

**第 311 行**

> 再放 22 个被调用者寄存器(含 FP 保存区), x30(LR)→arch_fork_resume_user

**第 313 行（行尾）**

> x19 = &amp;child_frame

**第 316 行（行尾）**

> x30 (LR)

**第 318 行**

> RISC-V trap_frame_t: x[0..31], sepc, scause, stval, sstatus, f[0..31], fcsr

**第 332 行**

> fork/线程返回值 = 0 (a0/x10)

**第 334 行**

> 覆盖用户栈 / TLS (tp = x4)

**第 336 行（行尾）**

> sp

**第 338 行（行尾）**

> tp

**第 340 行**

> 再放 26 个被调用者寄存器(含 FP), ra-&gt;arch_fork_resume_user, s0-&gt;child_frame

**第 342 行（行尾）**

> ra

**第 343 行（行尾）**

> s0 = &amp;child_frame

**第 347 行**

> x86_64: 在内核栈上放 trap_frame, 再放 arch_task_switch 的被调用者寄存器帧

**第 352 行（行尾）**

> fork/线程子进程返回 0

**第 354 行（行尾）**

> 覆盖用户栈

**第 355 行**

> x86_64 TLS (fs_base) 在 clone 调用者处通过 task-&gt;fs_base 设置

**第 357 行**

> arch_task_switch 保存/恢复顺序(低地址→高地址)：
> [sp+0] r15  [sp+8] r14  [sp+16] r13  [sp+24] r12
> [sp+32] rbp [sp+40] rbx [sp+48] 返回地址
> rbx 恢复后值为 &amp;child_frame, ret 后跳 arch_fork_resume_user。

**第 364 行（行尾）**

> r15

**第 365 行（行尾）**

> r14

**第 366 行（行尾）**

> r13

**第 367 行（行尾）**

> r12

**第 368 行（行尾）**

> rbp

**第 369 行（行尾）**

> rbx = frame 指针

**第 370 行（行尾）**

> 返回地址

</details>

**位置**：`include/task/switch.h:291`

<a id="mod-include-task-task.h"></a>

## include/task/task.h — 内核任务控制块与公共 API

`include/task/task.h` · 5 个函数 · 4 个有说明 · 541 行

设计：
- 内核任务 + 用户进程共用 TCB；用户进程的地址空间/文件/信号见各字段注释
- 静态任务池, 最多 TASK_MAX 个并发任务
- 调度器 (sched.c) 实现轮转调度 (round-robin)
- 上下文切换由 arch_task_switch() 完成(仅保存被调用者保存寄存器)
- 字段按“切换/调度热区 → 用户状态 → 冷数据/大数组”排列，
并由文件末尾的 _Static_assert 锁住布局（见 docs/task_t_refactor.md）

<a id="fn-include-task-task.h-215"></a>

### `task_is_thread`

```c
static inline bool task_is_thread(const task_t *t)
```

CLONE_THREAD 创建的任务：tgid 指向线程组首线程；exec 后 tgid 恢复为自身 id。

**位置**：`include/task/task.h:215`

<a id="fn-include-task-task.h-232"></a>

### `task_is_live_user`

```c
static inline bool task_is_live_user(const task_t *t)
```

task_is_live_user - 扫描任务池时，能否安全读取该任务的用户态字段

不能只判断 state != TASK_DEAD：回收路径 task_reap_dead() 先在池锁内
把 state 改成 TASK_ALLOCATING，随后 slot_take_and_free() 在锁外
task_release_objects() 把 mm 置 NULL，而 g_stack_used[] 要到更后面
才清零。中间这窗口里"槽还在 + 不是 DEAD + mm 已为 NULL"同时成立，
于是 task->mm->pgd 会读到 NULL+8(mm_struct 里 pgd 的偏移正好是 8)。
创建失败回滚(free_task_slot)同样是先释放对象再清槽。

mm 非空是最后一道保险：任何扫描都别假设 is_user_process 为真就一定有 mm。

**位置**：`include/task/task.h:232`

<a id="fn-include-task-task.h-238"></a>

### `task_is_rt`

```c
static inline bool task_is_rt(const task_t *t)
```

是否 RT 调度类（FIFO/RR）。

**位置**：`include/task/task.h:238`

<a id="fn-include-task-task.h-247"></a>

### `task_sched_is_rt`

```c
static inline bool task_sched_is_rt(const task_t *t)
```

调度器用的“有效 RT 属性”：基础 RT 调度类，或被 rt_mutex 临时提升的
NORMAL 任务。pi_priority 只由 sched_set_pi_boost() 写。

**位置**：`include/task/task.h:247`

<a id="fn-include-task-task.h-252"></a>

### `task_effective_rt_prio`

```c
static inline uint8_t task_effective_rt_prio(const task_t *t)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`include/task/task.h:252`

<a id="mod-include-vmm-vmm.h"></a>

## include/vmm/vmm.h — VMM 类型和公共 API

`include/vmm/vmm.h` · 8 个函数 · 8 个有说明 · 1023 行

架构模型：vCPU = task, VM = process(虚拟进程)
当前实现仅支持 AArch64 + VHE(Virtualization Host Extensions)。

vcpu_t 前部固定偏移(与 el2_vmcs.S / vcpu_ctx.S 必须完全一致)：

Offset   0 : r[0..30]      = 31 × 8 = 248 B   (x0-x30 guest GPRs)
Offset 248 : sp_el1        = 8 B               (guest SP_EL1)
Offset 256 : elr           = 8 B               (guest PC = ELR_EL2)
Offset 264 : spsr          = 8 B               (guest PSTATE = SPSR_EL2)
Offset 272 : host_ctx[13]  = 13 × 8 = 104 B   (x19-x28, x29, x30, sp)
Offset 376 : host_vbar     = 8 B               host VBAR_EL2
Offset 384 : host_tpidr    = 8 B               host per-CPU pointer
Offset 392 : sysregs[128]  = 128 B             (guest EL1 系统寄存器)
Offset 568 : fp_pad        = 8 B               对齐填充
Offset 576 : fpsimd[66]    = 528 B             guest q0-q31 + FPCR/FPSR

以上偏移由下方 VCPU_* 宏固化, 请勿在 C 侧调整字段顺序。

<a id="fn-include-vmm-vmm.h-763"></a>

### `vmm_vcpu_vm`

```c
static inline vm_t *vmm_vcpu_vm(const vcpu_t *vcpu)
```

── vCPU 运行期视角 ──────────────────────────────────────────

VM-exit 时拿到的永远是发生这次退出的那颗 vCPU(各架构的
vmm_arch_exit_handler(vcpu))。多 vCPU 之后, 处理一次 exit 经常需要回答
三个问题, 所以统一收在这里, 别在设备代码里各自摸 vm->vcpus[]：

我是谁            vmm_vcpu_id() / vmm_vcpu_is_bsp()
我属于哪个 VM      vmm_vcpu_vm()
这个 VM 还有谁     vmm_vm_for_each_vcpu() / vmm_vm_vcpu()
每颗 vCPU 在哪     vmm_vcpu_host_cpu()(-1 = 未运行)

host_cpu 由 vmm_run_vcpu() 在每次进 guest 前用 get_current_cpu_id()
刷新。调度器不做运行期迁移(任务入队即粘核, 见 vcpu_task_create 的说明),
所以它同时也是「这颗 vCPU 当前被钉在哪颗核」。

**位置**：`include/vmm/vmm.h:763`

<a id="fn-include-vmm-vmm.h-768"></a>

### `vmm_vcpu_id`

```c
static inline int vmm_vcpu_id(const vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 vcpu 的 vcpu_id，vcpu 为空时返回 -1。

**位置**：`include/vmm/vmm.h:768`

<a id="fn-include-vmm-vmm.h-773"></a>

### `vmm_vcpu_is_bsp`

```c
static inline bool vmm_vcpu_is_bsp(const vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断 vcpu 非空且 vcpu_id 为 0，即是否为引导处理器。

**位置**：`include/vmm/vmm.h:773`

<a id="fn-include-vmm-vmm.h-779"></a>

### `vmm_vcpu_host_cpu`

```c
static inline int vmm_vcpu_host_cpu(const vcpu_t *vcpu)
```

最近一次进/出 guest 的宿主 CPU；-1 = 还没跑过(或已退出)

**位置**：`include/vmm/vmm.h:779`

<a id="fn-include-vmm-vmm.h-784"></a>

### `vmm_vcpu_task`

```c
static inline struct task *vmm_vcpu_task(const vcpu_t *vcpu)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 vcpu 关联的 task 指针，vcpu 为空时返回 NULL。

**位置**：`include/vmm/vmm.h:784`

<a id="fn-include-vmm-vmm.h-790"></a>

### `vmm_vm_vcpu`

```c
static inline vcpu_t *vmm_vm_vcpu(vm_t *vm, int vcpu_id)
```

按 VM 内部编号取 vCPU(越界返回 NULL)

**位置**：`include/vmm/vmm.h:790`

<a id="fn-include-vmm-vmm.h-798"></a>

### `vmm_vcpu_sibling`

```c
static inline vcpu_t *vmm_vcpu_sibling(const vcpu_t *vcpu, int vcpu_id)
```

取「同一个 VM 里的另一颗 vCPU」—— 跨 VM 返回 NULL

**位置**：`include/vmm/vmm.h:798`

<a id="fn-include-vmm-vmm.h-969"></a>

### `vmm_vcpu_is_online`

```c
static inline int vmm_vcpu_is_online(const vcpu_t *vcpu)
```

AP 是否已经启动过(PSCI AFFINITY_INFO / 诊断用)

**位置**：`include/vmm/vmm.h:969`

<a id="mod-include-vmm-vmm_console.h"></a>

## include/vmm/vmm_console.h — guest 控制台 vdev 的架构无关入口

`include/vmm/vmm_console.h` · 9 个函数 · 9 个有说明 · 164 行

每个架构的 guest 控制台是不同型号的 UART, 但两者对宿主暴露的语义
完全一致(见各自头文件的「输出通路 / 输入通路」两节)：

AArch64 : PL011  @0x09000000, 中断线 33   —— vmm_vpl011.h
RISC-V  : 16550A @0x10000000, PLIC 源 10  —— vmm_uart16550.h
x86_64  : 16550A @0x10000000              —— vmm_uart16550.h

宿主侧只有两处需要这些接口：/dev/vmm(helper 模式搬运字节)和 VMM 主循环
(直启模式轮询宿主控制台)。它们不应该各写一份 #if ARCH_*, 所以在这里
收成一组转发函数。

不放进 vmm.h 的原因：vmm.h 是三个架构共用的类型定义头, 已经要
按架构挑 vGIC 后端；控制台型号再塞进去会让它的条件编译更难读。

<a id="fn-include-vmm-vmm_console.h-51"></a>

### `vmm_console_init`

```c
static inline int vmm_console_init(vm_t *vm, mmio_bus_t *bus)
```

vmm_console_init — 初始化虚拟控制台并注册到 MMIO 总线
返回 0 成功。

<details>
<summary>函数内备注（1 条）</summary>

**第 53 行**

> 两个控制台的状态与设备对象都已搬进各自 .c 的静态池, 不再收 dev

</details>

**位置**：`include/vmm/vmm_console.h:51`

<a id="fn-include-vmm-vmm_console.h-62"></a>

### `vmm_console_tx_set_enabled`

```c
static inline void vmm_console_tx_set_enabled(vm_t *vm, int enabled)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按架构分派到 vpl011 或 16550 的实现，设置控制台发送通道开关。

**位置**：`include/vmm/vmm_console.h:62`

<a id="fn-include-vmm-vmm_console.h-71"></a>

### `vmm_console_tx_channel_enabled`

```c
static inline int vmm_console_tx_channel_enabled(vm_t *vm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按架构分派查询 vpl011 或 16550 的发送通道是否启用。

**位置**：`include/vmm/vmm_console.h:71`

<a id="fn-include-vmm-vmm_console.h-81"></a>

### `vmm_console_tx_pop`

```c
static inline int vmm_console_tx_pop(vm_t *vm, uint8_t *c)
```

取一个 guest 输出字节；缓冲空时返回 0

**位置**：`include/vmm/vmm_console.h:81`

<a id="fn-include-vmm-vmm_console.h-90"></a>

### `vmm_console_tx_has_data`

```c
static inline int vmm_console_tx_has_data(vm_t *vm)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按架构分派查询 vpl011 或 16550 发送通道是否有待发数据。

**位置**：`include/vmm/vmm_console.h:90`

<a id="fn-include-vmm-vmm_console.h-106"></a>

### `vmm_console_putchar`

```c
static inline void vmm_console_putchar(vm_t *vm, uint8_t c)
```

vmm_console_putchar — VMM 自己往 guest 控制台写一个字节

与 guest 写 THR/UARTDR 走同一条通路, 所以 helper 模式下同样进 TX 缓冲。
目前只有 RISC-V 的 SBI console_putchar 用它(aarch64 没有 SBI 控制台,
退回宿主日志即可 —— 那里也必须走 klog_write 这个唯一入口)。

**位置**：`include/vmm/vmm_console.h:106`

<a id="fn-include-vmm-vmm_console.h-117"></a>

### `vmm_console_push_rx`

```c
static inline void vmm_console_push_rx(vm_t *vm, uint8_t c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按架构分派把字节注入 vpl011 或 16550 的接收路径。

**位置**：`include/vmm/vmm_console.h:117`

<a id="fn-include-vmm-vmm_console.h-131"></a>

### `vmm_console_irq_asserted`

```c
static inline int vmm_console_irq_asserted(vm_t *vm)
```

控制台中断线是否应保持有效(电平触发)。
调用方在每次进入 guest 前调用, 为真时 `vmm_arch_irq_raise(vcpu,
virq_line(VIRQ_CONSOLE))` —— 必须每次入口重拉, 见 vmm_virq.h。

**位置**：`include/vmm/vmm_console.h:131`

<a id="fn-include-vmm-vmm_console.h-141"></a>

### `vmm_console_rx_flush`

```c
static inline void vmm_console_rx_flush(vm_t *vm)
```

丢弃未被 guest 取走的输入字节(停在 guest 时用)

**位置**：`include/vmm/vmm_console.h:141`

<a id="mod-include-vmm-vmm_virq.h"></a>

## include/vmm/vmm_virq.h — guest 中断「线」的架构无关命名

`include/vmm/vmm_virq.h` · 1 个函数 · 1 个有说明 · 108 行

只统一到「设备拉哪条线」这一层。线的号码空间由架构决定, 线的投递
语义由各控制器决定, 两者都不在这里统一。

从前是「一个 bool 查询 + 另一个头里的 #define 中断号 + 调用方负责配对」,
三个架构各写一遍同一个模式：

device:    xxx_irq_asserted(vm)          ← 裸 bool
caller:    if (asserted) yyy_set_pending(vm, 某个宏)

现在收成一句 `vmm_arch_irq_raise(vcpu, virq_line(VIRQ_XXX))`。

── 为什么没有 ops 表 / 没有控制器 vtable ────────────────────────────
① 能真正统一的只有一个动作(「置 pending」)。其余全是异构的：
vplic_next_deliverable 是 VMM 主动轮询推进；vlapic_take_pending +
accept_interrupt 是两阶段, 中间还夹一次可能失败的注入；
vmm_vgic_ack/eoi 由 guest 写 MMIO 驱动。套表只会得到一张大部分 NULL
的胖接口。
② 同一个构建里只有一个控制器(aarch64 由 DRIVER_GIC_V3 编译期二选一,
riscv/x86 各只有一个), 运行期多态零收益。
③ sync_entry/sync_exit 是 vGIC 独有的(PLIC/LAPIC 没有 LR 那种硬件影子)。
把它们放进表里, 等于对另外两个后端宣称「你们也有 entry/exit 对称生命周期」,
将来有人在这张表上写调用点会得到静默无操作而不是编译错误 —— 这正是
vgicv3.c 那条「已 ack 未 EOI 的中断必须重装回 LR」的不变量当初的成因。

── 所以「统一」体现在哪 ────────────────────────────────────────────
只在分层与命名：设备 → 线 的 `vmm_arch_irq_raise()` 是新的；线 → 投递的
三个控制器后端原样保留(vmm_vgic{,3}_set_pending / vplic_set_pending /
vlapic_raise_irq)。那四个函数是同一个语义动作的四个后端实现。

── 明确不放进 virq_t 的东西(别加) ────────────────────────────
- vcpu_id：它是入口上下文的属性, 不是线的属性。PLIC 根本没有这个
维度(它的 pending 是源级全局的, `vplic.c` 把同一位置进所有 context),
GIC 要的是「当前正在进入的这个 vcpu」—— 由 vmm_arch_irq_raise 的
`vcpu_t *` 参数给出, 不是线的字段。
- vector：x86 上向量由 guest 自己写进 IO-APIC 重定向表, VMM 无权决定。
- 触发语义(level/edge)：三个控制台都是 level, 「是否每次入口重拉」由
架构入口钩子无条件执行, 不是设备声明的属性。加进来就是个骗人的旋钮。

<a id="fn-include-vmm-vmm_virq.h-70"></a>

### `virq_line`

```c
static inline virq_t virq_line(uint32_t line)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把行号填入 virq_t 结构并返回值，注释称避免复合字面量更稳。

<details>
<summary>函数内备注（1 条）</summary>

**第 73 行（行尾）**

> 用函数而不是复合字面量：freestanding 下更稳

</details>

**位置**：`include/vmm/vmm_virq.h:70`

<a id="mod-include-x86_64-barrier_impl.h"></a>

## include/x86_64/barrier_impl.h — x86_64 内存屏障实现

`include/x86_64/barrier_impl.h` · 7 个函数 · 7 个有说明 · 89 行

使用 MFENCE, SFENCE, LFENCE 指令

<a id="fn-include-x86_64-barrier_impl.h-22"></a>

### `barrier_data`

```c
static inline void barrier_data(void)
```

barrier_data - 完整数据内存屏障

MFENCE: 内存屏障(读+写)

**位置**：`include/x86_64/barrier_impl.h:22`

<a id="fn-include-x86_64-barrier_impl.h-32"></a>

### `barrier_data_read`

```c
static inline void barrier_data_read(void)
```

barrier_data_read - 读数据屏障

LFENCE: 加载屏障

**位置**：`include/x86_64/barrier_impl.h:32`

<a id="fn-include-x86_64-barrier_impl.h-42"></a>

### `barrier_data_write`

```c
static inline void barrier_data_write(void)
```

barrier_data_write - 写数据屏障

SFENCE: 存储屏障

**位置**：`include/x86_64/barrier_impl.h:42`

<a id="fn-include-x86_64-barrier_impl.h-55"></a>

### `barrier_instr_full`

```c
static inline void barrier_instr_full(void)
```

barrier_instr_full - 完整指令同步屏障

x86_64 上 MFENCE 已经提供了完整的同步
CPUID 也可以用作指令序列化器

**位置**：`include/x86_64/barrier_impl.h:55`

<a id="fn-include-x86_64-barrier_impl.h-60"></a>

### `barrier_sync`

```c
static inline void barrier_sync(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

执行 mfence 并加 memory 屏障，保证前后访存顺序。

**位置**：`include/x86_64/barrier_impl.h:60`

<a id="fn-include-x86_64-barrier_impl.h-73"></a>

### `barrier_acquire`

```c
static inline void barrier_acquire(void)
```

barrier_acquire - 获取语义屏障

x86_64 的 TSO (Total Store Order) 模型已经提供了较强的保证
使用编译器屏障即可

**位置**：`include/x86_64/barrier_impl.h:73`

<a id="fn-include-x86_64-barrier_impl.h-84"></a>

### `barrier_release`

```c
static inline void barrier_release(void)
```

barrier_release - 释放语义屏障

x86_64 的 TSO 模型已经提供了较强的保证
使用编译器屏障即可

**位置**：`include/x86_64/barrier_impl.h:84`

<a id="mod-include-x86_64-cache_impl.h"></a>

## include/x86_64/cache_impl.h — x86_64 缓存操作底层实现

`include/x86_64/cache_impl.h` · 9 个函数 · 9 个有说明 · 119 行

架构特定的单个缓存行操作

<a id="fn-include-x86_64-cache_impl.h-30"></a>

### `__clean_dcache_one`

```c
static inline void __clean_dcache_one(const void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 clwb 指令把该地址所在缓存行写回内存。

**位置**：`include/x86_64/cache_impl.h:30`

<a id="fn-include-x86_64-cache_impl.h-35"></a>

### `__clean_dcache_one`

```c
static inline void __clean_dcache_one(const void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 clwb 指令把该地址所在缓存行写回内存。

<details>
<summary>函数内备注（1 条）</summary>

**第 37 行**

> CLFLUSH: 写回并失效

</details>

**位置**：`include/x86_64/cache_impl.h:35`

<a id="fn-include-x86_64-cache_impl.h-50"></a>

### `__invalidate_dcache_one`

```c
static inline void __invalidate_dcache_one(const void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 clflushopt 失效该缓存行，随后调用 barrier_data_write() 保证失效先于后续访存完成。

<details>
<summary>函数内备注（1 条）</summary>

**第 53 行**

> CLFLUSHOPT 与其它写不保序(这点和 CLFLUSH 不同), Intel SDM 要求
> 后面跟一条 SFENCE 才能保证失效先于后续访存完成。少了它, DMA 场景下
> 设备可能读到尚未刷回的内存。

</details>

**位置**：`include/x86_64/cache_impl.h:50`

<a id="fn-include-x86_64-cache_impl.h-61"></a>

### `__invalidate_dcache_one`

```c
static inline void __invalidate_dcache_one(const void *addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 clflushopt 失效该缓存行，随后调用 barrier_data_write() 保证失效先于后续访存完成。

**位置**：`include/x86_64/cache_impl.h:61`

<a id="fn-include-x86_64-cache_impl.h-73"></a>

### `__clean_and_invalidate_dcache_one`

```c
static inline void __clean_and_invalidate_dcache_one(const void *addr)
```

__clean_and_invalidate_dcache_one - 清理并使单个缓存行失效
@addr: 地址

CLFLUSH: 写回并使缓存行失效

**位置**：`include/x86_64/cache_impl.h:73`

<a id="fn-include-x86_64-cache_impl.h-83"></a>

### `get_cache_line_size`

```c
static inline size_t get_cache_line_size(void)
```

get_cache_line_size - 获取缓存行大小

**位置**：`include/x86_64/cache_impl.h:83`

<a id="fn-include-x86_64-cache_impl.h-91"></a>

### `init_cache`

```c
static inline void init_cache(void)
```

init_cache - 初始化缓存子系统

<details>
<summary>函数内备注（2 条）</summary>

**第 93 行**

> x86_64 标准缓存行大小为 64 字节

**第 94 行**

> 可以通过 CPUID 指令检测, 但 64 字节是通用标准

</details>

**位置**：`include/x86_64/cache_impl.h:91`

<a id="fn-include-x86_64-cache_impl.h-109"></a>

### `sync_icache_all`

```c
static inline void sync_icache_all(void)
```

sync_icache_all - 让刚写入的指令可见

x86 的 I-cache 与 D-cache 由硬件保持一致, 写完代码只需一条序列化屏障。
(严格的自改码场景还要求执行 cpuid/jmp 之类的序列化指令, 本内核没有 JIT。)

**位置**：`include/x86_64/cache_impl.h:109`

<a id="fn-include-x86_64-cache_impl.h-114"></a>

### `sync_caches`

```c
static inline void sync_caches(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

仅调用 barrier_data() 建立数据屏障。

**位置**：`include/x86_64/cache_impl.h:114`

<a id="mod-include-x86_64-cpu_impl.h"></a>

## include/x86_64/cpu_impl.h — x86_64 per-CPU 指针 / 硬件 ID 实现

`include/x86_64/cpu_impl.h` · 3 个函数 · 3 个有说明 · 55 行

约定：
IA32_GS_BASE        (MSR 0xC0000101) — 用户态运行时保存 cpu_t*
IA32_KERNEL_GS_BASE (MSR 0xC0000102) — 内核态运行时保存 cpu_t*

syscall_entry 执行 swapgs 后 GS_BASE = cpu_t*(从 KERNEL_GS_BASE 换入),
可通过 gs:offset 访问 per-CPU 数据。sysretq 前再次 swapgs 换回。

内核纯态路径(非 SYSCALL)仍通过 rdmsr(GS_BASE) 读取 cpu_t*。

<a id="fn-include-x86_64-cpu_impl.h-30"></a>

### `arch_cpu_self_set`

```c
static inline void arch_cpu_self_set(struct cpu *self)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 self 的 64 位值拆成高低 32 位，分别 wrmsr 写入 GS_BASE 与 KERNEL_GS_BASE。

**位置**：`include/x86_64/cpu_impl.h:30`

<a id="fn-include-x86_64-cpu_impl.h-39"></a>

### `arch_cpu_self_get`

```c
static inline struct cpu *arch_cpu_self_get(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 rdmsr 读 GS_BASE，拼成 64 位后强转为 struct cpu 指针返回。

**位置**：`include/x86_64/cpu_impl.h:39`

<a id="fn-include-x86_64-cpu_impl.h-50"></a>

### `arch_cpu_hw_id`

```c
static inline uint64_t arch_cpu_hw_id(void)
```

LAPIC ID 需要 LAPIC 映射就绪后才能读；Phase 0 返回 0 占位,
后续 SMP 阶段改为 cpu_current()->hw_id(BSP 启动时存入)。

**位置**：`include/x86_64/cpu_impl.h:50`

<a id="mod-include-x86_64-exception_impl.h"></a>

## include/x86_64/exception_impl.h

`include/x86_64/exception_impl.h` · 6 个函数 · 6 个有说明 · 91 行

<a id="fn-include-x86_64-exception_impl.h-25"></a>

### `arch_irq_flags`

```c
static inline uint64_t arch_irq_flags(void)
```

读当前 RFLAGS(不改动状态)

**位置**：`include/x86_64/exception_impl.h:25`

<a id="fn-include-x86_64-exception_impl.h-35"></a>

### `arch_irq_is_enabled`

```c
static inline int arch_irq_is_enabled(void)
```

读是否允许 IRQ(抢占判断用)

**位置**：`include/x86_64/exception_impl.h:35`

<a id="fn-include-x86_64-exception_impl.h-41"></a>

### `arch_irq_save`

```c
static inline uint64_t arch_irq_save(void)
```

关中断并返回旧 RFLAGS(必须用 arch_irq_restore 配对恢复)

**位置**：`include/x86_64/exception_impl.h:41`

<a id="fn-include-x86_64-exception_impl.h-60"></a>

### `arch_irq_restore`

```c
static inline void arch_irq_restore(uint64_t flags)
```

恢复到 arch_irq_save()/arch_irq_flags() 取到的状态(POPFQ 恢复 IF)

**位置**：`include/x86_64/exception_impl.h:60`

<a id="fn-include-x86_64-exception_impl.h-74"></a>

### `arch_irq_disable`

```c
static inline void arch_irq_disable(void)
```

无条件关中断(不需要旧状态时用)

**位置**：`include/x86_64/exception_impl.h:74`

<a id="fn-include-x86_64-exception_impl.h-83"></a>

### `arch_irq_enable`

```c
static inline void arch_irq_enable(void)
```

无条件开中断(新任务首次运行时使用)

**位置**：`include/x86_64/exception_impl.h:83`

<a id="mod-include-x86_64-halt_arch.h"></a>

## include/x86_64/halt_arch.h — x86_64 架构特定的 halt 实现

`include/x86_64/halt_arch.h` · 1 个函数 · 1 个有说明 · 40 行

<a id="fn-include-x86_64-halt_arch.h-22"></a>

### `arch_halt`

```c
static inline void arch_halt(void)
```

arch_halt - 停止 x86_64 处理器
使用 HLT 指令停止处理器, 直到收到中断
在中断禁用的情况下, HLT 会永久停止

<details>
<summary>函数内备注（5 条）</summary>

**第 24 行**

> 内存屏障, 确保所有操作完成

**第 27 行**

> 禁用中断(统一接口)

**第 30 行**

> HLT - Halt Processor

**第 31 行**

> 在中断禁用的情况下, 处理器会永久停止

**第 34 行**

> 如果 HLT 被唤醒(不应该发生), 死循环

</details>

**位置**：`include/x86_64/halt_arch.h:22`

<a id="mod-include-x86_64-io.h"></a>

## include/x86_64/io.h — x86_64 Port I/O

`include/x86_64/io.h` · 6 个函数 · 6 个有说明 · 63 行

x86 拥有独立于内存地址空间的 I/O 端口空间(64K 个端口),
通过专用指令 in/out 访问, 与 MMIO 的 volatile 指针访问无关。

x86 的 in/out 指令自带序列化语义, 无需额外内存屏障。

<a id="fn-include-x86_64-io.h-23"></a>

### `inb`

```c
static inline uint8_t inb(uint16_t port)
```

x86_64 Port I/O

x86 拥有独立于内存地址空间的 I/O 端口空间(64K 个端口),
通过专用指令 in/out 访问, 与 MMIO 的 volatile 指针访问无关。

x86 的 in/out 指令自带序列化语义, 无需额外内存屏障。

**位置**：`include/x86_64/io.h:23`

<a id="fn-include-x86_64-io.h-30"></a>

### `outb`

```c
static inline void outb(uint16_t port, uint8_t value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 outb 指令向指定端口写 8 位值。

**位置**：`include/x86_64/io.h:30`

<a id="fn-include-x86_64-io.h-37"></a>

### `inw`

```c
static inline uint16_t inw(uint16_t port)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 inw 指令从指定端口读 16 位值并返回。

**位置**：`include/x86_64/io.h:37`

<a id="fn-include-x86_64-io.h-44"></a>

### `outw`

```c
static inline void outw(uint16_t port, uint16_t value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 outw 指令向指定端口写 16 位值。

**位置**：`include/x86_64/io.h:44`

<a id="fn-include-x86_64-io.h-51"></a>

### `inl`

```c
static inline uint32_t inl(uint16_t port)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 inl 指令从指定端口读 32 位值并返回。

**位置**：`include/x86_64/io.h:51`

<a id="fn-include-x86_64-io.h-58"></a>

### `outl`

```c
static inline void outl(uint16_t port, uint32_t value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 outl 指令向指定端口写 32 位值。

**位置**：`include/x86_64/io.h:58`

<a id="mod-include-x86_64-mm_vm.h"></a>

## include/x86_64/mm_vm.h — x86_64 页表操作接口

`include/x86_64/mm_vm.h` · 3 个函数 · 3 个有说明 · 68 行

本头文件由 mm_vm.h 包含, phys_to_virt/virt_to_phys 已在包含前定义。
实现位于 kernel/mm/x86_64/vmm.c。

<a id="fn-include-x86_64-mm_vm.h-21"></a>

### `x86_pa_to_kva`

```c
static inline void *x86_pa_to_kva(uint64_t pa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

直接转调 phys_to_virt 把物理地址换算为内核虚拟地址。

**位置**：`include/x86_64/mm_vm.h:21`

<a id="fn-include-x86_64-mm_vm.h-26"></a>

### `x86_next_level`

```c
static inline uint64_t *x86_next_level(uint64_t pte)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 x86_pte_to_pa 取出下一级页表物理地址并转成虚拟地址返回。

**位置**：`include/x86_64/mm_vm.h:26`

<a id="fn-include-x86_64-mm_vm.h-45"></a>

### `mm_vm_set_pte_keep`

```c
static inline void mm_vm_set_pte_keep(void *pgd, uint64_t vaddr)
```

把一页标成"设备缓冲"：unmap 时只拆映射, 不释放页也不碰引用计数。
用于把 dma_buf / ION 的缓冲区映射进用户态 —— 页归句柄所有, 不归映射。
语义与 PTE_NOFREE 的区别见 mmu.h 里 PTE_KEEP 的说明。

**位置**：`include/x86_64/mm_vm.h:45`

<a id="mod-include-x86_64-mmu.h"></a>

## include/x86_64/mmu.h — ══════════════════════════════════════════════════════════════════

`include/x86_64/mmu.h` · 8 个函数 · 8 个有说明 · 285 行

x86_64 页表结构(4 级页表)
══════════════════════════════════════════════════════════════════

虚拟地址布局(48 位)：
[47:39] PML4  (Page Map Level 4)      - 9 bits, 512 entries
[38:30] PDPT  (Page Directory Pointer) - 9 bits, 512 entries
[29:21] PD    (Page Directory)         - 9 bits, 512 entries
[20:12] PT    (Page Table)             - 9 bits, 512 entries
[11:0]  Offset                         - 12 bits

内核映射：
内核虚拟地址：0xffff800000000000 - 0xffffffffffffffff (高 128 TB)
用户虚拟地址：0x0000000000000000 - 0x00007fffffffffff (低 128 TB)

<a id="fn-include-x86_64-mmu.h-178"></a>

### `x86_pte_to_pa`

```c
static inline uint64_t x86_pte_to_pa(uint64_t pte)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 PTE_ADDR_MASK 掩掉标志位，取页表项中的物理地址。

**位置**：`include/x86_64/mmu.h:178`

<a id="fn-include-x86_64-mmu.h-183"></a>

### `x86_make_table_pte`

```c
static inline uint64_t x86_make_table_pte(uint64_t pa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把物理地址加上 PRESENT、WRITABLE、USER 位构造表型页表项。

**位置**：`include/x86_64/mmu.h:183`

<a id="fn-include-x86_64-mmu.h-188"></a>

### `x86_pte_is_leaf`

```c
static inline bool x86_pte_is_leaf(uint64_t pte, int level)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

level 大于 0 且有 HUGE 位判为叶子；level 为 0 时看 PRESENT 位，其余返回 false。

**位置**：`include/x86_64/mmu.h:188`

<a id="fn-include-x86_64-mmu.h-200"></a>

### `read_cr3`

```c
static inline uint64_t read_cr3(void)
```

读取 CR3(当前页表基址)

**位置**：`include/x86_64/mmu.h:200`

<a id="fn-include-x86_64-mmu.h-208"></a>

### `write_cr3`

```c
static inline void write_cr3(uint64_t value)
```

写入 CR3(切换页表)

**位置**：`include/x86_64/mmu.h:208`

<a id="fn-include-x86_64-mmu.h-214"></a>

### `flush_tlb`

```c
static inline void flush_tlb(void)
```

刷新 TLB

**位置**：`include/x86_64/mmu.h:214`

<a id="fn-include-x86_64-mmu.h-221"></a>

### `flush_tlb_single`

```c
static inline void flush_tlb_single(uint64_t vaddr)
```

刷新单个虚拟地址的 TLB

**位置**：`include/x86_64/mmu.h:221`

<a id="fn-include-x86_64-mmu.h-273"></a>

### `x86_copy_kernel_mappings`

```c
static inline void x86_copy_kernel_mappings(uint64_t *user_pml4, const uint64_t *kernel_pml4)
```

x86_copy_kernel_mappings - 将内核高半区 PML4[256..511] 复制到用户页表
@user_pml4:   目标用户 PML4(内核虚拟地址, uint64_t[512])
@kernel_pml4: 源内核 PML4(内核虚拟地址, uint64_t[512])

**位置**：`include/x86_64/mmu.h:273`

<a id="mod-include-x86_64-spin_lock_impl.h"></a>

## include/x86_64/spin_lock_impl.h — x86_64 架构的spinlock实现

`include/x86_64/spin_lock_impl.h` · 6 个函数 · 6 个有说明 · 98 行

使用 xchg 指令实现原子交换
使用 mfence 指令进行内存屏障

<a id="fn-include-x86_64-spin_lock_impl.h-25"></a>

### `spin_lock`

```c
static inline void spin_lock(spinlock_t *lock)
```

x86_64 的原子操作使用 xchg 指令
xchg 会自动锁定总线, 保证原子性

<details>
<summary>函数内备注（4 条）</summary>

**第 29 行（行尾）**

> 原子交换：val = lock-&gt;lock; lock-&gt;lock = 1

**第 30 行（行尾）**

> 测试 val 是否为 0

**第 31 行（行尾）**

> 如果不为 0, 说明锁占用, 继续自旋

**第 32 行（行尾）**

> 内存屏障, 确保后续操作有序

</details>

**位置**：`include/x86_64/spin_lock_impl.h:25`

<a id="fn-include-x86_64-spin_lock_impl.h-38"></a>

### `spin_trylock`

```c
static inline int spin_trylock(spinlock_t *lock)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

关抢占后用 lock xchg 尝试把锁置 1，成功返回 0，失败恢复抢占并返回 1。

<details>
<summary>函数内备注（4 条）</summary>

**第 43 行（行尾）**

> 尝试获取锁

**第 45 行（行尾）**

> result = 0

**第 46 行（行尾）**

> 如果非 0, 跳转

**第 48 行（行尾）**

> result = 1

</details>

**位置**：`include/x86_64/spin_lock_impl.h:38`

<a id="fn-include-x86_64-spin_lock_impl.h-58"></a>

### `spin_unlock`

```c
static inline void spin_unlock(spinlock_t *lock)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

执行 mfence 后把锁字清零释放，并重新开启抢占。

<details>
<summary>函数内备注（2 条）</summary>

**第 60 行（行尾）**

> 内存屏障

**第 61 行（行尾）**

> lock-&gt;lock = 0

</details>

**位置**：`include/x86_64/spin_lock_impl.h:58`

<a id="fn-include-x86_64-spin_lock_impl.h-77"></a>

### `spin_lock_irqsave`

```c
static inline void spin_lock_irqsave(spinlock_t *lock, uint64_t *flags)
```

带中断保护的 spinlock。

中断状态存在调用点的局部变量里(由 *flags 带回), 不再存进锁对象 ——
存进锁对象时, SMP 下争锁的另一颗 CPU 会把它的 flags 覆盖上去, 解锁时
恢复的就是别人的中断状态。中断原语本身统一来自
include/x86_64/exception_impl.h(arch_irq_save/restore)。

**位置**：`include/x86_64/spin_lock_impl.h:77`

<a id="fn-include-x86_64-spin_lock_impl.h-83"></a>

### `spin_trylock_irqsave`

```c
static inline int spin_trylock_irqsave(spinlock_t *lock, uint64_t *flags)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

保存并关中断后尝试取锁，失败恢复中断返回 1，成功返回 0。

**位置**：`include/x86_64/spin_lock_impl.h:83`

<a id="fn-include-x86_64-spin_lock_impl.h-92"></a>

### `spin_unlock_irqrestore`

```c
static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先释放锁，再恢复调用方保存的中断状态。

**位置**：`include/x86_64/spin_lock_impl.h:92`

<a id="mod-include-x86_64-string_impl.h"></a>

## include/x86_64/string_impl.h — x86_64 架构的字符串函数优化实现

`include/x86_64/string_impl.h` · 2 个函数 · 2 个有说明 · 29 行

目前转发到通用实现。要注意的是本架构内核编译带 `-mno-mmx -mno-sse`
(见 Makefile), 所以要做 SSE2/AVX 版本, 得先给这几个函数所在的翻译单元
单独开浮点/向量开关, 并按 aarch64 的做法把 FP 状态纳入上下文保存。

<a id="fn-include-x86_64-string_impl.h-19"></a>

### `memcpy_arch`

```c
static inline void *memcpy_arch(void *dest, const void *src, size_t n)
```

x86_64 架构的字符串函数优化实现

目前转发到通用实现。要注意的是本架构内核编译带 `-mno-mmx -mno-sse`
(见 Makefile), 所以要做 SSE2/AVX 版本, 得先给这几个函数所在的翻译单元
单独开浮点/向量开关, 并按 aarch64 的做法把 FP 状态纳入上下文保存。

**位置**：`include/x86_64/string_impl.h:19`

<a id="fn-include-x86_64-string_impl.h-24"></a>

### `memset_arch`

```c
static inline void *memset_arch(void *s, int c, size_t n)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

转调 memset_generic 完成内存填充。

**位置**：`include/x86_64/string_impl.h:24`

<a id="mod-include-x86_64-syscall_abi.h"></a>

## include/x86_64/syscall_abi.h

`include/x86_64/syscall_abi.h` · 6 个函数 · 6 个有说明 · 83 行

<a id="fn-include-x86_64-syscall_abi.h-13"></a>

### `syscall_abi_nr`

```c
static inline uint64_t syscall_abi_nr(const trap_frame_t *frame)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 trap frame 的 rax 读出系统调用号。

<details>
<summary>函数内备注（1 条）</summary>

**第 15 行（行尾）**

> syscall number in rax

</details>

**位置**：`include/x86_64/syscall_abi.h:13`

<a id="fn-include-x86_64-syscall_abi.h-18"></a>

### `syscall_abi_arg`

```c
static inline uint64_t syscall_abi_arg(const trap_frame_t *frame, int idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 idx 依次返回 rdi、rsi、rdx、r10、r8、r9，越界返回 0。

**位置**：`include/x86_64/syscall_abi.h:18`

<a id="fn-include-x86_64-syscall_abi.h-38"></a>

### `syscall_abi_set_ret`

```c
static inline void syscall_abi_set_ret(trap_frame_t *frame, uint64_t ret)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把返回值写入 trap frame 的 rax。

**位置**：`include/x86_64/syscall_abi.h:38`

<a id="fn-include-x86_64-syscall_abi.h-43"></a>

### `syscall_abi_ip`

```c
static inline uint64_t syscall_abi_ip(const trap_frame_t *frame)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 trap frame 中保存的 rip。

**位置**：`include/x86_64/syscall_abi.h:43`

<a id="fn-include-x86_64-syscall_abi.h-48"></a>

### `syscall_abi_user_sp`

```c
static inline uint64_t syscall_abi_user_sp(const trap_frame_t *frame)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 trap frame 中保存的 rsp。

**位置**：`include/x86_64/syscall_abi.h:48`

<a id="fn-include-x86_64-syscall_abi.h-61"></a>

### `syscall_abi_reset_frame_for_exec`

```c
static inline void syscall_abi_reset_frame_for_exec(trap_frame_t *frame, uint64_t entry, uint64_t sp)
```

syscall_abi_reset_frame_for_exec - execve 成功后把 trap frame 重置成
新程序的初始用户现场

syscall_wrapper.S 的返回路径用 trap frame 里的 RCX/R11/RSP 做 sysretq，
所以入口/栈/标志位都从这里取。FP 状态没有 per-task 保存，不在本函数
范围内(和现有首次进入路径一致)。CS/SS 保持入口压入的 0x2B/0x23。

<details>
<summary>函数内备注（1 条）</summary>

**第 79 行（行尾）**

> IF=1, Reserved=1

</details>

**位置**：`include/x86_64/syscall_abi.h:61`

<a id="mod-include-x86_64-vmx.h"></a>

## include/x86_64/vmx.h — Intel VMX 常量, VMCS 字段编码与辅助类型

`include/x86_64/vmx.h` · 18 个函数 · 18 个有说明 · 464 行

对标 ref/bare-vm/arch/x86_64/include/vmx.h, 精简适配 Avatar OS。

<a id="fn-include-x86_64-vmx.h-319"></a>

### `vmx_rdmsr`

```c
static inline uint64_t vmx_rdmsr(uint32_t idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 rdmsr 读指定 MSR，把高低 32 位拼成 64 位返回。

**位置**：`include/x86_64/vmx.h:319`

<a id="fn-include-x86_64-vmx.h-326"></a>

### `vmx_wrmsr`

```c
static inline void vmx_wrmsr(uint32_t idx, uint64_t val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 64 位值拆成高低 32 位，用 wrmsr 写入指定 MSR。

**位置**：`include/x86_64/vmx.h:326`

<a id="fn-include-x86_64-vmx.h-331"></a>

### `vmx_read_cr0`

```c
static inline uint64_t vmx_read_cr0(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 mov 读取 CR0 并返回。

**位置**：`include/x86_64/vmx.h:331`

<a id="fn-include-x86_64-vmx.h-337"></a>

### `vmx_read_cr3`

```c
static inline uint64_t vmx_read_cr3(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 mov 读取 CR3 并返回。

**位置**：`include/x86_64/vmx.h:337`

<a id="fn-include-x86_64-vmx.h-343"></a>

### `vmx_read_cr4`

```c
static inline uint64_t vmx_read_cr4(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 mov 读取 CR4 并返回。

**位置**：`include/x86_64/vmx.h:343`

<a id="fn-include-x86_64-vmx.h-349"></a>

### `vmx_write_cr0`

```c
static inline void vmx_write_cr0(uint64_t v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 mov 把给定值写入 CR0，并附 memory 屏障。

**位置**：`include/x86_64/vmx.h:349`

<a id="fn-include-x86_64-vmx.h-353"></a>

### `vmx_write_cr4`

```c
static inline void vmx_write_cr4(uint64_t v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 mov 把给定值写入 CR4，并附 memory 屏障。

**位置**：`include/x86_64/vmx.h:353`

<a id="fn-include-x86_64-vmx.h-358"></a>

### `vmx_read_rflags`

```c
static inline uint64_t vmx_read_rflags(void)
```

读 RFLAGS：与 arch_irq_flags() 是同一条指令, 直接转调, 不重复写汇编

**位置**：`include/x86_64/vmx.h:358`

<a id="fn-include-x86_64-vmx.h-362"></a>

### `vmx_cpuid`

```c
static inline void vmx_cpuid(uint32_t op, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 op 为叶子、ecx 置 0 执行 cpuid，把各寄存器结果写回传入指针。

**位置**：`include/x86_64/vmx.h:362`

<a id="fn-include-x86_64-vmx.h-370"></a>

### `vmcs_read`

```c
static inline uint64_t vmcs_read(vmcs_field_t enc)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 vmread 按编码读取 VMCS 字段值并返回。

**位置**：`include/x86_64/vmx.h:370`

<a id="fn-include-x86_64-vmx.h-377"></a>

### `vmcs_write`

```c
static inline int vmcs_write(vmcs_field_t enc, uint64_t val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 vmwrite 写 VMCS 字段，并用 setbe 捕获 CF/ZF 作为失败标志返回。

**位置**：`include/x86_64/vmx.h:377`

<a id="fn-include-x86_64-vmx.h-401"></a>

### `vmx_on`

```c
static inline int vmx_on(uint64_t pa)
```

返回 0 = 成功；非 0 = 指令失败(CF/ZF)

**位置**：`include/x86_64/vmx.h:401`

<a id="fn-include-x86_64-vmx.h-410"></a>

### `vmcs_clear_pa`

```c
static inline int vmcs_clear_pa(uint64_t pa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先置 RFLAGS 的 CF/ZF，执行 vmclear 清除指定 VMCS，用 setbe 返回失败标志。

**位置**：`include/x86_64/vmx.h:410`

<a id="fn-include-x86_64-vmx.h-419"></a>

### `vmcs_load_pa`

```c
static inline int vmcs_load_pa(uint64_t pa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先置 RFLAGS 的 CF/ZF，执行 vmptrld 加载指定 VMCS 物理地址，用 setbe 返回失败标志。

**位置**：`include/x86_64/vmx.h:419`

<a id="fn-include-x86_64-vmx.h-433"></a>

### `vmx_sgdt`

```c
static inline void vmx_sgdt(vmx_desc_ptr_t *desc)
```

SGDT/SIDT 写 10 字节：2 字节 limit + 8 字节 base。
描述符结构必须 packed —— 默认对齐会让 base 落在 offset 8, 指令却按
offset 2 写, 读出来就错位了。

**位置**：`include/x86_64/vmx.h:433`

<a id="fn-include-x86_64-vmx.h-438"></a>

### `vmx_sidt`

```c
static inline void vmx_sidt(vmx_desc_ptr_t *desc)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 sidt 指令把 IDTR 内容写入给定描述符结构。

**位置**：`include/x86_64/vmx.h:438`

<a id="fn-include-x86_64-vmx.h-447"></a>

### `vmx_read_cr2`

```c
static inline uint64_t vmx_read_cr2(void)
```

读 CR2。VM-exit 不保存/恢复 CR2, 所以 guest 缺页后退出到 VMM 时这里读到的
仍是 guest 的缺页地址 —— 这是诊断 #PF 的唯一窗口(见 vmx.c 的 EXC_NMI 分支)。

**位置**：`include/x86_64/vmx.h:447`

<a id="fn-include-x86_64-vmx.h-456"></a>

### `vmx_read_tr`

```c
static inline uint16_t vmx_read_tr(void)
```

读 TR 选择子。用来校验 VM-exit 装回来的 TSS 是不是本核的(见 vmx.c)。

**位置**：`include/x86_64/vmx.h:456`
