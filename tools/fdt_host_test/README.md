# fdt_host_test —— 把项目里那份 `fdt.c` 编到宿主上跑

`kernel/vmm/fdt.c` 是纯字节操作（入参就是宿主缓冲，不碰 `vm_t`、不碰 stage-2），
所以可以**原样**编到宿主上、拿真实 DTB 当输入跑 —— 不需要板子、不需要 QEMU。

存在的理由：`guest_loader_dtb_node_has_prop()`（那个"DTB 里有没有 `no-map`
/ `dma-coherent`"的启动自检）**我写坏过两次，两次都是靠这里的最小复现抓出来的**：

| 版本 | 判据 | 实际行为 |
|---|---|---|
| v1 | 查 strings 块里有没有属性名 | `dma-coherent` 在 QEMU 的 `fw-cfg`/`virtio_mmio`/`pcie` 上本来就有 → **永远通过**（假阳性） |
| v2 | 遍历结构块，但 depth 差一层 + 内外层抢着消费 token | 对**所有** DTB 返回 0（假阴性）—— 一个永远说"没有"的检查，会让人去改本来正确的东西 |
| v3 | 任意深度匹配节点名（当前） | ✓ |

那个自检的意义见 `docs/vmm/AARCH64_GUEST_NPU.md`：`no-map` 和 `dma-coherent`
**缺任何一个**，guest 里 NPU 都会以一种完全不像 DTB 问题的方式失败
（前者作业永远超时、后者一跑就 Oops），真机上各耗掉一轮。

## 怎么跑

```bash
# 1) 编译（⚠️ -I 顺序有讲究，见下）
gcc -c -o /tmp/fdt_host.o fdt_host.c \
  -I$(pwd)/fdtshim \
  -I../../kernel/vmm -I../../include -I../../include/aarch64 \
  -I../../include/libc -DARCH_AARCH64=1 -DLOG_LEVEL=0

# 2) 跑：查某个 DTB 的某节点有没有某属性
gcc -o /tmp/fdtcheck fdt_check.c /tmp/fdt_host.o
/tmp/fdtcheck ../../imgs/guests/aarch64/linux-gicv3-npu.dtb
#   → no-map=1 dma-coherent=1
```

`fdt_main.c` 是另一个探针：把四个补丁函数按 `guest_loader.c` 的顺序跑一遍，
用来验证**补丁会不会弄丢属性**（查过：不会，`no-map` 原地不动）。
`neg.c` 是反向用例（不存在的节点 / 点错属性 / 非 FDT）。

## ⚠️ 两个编译上的坑（都踩过）

1. **`-I` 顺序**：`fdtshim` 必须排在项目 `include/` **前面**。项目的
   `include/stdio.h` 是个只声明不实现的 stub，会把系统头挡住 —— 不排前面的话
   `FILE` / `fopen` 全是未知类型。
2. **`fdtshim/string.h` 里不能用 `#include_next`** —— 那会命中项目自己的
   `include/string.h`（它会把 aarch64 的 NEON 内联汇编拉进来，宿主上编不过）。
   得写**绝对路径** `#include </usr/include/string.h>`。

## 这套东西的边界

它验的是 **FDT 解析逻辑**，不是 DTB 内容本身对不对。比如"`no-map` 该不该加"
这种语义问题它答不了 —— 那要靠 QEMU 里的
`tools/guest_npu_qemu_test.sh`（能真起 guest 看驱动 probe 结果）。
两者互补：这个快、这个能精确到函数；那个真。
