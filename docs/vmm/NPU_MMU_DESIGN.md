# 让宿主独占 RK3588 NPU 的 MMU（设计，**尚未实现**）

**这份文档描述的是还没写的东西。** 现状见 `docs/vmm/AARCH64_GUEST_NPU.md` ——
那里记的是已经跑通的透传，以及它最后一个未解决的问题。

## 要解决什么

guest 里 NPU 跑出来的结果是垃圾，根因已经定位（真机实测）：

```
[vnpu] ⚠ regcmd[126] = 0x78015044 是越界地址 —— 不在 NPU 窗口 [0x79000000, 0x7c000000)
```

`0x78015044` 是普通 guest RAM。**NPU 是没有 IOMMU 的物理设备，只认物理地址**，
而 guest 的 RAM 是按需分页、物理不连续 —— 只有那 48 MiB 恒等窗口里 IPA 才等于 PA。
NPU 按那个地址去读，读到的是别人的页。

更深一层的原因：**vendor 驱动的 non-iommu 路径假设"设备够得着所有 RAM"**
（`rknpu_drv.c:802` 那句 `using non-iommu mode`）。裸机上这个假设成立；
我们的 VM 里不成立 —— 因为我们把 `iommus` 从 guest DTB 里删掉了。

## 为什么是"宿主独占"而不是"给 guest 用"

RK3588 这块是 `compatible = "rockchip,iommu-v2"` —— **Rockchip 自研，不是 ARM SMMU**。
在 `rknpu_iommu.c`（619 行）里查过：**全文没有 stage 概念**，它只用通用 IOMMU API。
也就是**单级**：一张表，谁写谁说了算。

- 交给 guest 写 → guest 能把任意物理地址映射进来 → **隔离为零** ✗
- 宿主写 → 只有宿主愿意映射的页可达 ✓

所以让宿主独占它。这反而正合适：

```
guest 驱动（原样，仍以为没有 IOMMU）：写"物理地址" = IPA
        │
   宿主把 IPA 填进 NPU MMU 的表，指向真实的宿主页
        │
   NPU ──发出 IPA──▶ NPU MMU ──IPA→PA──▶ 宿主物理页（散着也行）
```

**guest 一个字都不用改**（继续走 non-iommu 路径，继续用 IPA 当物理地址），
恒等窗口整个不需要了，隔离也回来了。

## 硬件接口（从 mainline 的 `drivers/iommu/rockchip-iommu.c` 抄的，1408 行）

mainline **原生支持 v2** ✓（`iommu_data_ops_v2`），所以格式不用猜。

### 寄存器（每个 MMU 块 0x100）

| 偏移 | 名字 | 说明 |
|---|---|---|
| 0x00 | `DTE_ADDR` | 目录表物理地址 |
| 0x04 | `STATUS` | 见下 |
| 0x08 | `COMMAND` | 见下。**写命令，不是写数据** |
| 0x0C | `PAGE_FAULT_ADDR` | 最后一次缺页的 IOVA |
| 0x10 | `ZAP_ONE_LINE` | 失效单条 IOTLB |
| 0x14 | `INT_RAWSTAT` | 中断原始状态 |
| 0x18 | `INT_CLEAR` | 应答并重新武装 |
| 0x1C | `INT_MASK` | 中断使能 |
| 0x20 | `INT_STATUS` | 掩码后的状态 |
| 0x24 | `AUTO_GATING` | |

`STATUS` 位：`PAGING_ENABLED`(0) / `PAGE_FAULT_ACTIVE`(1) / `STALL_ACTIVE`(2) /
`IDLE`(3) / `REPLAY_BUFFER_EMPTY`(4) / `PAGE_FAULT_IS_WRITE`(5) /
`STALL_NOT_ACTIVE`(31)

`COMMAND` 值：`ENABLE_PAGING`=0 / `DISABLE_PAGING`=1 / `ENABLE_STALL`=2 /
`DISABLE_STALL`=3 / `ZAP_CACHE`=4 / `PAGE_FAULT_DONE`=5 / `FORCE_RESET`=6

### 页表

- 页大小 **4 KiB**（`SPAGE_SIZE = 4096`）
- 目录：一串 32 位 **DTE**，每项指向一张 PT
- PT：一串 32 位 **PTE**，每项覆盖 4 KiB

**DTE 的地址不是连续的**（这是 v2 和 v1 的区别，也是最容易写错的地方）：

```
31:12  PT 地址 bit 31:0
11: 8  PT 地址 bit 35:32      ← 高 8 位被拆成两段塞在中间
 7: 4  PT 地址 bit 39:36
 3: 1  保留
    0  有效位
```

PTE 的地址位域相同，另加 flags。所以打包/解包各需要一个辅助函数
（mainline 的 `rk_mk_dte_v2` / `rk_dte_pt_address_v2` 就是干这个的，照抄即可）。

## 设计

### 表的来源：**镜像 stage-2**，不要另建一套真值

宿主已经有一份权威的 IPA→PA 映射 —— stage-2 表。NPU MMU 的表应当**跟着它走**：

```
stage2_map_page(ipa → pa)  ──钩子──▶  npu_mmu_fill(ipa, pa)
```

一处改动、两个消费者，不会出现"CPU 看得到、NPU 看不到"的错位。反过来若在提交时
现扫 regcmd 建表，就会有两份真值，迟早不一致。

代价：NPU MMU 的表要为**整个 IPA 空间**预留。192 MiB 的 guest RAM：

```
192 MiB / 4 KiB      = 49152 个 PTE
每个 PT 1024 项 →      48 张 PT × 4 KiB  = 192 KiB
DTE 目录 4096 项 × 4 B                  =  16 KiB
```

一共约 208 KiB，可以接受。DTE 目录要**连续**（`DTE_ADDR` 只存一个基址），
所以得从宿主 PMM 拿物理连续的内存。

### 必须把 MMU 寄存器从透传里摘出来

4 个 MMU 块**现在就落在我们的透传窗口内**：

```
透传窗口：0xFDAB0000 .. 0xFDAE0000
MMU：     0xFDAB9000 / 0xFDABA000 / 0xFDACA000 / 0xFDADA000   （各 0x100）
```

也就是说 **guest 现在够得着它们** —— 只是我们 DTB 里没写 `iommus`、驱动不去碰。
要让宿主独占，`vnpu.c` 得把这几段**从转发里摘掉**（像 vGIC 那样单独处理并拒绝），
否则 guest 重编程一下就绕过了隔离。

### 初始化序列

1. 给 4 个 MMU 块各分配一张 DTE 目录（各核独立还是一张共享，**要查** ——
   vendor DTS 给了 4 个 reg，但 NPU 是 3 核，多出来的那个是什么得确认）
2. 填 DTE / PT
3. 关 `AUTO_GATING`（不然空闲时会省电关掉，翻译随机失效）
4. 写 `DTE_ADDR` → 发 `ENABLE_PAGING`
5. `rk_iommu_read` 回读 `DTE_ADDR` 确认（mainline 里就有这个检查，说明硬件会不认账）
6. 改映射后发 `ZAP_CACHE`（整表失效）或写 `ZAP_ONE_LINE` + IOVA

### 缺页怎么处理

NPU MMU 缺页会置 `PAGE_FAULT_ACTIVE`，`PAGE_FAULT_ADDR` 里是 IOVA。
宿主可以：**当场补表并 `PAGE_FAULT_DONE`** —— 和 CPU 的按需分页是同一个套路，
而 stage-2 那边本来就有现成的"缺页→分配→补映射"的代码可以复用。

## 落地步骤（建议顺序）

1. 把上面那份接口整理成 `include/npu/iommu_v2.h`（纯常量与内联函数，可单元测试）
2. DTE/PTE 的打包解包 —— **可以拿 `tools/fdt_host_test/` 那套办法在宿主上单测**
   （纯字节操作，不需要板子）。这是最值得先做的一步：位域拆装错了在硬件上
   表现为"翻译到错误的物理页"，和这个 bug 一开始的症状一模一样。
3. MMU 驱动本体（建表、命令序列、TLB）
4. `stage2_map_page` 里挂钩子
5. `vnpu.c` 摘掉那 4 个寄存器窗口
6. 真机验证：`regcmd` 扫描那行应当不再报越界，`yolo` 出正确的框

## 和其它两条路的关系

| | 散页 | 隔离 | guest 原样 | 代价 |
|---|---|---|---|---|
| 恒等映射整块 RAM | ✗ | ✗ | ✓ | 钉住 192 MiB |
| virtio NPU | ✓ | ✓ | ✗ | 弃用 rknpu.ko；每次拷权重 |
| **宿主独占 MMU** | ✓ | ✓ | ✓ | 写一个 MMU 驱动 |

**建议先做恒等映射**（3 个常量，当晚可验），先把"NPU 能不能算出正确结果"这件事
单独确认掉，再上 MMU。两个变量缠在一起很难查。
