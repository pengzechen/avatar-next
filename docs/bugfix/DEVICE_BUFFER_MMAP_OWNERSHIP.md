# 设备缓冲区映射进用户态会被 munmap / 进程退出错误释放

> **动 `vm_unmap_user_range()`、`memory_free_page()`、`destroy_uvm_4level()`、
> `rv_destroy_table()` / `x86_destroy_table()`、或任何往用户态映射设备内存的
> 代码之前，先读这篇。**
>
> 发现于 P0（dma-buf / dma-heap）验收，见 `docs/npu/RK3588_NPU_DRIVER.md`。

---

## 1. 症状

跑 `tools/dmatest_p0.sh` 时内核日志被刷屏：

```
[WARN][C0] kernel/mm/pmm.c:179: PMM: partial free detected:
           requested=1, actually_freed=0, paddr=0x41330000
```

- 4 KiB 缓冲区 → `requested=1,   actually_freed=0`
- 1 MiB 缓冲区 → `requested=256, actually_freed=0`

`actually_freed=0` 说明**释放时这些页在位图里根本不是"已分配"状态**。

**这个 warning 只是记账对不上的表现，真正的危害在别处**（见 §3）。

---

## 2. 根因

把一块**内核持有的设备内存**（ION 缓冲区 / dma-buf）映射进用户态之后：

1. 用户 `munmap` → `vm_unmap_user_range()` → `memory_free_page()`（aarch64）
   把页表项背后的物理页 **`pmm_free_pages()` 掉**；
2. 而这块内存归 **ION 句柄 / dma_buf 对象**所有，它还指着那些页；
3. 随后 `close(fd)` → `ion_free()` / `dma_heap_free()` **再释放一次**
   → 就是那条 `actually_freed=0`。

**为什么以前没暴露**：唯一会走这条路的映射路径是 ION，而 ION 只在
`TPU=cvitpu` 时启用、cvi_tpu 是 riscv 专属 —— 所以 `ION + mmap` 这个组合在
aarch64 / x86_64 上从来没跑过。dma-buf 一上来就在三个架构上都跑通了。

另外，原来那条映射路径里标的是 `RV_PTE_NOFREE`，而且**只在
`#if ARCH_RISCV64` 下标**：

```c
#if ARCH_RISCV64
        uint64_t *pte = rv_walk_l0_pte(pgd, va, false);
        if (pte)
            *pte |= RV_PTE_NOFREE;
#endif
```

就算在 riscv 上标了也不管用 —— `shared_page_unref()` 在表里找不到记录时会
**直接 `pmm_free_pages()`**（`kernel/mm/shared_page.c` 末尾那个没有
`return` 的兜底）。

---

## 3. 真正的危害（比 double free 更严重）

两次释放之间有一段窗口：**页已经回到 PMM 空闲池，而 dma_buf 仍然指着它**。

对 NPU 缓冲区来说，这等于「**设备往别人的内存里写**」——
一个随时会变成静默数据损坏、且极难复现的 use-after-free。

所以这条不是"日志噪音"，是必须修的内存安全问题。

---

## 4. 修法：新增 `PTE_KEEP`

`PTE_NOFREE` 表达的是「**这个映射**是共享页，别在这里释放，交给
`shared_page` 引用计数决定」—— 最后一个 unref 的人负责真正释放。

设备缓冲区需要的是另一种语义：「这一页**根本不属于这个映射**，归句柄所有」。
所以新增一个位：

| 架构 | 位 | 依据 |
|---|---|---|
| aarch64 | `PTE_KEEP = 1ULL << 56`（`include/aarch64/mmu.h`）| ARMv8 4KB stage-1 描述符的 55~58 是软件位；Linux arm64 也用 56（`PTE_SPECIAL`）|
| x86_64 | `PTE_KEEP = PTE_SW_BIT2`（bit 10）| 现有 `PTE_SW_BIT1/2/3` 里 9 已被 NOFREE 占用 |
| riscv64 | `RV_PTE_KEEP = 1ULL << 9` | RSW 两位（8、9）由 RISC-V 规范保留给软件 |

**语义：unmap / 销毁地址空间时只拆页表项，既不释放页、也不碰引用计数。**
释放永远是 `dma_heap_free()` / `ion_free()` 的事。

**不能用 NOFREE 顶替**，两个理由：
- `shared_page` 表只有 **256 项**（一个 1 MiB 缓冲区就塞满，NPU 缓冲区动辄几十 MB）；
- 那张表**没有锁**，SMP 下有竞争；
- 表满/找不到记录时会退回 `pmm_free_pages()`，等于没保护。

---

## 5. ⚠️ 必须**同时**改的四处

这是这篇文档最值钱的部分：**释放页的路径有四条，分属两套遍历**。
只改一条的症状是"大部分场景好了，但 fork 之后还漏一页"——
实测就是这样：改完前三条后 fork 用例仍然冒出**恰好 1 条** `partial free`。

| # | 位置 | 场景 |
|---|---|---|
| 1 | `kernel/mm/aarch64/vmm.c` `memory_free_page()` | aarch64 `munmap` |
| 2 | `kernel/mm/aarch64/vmm.c` `_destroy_page_table_vm()` | aarch64 **进程退出**（与 1 是两趟不同的遍历！）|
| 3 | `kernel/mm/vm_user.c` `vm_unmap_user_range()` 的 riscv 分支 | riscv `munmap` |
| 4 | `kernel/mm/vm_user.c` `rv_destroy_table()` / `x86_destroy_table()` | riscv / x86 **进程退出** |

另外**复制**路径也要认它，否则 fork 出来的子进程会拿到一份**私有副本**
（内存安全，但设备缓冲区在父子间静默分叉 —— 不崩，只让结果对不上）：

| # | 位置 |
|---|---|
| 5 | `kernel/syscall/core/proc_lifecycle.c` 的 `CLONE_COPY_RANGE`（`CLONE_CHECK_KEEP` / `CLONE_MARK_KEEP`）|
| 6 | `kernel/mm/aarch64/vmm.c` `_copy_page_table()` —— 复制 PTE 时不加 `shared_page_ref` |
| 7 | `kernel/mm/riscv64/vmm.c` / `x86_64/vmm.c` 的 `*_copy_pt_recursive()` —— 同上 |

映射侧统一走 **`mm_vm_set_pte_keep(pgd, va)`**（三个架构的 `mm_vm.h` 各有一个
static inline），不要在各处自己 walk 页表。

---

## 6. 验证

```
make PLATFORM=qemu-virt-aarch64 LOG=info DMA=1 clean
make PLATFORM=qemu-virt-aarch64 LOG=info DMA=1 kernel rootfs
tools/dmatest_p0.sh aarch64
```

判据：
- `=== 结果: 5 通过, 0 失败 ===`
- 日志里 `partial free` **0 条**（修之前 311 条）
- 其中 **fork 用例**是专门为第 5 条路径加的 —— 少了它，第 2 条那个漏网会
  一直藏着（内存安全但语义错，不会崩）

> 注意 `dmatest` 里那条 `[mmap] dmabuf fd=4 len=0x2000 exceeds size=0x1000`
> 是**故意**打的边界用例，不是故障。

---

## 7. 回归范围

aarch64 是 rknpu 的目标平台，已实测通过。
**riscv64 / x86_64 只做了"能编译"级别的确认**（`make ... DMA=1 kernel`
0 error 0 warning），没有跑过带设备缓冲区的用户态用例 —— 那边
`ION=dma-buf` 的实际使用路径尚未建立。以后有需要时补。
