# driver — C API

[← 返回总览](index.md) · 486 个函数 · 说明覆盖率 94.2%

<a id="mod-driver-blk-block.c"></a>

## driver/blk/block.c — 根块设备注册

`driver/blk/block.c` · 5 个函数 · 5 个有说明 · 49 行

ramblk/sdblk 在 probe 时把自己的 ext4_blockdev 注册到这里；fs_init()
只问 block_root_device()，不再按平台/驱动 #if 选择。

SD 卡需要先扫描 MBR 取第 2 分区，ramblk 是直接可挂载的 ext4 镜像，
这个差异用 block_root_needs_mbr() 表达。

<a id="fn-driver-blk-block.c-22"></a>

### `block_set_root`

```c
static int block_set_root(struct ext4_blockdev *bdev, bool needs_mbr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

保存根块设备指针与是否需要 MBR 解析的标志，bdev 为空时返回 -1，成功返回 0。

**位置**：`driver/blk/block.c:22`

<a id="fn-driver-blk-block.c-31"></a>

### `block_register_root`

```c
int block_register_root(struct ext4_blockdev *bdev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 needs_mbr=false 调用 block_set_root 注册根块设备并返回其结果。

**位置**：`driver/blk/block.c:31`

<a id="fn-driver-blk-block.c-36"></a>

### `block_register_root_mbr`

```c
int block_register_root_mbr(struct ext4_blockdev *bdev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 needs_mbr=true 注册根块设备，表示后续需解析 MBR 分区表。

**位置**：`driver/blk/block.c:36`

<a id="fn-driver-blk-block.c-41"></a>

### `block_root_device`

```c
struct ext4_blockdev *block_root_device(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回已注册的根块设备指针，未注册时为 NULL。

**位置**：`driver/blk/block.c:41`

<a id="fn-driver-blk-block.c-46"></a>

### `block_root_needs_mbr`

```c
bool block_root_needs_mbr(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回根块设备是否需要 MBR 解析的全局标志。

**位置**：`driver/blk/block.c:46`

<a id="mod-driver-blk-ramblk.c"></a>

## driver/blk/ramblk.c — 基于物理内存区域的 RAM 块设备

`driver/blk/ramblk.c` · 7 个函数 · 7 个有说明 · 95 行

将固定的物理内存区域(由 QEMU -device loader 预加载 ext4 镜像)
封装为 lwext4 的 ext4_blockdev 接口。

编译要求：使用 LWEXT4_CFLAGS(包含 fs/lwext4/include 和 lwext4_port/libc_shim)。

<a id="fn-driver-blk-ramblk.c-27"></a>

### `ramblk_open`

```c
static int ramblk_open(struct ext4_blockdev *bdev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

内存映射设备无需打开操作，忽略参数直接返回 EOK。

<details>
<summary>函数内备注（1 条）</summary>

**第 30 行**

> 设备是内存映射的, 无需实际打开操作

</details>

**位置**：`driver/blk/ramblk.c:27`

<a id="fn-driver-blk-ramblk.c-34"></a>

### `ramblk_bread`

```c
static int ramblk_bread(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id, uint32_t blk_cnt)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

由固定物理基址换算虚拟地址后，把 blk_cnt 个块 memcpy 到 buf 并返回 EOK。

**位置**：`driver/blk/ramblk.c:34`

<a id="fn-driver-blk-ramblk.c-42"></a>

### `ramblk_bwrite`

```c
static int ramblk_bwrite(struct ext4_blockdev *bdev, const void *buf, uint64_t blk_id, uint32_t blk_cnt)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 buf 中 blk_cnt 个块 memcpy 写入固定物理基址对应的内存并返回 EOK。

**位置**：`driver/blk/ramblk.c:42`

<a id="fn-driver-blk-ramblk.c-50"></a>

### `ramblk_close`

```c
static int ramblk_close(struct ext4_blockdev *bdev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空实现，忽略参数直接返回 EOK。

**位置**：`driver/blk/ramblk.c:50`

<a id="fn-driver-blk-ramblk.c-65"></a>

### `ramblk_get_bdev`

```c
struct ext4_blockdev *ramblk_get_bdev(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回内部静态 ramblk 块设备结构的指针。

**位置**：`driver/blk/ramblk.c:65`

<a id="fn-driver-blk-ramblk.c-70"></a>

### `ramblk_init`

```c
void ramblk_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 RAMBLK_SIZE 计算并设置块设备块数与分区大小，并打印基址、容量与扇区信息。

<details>
<summary>函数内备注（2 条）</summary>

**第 72 行**

> 从运行时全局变量设置扇区数和分区大小(platform_conf_scan() 已填充)

**第 77 行**

> 四行合并成一行 —— 它回答的是&quot;rootfs 在哪&quot;, 一句话就够

</details>

**位置**：`driver/blk/ramblk.c:70`

<a id="fn-driver-blk-ramblk.c-82"></a>

### `ramblk_probe`

```c
static int ramblk_probe(const device_t *dev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

调用 ramblk_init 初始化，再把 ramblk 注册为根块设备并返回注册结果。

<details>
<summary>函数内备注（1 条）</summary>

**第 84 行（行尾）**

> 阶段 1：ramblk 使用运行时 g_mem_rootfs_*

</details>

**位置**：`driver/blk/ramblk.c:82`

<a id="mod-driver-blk-sdblk.c"></a>

## driver/blk/sdblk.c — SG2002 / CV1811 SDMMC (SDHCI) 块设备驱动

`driver/blk/sdblk.c` · 29 个函数 · 29 个有说明 · 606 行

设计：
- 纯 PIO 轮询模式(无 DMA / 无中断)
- 目标平台：SOPHGO SG2002 / CV1811H, SDIO0 控制器
- 与 SDHCI 规范基本兼容

平台硬件地址由 platforms/<platform>/platform.conf 的 sdmmc 表提供,
在 sdblk_init() 中通过 platform_get_mmio() 运行时读取。

<a id="fn-driver-blk-sdblk.c-120"></a>

### `sd_rd`

```c
static inline uint32_t sd_rd(uint32_t off)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 SD 控制器基址加偏移处读取 32 位寄存器值。

**位置**：`driver/blk/sdblk.c:120`

<a id="fn-driver-blk-sdblk.c-125"></a>

### `sd_wr`

```c
static inline void sd_wr(uint32_t off, uint32_t v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

向 SD 控制器基址加偏移处写入 32 位寄存器值。

**位置**：`driver/blk/sdblk.c:125`

<a id="fn-driver-blk-sdblk.c-130"></a>

### `sd_set`

```c
static inline void sd_set(uint32_t off, uint32_t mask)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读改写：把 SD 控制器指定偏移寄存器的 mask 位置 1。

**位置**：`driver/blk/sdblk.c:130`

<a id="fn-driver-blk-sdblk.c-135"></a>

### `sd_clr`

```c
static inline void sd_clr(uint32_t off, uint32_t mask)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读改写：把 SD 控制器指定偏移寄存器的 mask 位清 0。

**位置**：`driver/blk/sdblk.c:135`

<a id="fn-driver-blk-sdblk.c-140"></a>

### `sd_mod`

```c
static inline void sd_mod(uint32_t off, uint32_t mask, uint32_t val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读改写：用 val 的 mask 位替换 SD 控制器寄存器对应位。

**位置**：`driver/blk/sdblk.c:140`

<a id="fn-driver-blk-sdblk.c-145"></a>

### `top_wr`

```c
static inline void top_wr(uint32_t off, uint32_t v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

向 top_base 加偏移处写入 32 位寄存器值。

**位置**：`driver/blk/sdblk.c:145`

<a id="fn-driver-blk-sdblk.c-151"></a>

### `sd_rd_pio`

```c
static inline uint32_t sd_rd_pio(uint32_t off)
```

快速 PIO 读写(热路径, 不需要额外 dsb)

**位置**：`driver/blk/sdblk.c:151`

<a id="fn-driver-blk-sdblk.c-156"></a>

### `sd_wr_pio`

```c
static inline void sd_wr_pio(uint32_t off, uint32_t v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用非严格有序的写向 SD 基址偏移处写 32 位值（PIO 数据口）。

**位置**：`driver/blk/sdblk.c:156`

<a id="fn-driver-blk-sdblk.c-162"></a>

### `sd_delay`

```c
static void sd_delay(volatile uint32_t n)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

调用 timer_spin 忙等 n 个计数周期。

**位置**：`driver/blk/sdblk.c:162`

<a id="fn-driver-blk-sdblk.c-175"></a>

### `sd_power`

```c
static void sd_power(uint32_t volt_bits)
```

════════════════════════════════════════════════════════════════
内部函数
════════════════════════════════════════════════════════════════

**位置**：`driver/blk/sdblk.c:175`

<a id="fn-driver-blk-sdblk.c-187"></a>

### `sd_set_clock`

```c
static void sd_set_clock(uint8_t divider)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先关 SD 时钟并设分频，开内部时钟等其稳定后再开 SD 时钟并延时。

**位置**：`driver/blk/sdblk.c:187`

<a id="fn-driver-blk-sdblk.c-199"></a>

### `sd_reset_config`

```c
static void sd_reset_config(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

断电并清各软复位位后延时，重新上电 3V3 并置 4 位数据总线。

**位置**：`driver/blk/sdblk.c:199`

<a id="fn-driver-blk-sdblk.c-208"></a>

### `sd_wait_cmd_done`

```c
static int sd_wait_cmd_done(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

轮询中断状态最多百万次等命令完成，出错或超时清全部中断并返回 SDBLK_ERR。

**位置**：`driver/blk/sdblk.c:208`

<a id="fn-driver-blk-sdblk.c-227"></a>

### `sd_wait_xfer_done`

```c
static int sd_wait_xfer_done(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

轮询最多五百万次等传输完成，出错或超时清中断并打日志后返回错误。

**位置**：`driver/blk/sdblk.c:227`

<a id="fn-driver-blk-sdblk.c-247"></a>

### `sd_send_cmd`

```c
static int sd_send_cmd(uint32_t cmdw, uint32_t arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

等待命令/数据禁止位清零后设超时、清中断、写参数与命令字，再等命令完成。

<details>
<summary>函数内备注（1 条）</summary>

**第 250 行**

> spin

</details>

**位置**：`driver/blk/sdblk.c:247`

<a id="fn-driver-blk-sdblk.c-263"></a>

### `sd_pio_read_blocks`

```c
static int sd_pio_read_blocks(uint8_t *buf, size_t count)
```

sd_pio_read_blocks — PIO 读取 N 个 512 字节块
每块等待 BUF_RRDY, 然后从 R_BUF_DATA 读取 128 个 32 位字。

**位置**：`driver/blk/sdblk.c:263`

<a id="fn-driver-blk-sdblk.c-295"></a>

### `sd_pio_write_blocks`

```c
static int sd_pio_write_blocks(const uint8_t *buf, size_t count)
```

sd_pio_write_blocks — PIO 写入 N 个 512 字节块
每块等待 BUF_WRDY, 然后向 R_BUF_DATA 写入 128 个 32 位字。

**位置**：`driver/blk/sdblk.c:295`

<a id="fn-driver-blk-sdblk.c-329"></a>

### `sd_parse_csd`

```c
static uint64_t sd_parse_csd(uint8_t *out_struct, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3)
```

sd_parse_csd — 从 R2 响应寄存器解析 SD 卡容量

SDHCI 将 R2 响应的 [135:8] 放入 RESP[127:0]：
RESP_REG[i] = CSD[i+8](CSD[7:0] CRC 已由硬件丢弃)
r3=RESP3=CSD[127:96], r2=RESP2=CSD[95:64],
r1=RESP1=CSD[63:32],  r0=RESP0=CSD[39:8]

<details>
<summary>函数内备注（3 条）</summary>

**第 338 行**

> CSD v1.0 (SDSC)

**第 349 行**

> CSD v2.0 (SDHC/SDXC)：C_SIZE = response1[29:8], 22-bit

**第 355 行**

> CSD v3.0 (SDUC)：C_SIZE 28-bit, 跨 response2/response1

</details>

**位置**：`driver/blk/sdblk.c:329`

<a id="fn-driver-blk-sdblk.c-372"></a>

### `sdblk_init`

```c
int sdblk_init(void)
```

════════════════════════════════════════════════════════════════
公开 API
════════════════════════════════════════════════════════════════

<details>
<summary>函数内备注（12 条）</summary>

**第 375 行**

> 从 platform.conf 的 platform.sdmmc.* 读取硬件地址

**第 392 行**

> 复位配置(与 Rust BSP 一致：关电 → 清复位位 → 开 3.3V → 4bit)

**第 395 行**

> 切换到 1.8V(U-Boot 已将物理电压轨切到 1.8V)

**第 398 行**

> 时钟 divider=4(与 Rust BSP 一致)

**第 401 行**

> CMD0: GO_IDLE_STATE

**第 404 行**

> CMD8: SEND_IF_COND

**第 407 行**

> ACMD41 循环：等待卡初始化完成

**第 417 行**

> CMD2: ALL_SEND_CID

**第 423 行**

> CMD3: SEND_RELATIVE_ADDR(R6 → Response48Busy 与 Rust BSP 一致)

**第 431 行**

> CMD9: SEND_CSD

**第 444 行**

> CMD7: SELECT_CARD

**第 450 行**

> ACMD6: SET_BUS_WIDTH = 4 bit

</details>

**位置**：`driver/blk/sdblk.c:372`

<a id="fn-driver-blk-sdblk.c-464"></a>

### `sdblk_read_blocks`

```c
int sdblk_read_blocks(uint32_t block_id, void *buf, size_t count)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按块数选 CMD17 或 CMD18（多块+AutoCMD12）发起读，再做 PIO 读取并等传输完成。

<details>
<summary>函数内备注（2 条）</summary>

**第 471 行**

> CMD17: READ_SINGLE_BLOCK

**第 476 行**

> CMD18: READ_MULTIPLE_BLOCK + AutoCMD12

</details>

**位置**：`driver/blk/sdblk.c:464`

<a id="fn-driver-blk-sdblk.c-494"></a>

### `sdblk_write_blocks`

```c
int sdblk_write_blocks(uint32_t block_id, const void *buf, size_t count)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按块数选 CMD24 或 CMD25 发起写，再做 PIO 写入并等传输完成。

<details>
<summary>函数内备注（2 条）</summary>

**第 501 行**

> CMD24: WRITE_BLOCK

**第 506 行**

> CMD25: WRITE_MULTIPLE_BLOCK + AutoCMD12

</details>

**位置**：`driver/blk/sdblk.c:494`

<a id="fn-driver-blk-sdblk.c-523"></a>

### `sdblk_capacity_bytes`

```c
uint64_t sdblk_capacity_bytes(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

设备已初始化时返回卡的字节容量，否则返回 0。

**位置**：`driver/blk/sdblk.c:523`

<a id="fn-driver-blk-sdblk.c-528"></a>

### `sdblk_capacity_blocks`

```c
uint64_t sdblk_capacity_blocks(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

设备已初始化时返回容量除以块大小的块数，否则返回 0。

**位置**：`driver/blk/sdblk.c:528`

<a id="fn-driver-blk-sdblk.c-540"></a>

### `sdblk_bdev_open`

```c
static int sdblk_bdev_open(struct ext4_blockdev *bdev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

未初始化时先初始化卡，回填块数，并仅在 part_size 为空或越界时用整盘容量填充。

<details>
<summary>函数内备注（2 条）</summary>

**第 542 行**

> 正常路径下 STORAGE probe 已经初始化过卡；这里只在直接调用 bdev
> 的场景（测试/旧路径）补一次，避免重复 CMD0/ACMD41 把已挂载的卡复位。

**第 552 行**

> 分区 bdev 的 part_size 是 MBR 扫描时设好的，不能被整盘容量覆盖 ——
> FatFs 的 GET_SECTOR_COUNT 和 lwext4 的边界检查都靠它。只有 parent
> （part_size 还是 0）或明显越界的值才回填整盘容量。

</details>

**位置**：`driver/blk/sdblk.c:540`

<a id="fn-driver-blk-sdblk.c-563"></a>

### `sdblk_bdev_bread`

```c
static int sdblk_bdev_bread(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id, uint32_t blk_cnt)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

调用 sdblk_read_blocks 读块，成功返回 EOK，失败返回 EIO。

**位置**：`driver/blk/sdblk.c:563`

<a id="fn-driver-blk-sdblk.c-569"></a>

### `sdblk_bdev_bwrite`

```c
static int sdblk_bdev_bwrite(struct ext4_blockdev *bdev, const void *buf, uint64_t blk_id, uint32_t blk_cnt)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

调用 sdblk_write_blocks 写块，成功返回 EOK，失败返回 EIO。

**位置**：`driver/blk/sdblk.c:569`

<a id="fn-driver-blk-sdblk.c-575"></a>

### `sdblk_bdev_close`

```c
static int sdblk_bdev_close(struct ext4_blockdev *bdev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空实现，忽略参数直接返回 EOK。

**位置**：`driver/blk/sdblk.c:575`

<a id="fn-driver-blk-sdblk.c-587"></a>

### `sdblk_get_bdev`

```c
struct ext4_blockdev *sdblk_get_bdev(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回内部静态 sdblk 块设备结构的指针。

**位置**：`driver/blk/sdblk.c:587`

<a id="fn-driver-blk-sdblk.c-592"></a>

### `sdblk_probe`

```c
static int sdblk_probe(const device_t *dev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

初始化 SD 卡，失败返回 -1，成功则把该设备注册为需 MBR 解析的根块设备。

<details>
<summary>函数内备注（1 条）</summary>

**第 594 行（行尾）**

> 阶段 1：sdblk_init() 仍直接读平台配置

</details>

**位置**：`driver/blk/sdblk.c:592`

<a id="mod-driver-core-device.c"></a>

## driver/core/device.c — 平台设备表访问

`driver/core/device.c` · 2 个函数 · 2 个有说明 · 35 行

g_platform_devices[] 由 tools/gen_platform.py 生成到
build/<platform>/platform_static.c。这里只做按 class 查找和 MMIO 地址换算。

<a id="fn-driver-core-device.c-18"></a>

### `driver_device`

```c
const device_t *driver_device(drv_class_t cls)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在平台设备表中线性查找第一个类为 cls 的设备并返回其地址，cls 越界或无匹配时返回 NULL。

**位置**：`driver/core/device.c:18`

<a id="fn-driver-core-device.c-30"></a>

### `device_mmio`

```c
uintptr_t device_mmio(uintptr_t raw_pa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

物理地址为 0 时返回 0，否则按内核是否需要重映射决定是否加上 KERNEL_VMA 偏移后返回。

**位置**：`driver/core/device.c:30`

<a id="mod-driver-core-driver.c"></a>

## driver/core/driver.c — 驱动注册表与初始化阶段

`driver/core/driver.c` · 6 个函数 · 6 个有说明 · 139 行

驱动通过 DRIVER_ENTRY() 放进 .drivers 段，链接脚本把它收在 .rodata 内，
于是这里可以直接用 __start_drivers/__stop_drivers 遍历。

一次构建每个 class 只有一个后端，所以绑定只按 class；driver 名字仅用于
日志/诊断。命令行覆盖（make UART=dw 等）只改变被编译的后端，不会和
platform.conf 里的设备表失配。

<a id="fn-driver-core-driver.c-28"></a>

### `driver_is_service`

```c
static bool driver_is_service(drv_class_t cls)
```

没有设备、但需要初始化的服务

**位置**：`driver/core/driver.c:28`

<a id="fn-driver-core-driver.c-34"></a>

### `driver_is_required`

```c
static bool driver_is_required(drv_class_t cls)
```

有设备就必须有驱动的 class

**位置**：`driver/core/driver.c:34`

<a id="fn-driver-core-driver.c-50"></a>

### `driver_of_class`

```c
static const driver_t *driver_of_class(drv_class_t cls)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

遍历链接器圈定的驱动段，返回第一个类匹配的驱动描述符，未找到时返回 NULL。

**位置**：`driver/core/driver.c:50`

<a id="fn-driver-core-driver.c-59"></a>

### `driver_required_panic`

```c
static void driver_required_panic(const driver_t *d, const char *what, int rc)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 rc 是否非零打印带错误码的失败日志，随后调用 platform_panic() 停机。

**位置**：`driver/core/driver.c:59`

<a id="fn-driver-core-driver.c-68"></a>

### `drivers_init`

```c
void drivers_init(drv_phase_t phase)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按阶段初始化：EARLY 阶段告警无驱动设备，必需类缺驱动则 panic，随后先 probe 后 start 并记录绑定表。

<details>
<summary>函数内备注（4 条）</summary>

**第 73 行**

> 第一次进入（EARLY）时把&quot;平台声明了设备、但没有驱动&quot;的情况喊出来。
> 必需类在下面按阶段 panic；可选类只告警（例如 ETH=none 但设备表
> 仍然写了 virtio 时，能一眼看到配置不一致）。

**第 86 行**

> 必需设备：在它的 probe 阶段检查驱动是否存在

**第 99 行**

> 第一遍：probe

**第 122 行**

> 第二遍：start（保证同阶段内先 probe 后 start）

</details>

**位置**：`driver/core/driver.c:68`

<a id="fn-driver-core-driver.c-136"></a>

### `driver_present`

```c
bool driver_present(drv_class_t cls)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

检查类号合法且该类的驱动已绑定，返回布尔结果。

**位置**：`driver/core/driver.c:136`

<a id="mod-driver-dma-dma_buf.c"></a>

## driver/dma/dma_buf.c — dma-buf 缓冲区对象

`driver/dma/dma_buf.c` · 19 个函数 · 19 个有说明 · 355 行

一个 dma_buf 就是「一段物理连续内存 + 一个引用计数」, 用户态通过 fd 持有它。
内存来源全部交给 dma_heap.c(见那里的说明), 本文件不碰 PMM。

本文件不接触用户指针 —— ABI 层在 kernel/fs/pseudofs/dev.c,
那边负责 copy_from/to_user_bytes。这样 driver/ 不需要依赖 syscall 层。

<a id="fn-driver-dma-dma_buf.c-50"></a>

### `dma_buf_impl_from_index`

```c
dma_buf_t *dma_buf_impl_from_index(uint32_t idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

下标越界或表项未使用时返回 NULL，否则返回对应的 dma_buf。

**位置**：`driver/dma/dma_buf.c:50`

<a id="fn-driver-dma-dma_buf.c-58"></a>

### `dma_buf_impl_index`

```c
uint32_t dma_buf_impl_index(const dma_buf_t *buf)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空指针返回哨兵值 DMA_BUF_MAX，否则返回指针在全局表中的下标。

<details>
<summary>函数内备注（1 条）</summary>

**第 61 行（行尾）**

> 哨兵：越界值, from_index 会拒

</details>

**位置**：`driver/dma/dma_buf.c:58`

<a id="fn-driver-dma-dma_buf.c-67"></a>

### `dma_buf_impl_alloc`

```c
dma_buf_t *dma_buf_impl_alloc(size_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 dma_heap 取连续内存，持锁占用空闲表项并置引用计数 1、记录属主 pid；表满则释放内存返回 NULL。

**位置**：`driver/dma/dma_buf.c:67`

<a id="fn-driver-dma-dma_buf.c-109"></a>

### `dma_buf_impl_get`

```c
void dma_buf_impl_get(dma_buf_t *buf)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

持锁对仍在使用中的 buffer 增加一次引用计数，空指针直接返回。

**位置**：`driver/dma/dma_buf.c:109`

<a id="fn-driver-dma-dma_buf.c-119"></a>

### `dma_buf_impl_put`

```c
void dma_buf_impl_put(dma_buf_t *buf)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

持锁减引用，最后一个引用时清零表项并在锁外释放物理内存；重复 put 只打错误日志。

<details>
<summary>函数内备注（1 条）</summary>

**第 129 行**

> 重复 put：表项已被释放。当错误吞掉比跟着崩好 —— 这不是内存安全问题,
> 只是引用计数纪律被破坏, 日志足够定位。

</details>

**位置**：`driver/dma/dma_buf.c:119`

<a id="fn-driver-dma-dma_buf.c-159"></a>

### `dma_buf_impl_find_by_phys`

```c
dma_buf_t *dma_buf_impl_find_by_phys(uint64_t pa, uint64_t *off)
```

按物理地址反查缓冲区 —— 排障专用。

用途：用户态(librknnrt)在任务描述符里填的是设备地址, 我们只拿到
那个数字, 没法直接读它指向的内容。但它通常指向我们自己发出去的某个
dma-buf；查出来就能把内容 dump 出来, 否则只能看着一个裸地址猜。

返回命中的缓冲区(不增加引用计数 —— 只在调用方持锁或用完即走的
排障路径上使用), 并把页内偏移写回 *off。

**位置**：`driver/dma/dma_buf.c:159`

<a id="fn-driver-dma-dma_buf.c-179"></a>

### `dma_buf_impl_get_index`

```c
void dma_buf_impl_get_index(uint32_t idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

由下标取出 buffer 后增加其引用计数。

**位置**：`driver/dma/dma_buf.c:179`

<a id="fn-driver-dma-dma_buf.c-184"></a>

### `dma_buf_impl_put_index`

```c
void dma_buf_impl_put_index(uint32_t idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

由下标取出 buffer 后释放其引用计数。

**位置**：`driver/dma/dma_buf.c:184`

<a id="fn-driver-dma-dma_buf.c-191"></a>

### `dma_buf_impl_dev_addr`

```c
uint64_t dma_buf_impl_dev_addr(const dma_buf_t *buf)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 buffer 的物理地址作为设备地址，空指针返回 0。

<details>
<summary>函数内备注（1 条）</summary>

**第 193 行**

> 全系统唯一的「缓冲区 → 设备地址」出口(见 dma_buf.h 文件头)。
> 现在无 IOMMU, 设备看到的就是物理地址；将来接上 SMMU 之后,
> 这里改成返回 IOVA, 上层一行都不用动。

</details>

**位置**：`driver/dma/dma_buf.c:191`

<a id="fn-driver-dma-dma_buf.c-201"></a>

### `dma_buf_impl_kva`

```c
void *dma_buf_impl_kva(const dma_buf_t *buf)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 buffer 物理地址经 phys_to_virt 映射后的内核虚拟地址，空指针返回 NULL。

**位置**：`driver/dma/dma_buf.c:201`

<a id="fn-driver-dma-dma_buf.c-206"></a>

### `dma_buf_impl_size`

```c
size_t dma_buf_impl_size(const dma_buf_t *buf)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 buffer 的请求大小，空指针返回 0。

**位置**：`driver/dma/dma_buf.c:206`

<a id="fn-driver-dma-dma_buf.c-213"></a>

### `dma_buf_impl_sync_region`

```c
void dma_buf_impl_sync_region(dma_buf_t *buf, uint64_t off, uint64_t len, bool to_device)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验并裁剪范围后，按方向对指定区间做数据 cache 写回或失效。

<details>
<summary>函数内备注（1 条）</summary>

**第 220 行（行尾）**

> 夹一次, 调用方算错也不越界

</details>

**位置**：`driver/dma/dma_buf.c:213`

<a id="fn-driver-dma-dma_buf.c-229"></a>

### `dma_buf_impl_apply_sync`

```c
int dma_buf_impl_apply_sync(uint32_t idx, uint64_t flags)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 dma_buf_sync 语义：START|READ 时失效整块，END|WRITE 时写回整块，其余组合为空操作。

<details>
<summary>函数内备注（3 条）</summary>

**第 235 行**

> 语义照 Linux 的 dma_buf_sync：

> START|READ   CPU 要读设备写的数据 → invalidate(丢掉 CPU 里的旧行)
> END|WRITE    CPU 写完了要给设备看 → clean(写回内存)

> 另外两种组合(START|WRITE / END|READ)是空操作, 这是对的：
> - CPU 要写, 不需要先把设备的数据拉进来(整块会被覆盖)；
> - CPU 读完了, 不需要回写(就算有脏行, 它们本来就该回写,
> 但那由「CPU 写过」那条路径负责)。

> 注意： 把这里实现成空操作 = 静默数据损坏：模型输出会随机出错,
> 而且复现不了。这是本阶段最不能省的一行。

**第 253 行（行尾）**

> invalidate

**第 255 行（行尾）**

> clean

</details>

**位置**：`driver/dma/dma_buf.c:229`

<a id="fn-driver-dma-dma_buf.c-264"></a>

### `dma_buf_fd_get_phys`

```c
static int dma_buf_fd_get_phys(void *priv, uint64_t *paddr, uint64_t *size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

由私有下标查出 buffer 并输出设备地址与大小，查不到返回 -1。

**位置**：`driver/dma/dma_buf.c:264`

<a id="fn-driver-dma-dma_buf.c-274"></a>

### `dma_buf_fd_dup_cb`

```c
static int dma_buf_fd_dup_cb(void *priv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

对 fd 私有下标对应的 buffer 增加一次引用计数并返回 0。

**位置**：`driver/dma/dma_buf.c:274`

<a id="fn-driver-dma-dma_buf.c-280"></a>

### `dma_buf_fd_close_cb`

```c
static void dma_buf_fd_close_cb(void *priv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

对 fd 私有下标对应的 buffer 释放一次引用计数。

**位置**：`driver/dma/dma_buf.c:280`

<a id="fn-driver-dma-dma_buf.c-294"></a>

### `dma_buf_impl_alloc_fd_current`

```c
int dma_buf_impl_alloc_fd_current(dma_buf_t *buf)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

为当前任务创建携带 buffer 下标的 dmabuf fd，并为 fd 增加一份引用，失败返回 -1。

<details>
<summary>函数内备注（1 条）</summary>

**第 307 行**

> fd 自己持有一份引用；fd 关闭时(fd_obj_close)还回来。

</details>

**位置**：`driver/dma/dma_buf.c:294`

<a id="fn-driver-dma-dma_buf.c-312"></a>

### `dma_buf_impl_from_user_fd`

```c
dma_buf_t *dma_buf_impl_from_user_fd(int fd)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

由当前任务的 fd 查出 dmabuf 下标并返回对应 buffer，失败返回 NULL。

**位置**：`driver/dma/dma_buf.c:312`

<a id="fn-driver-dma-dma_buf.c-342"></a>

### `dma_buf_probe`

```c
static int dma_buf_probe(const device_t *dev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 dma_buf 后端操作表绑定到核心层，返回 0。

<details>
<summary>函数内备注（1 条）</summary>

**第 344 行（行尾）**

> service：无设备；分配在首次调用时惰性完成

</details>

**位置**：`driver/dma/dma_buf.c:342`

<a id="mod-driver-dma-dma_core.c"></a>

## driver/dma/dma_core.c — dma-buf 服务注册点

`driver/dma/dma_core.c` · 1 个函数 · 1 个有说明 · 21 行

永远参与构建。DMA=1 时 driver/dma/dma_buf.c 的 probe 调 dma_buf_bind()；
否则 g_dma_buf_ops 保持 NULL，include/driver/dma_buf.h 的内联包装统一返回失败。

<a id="fn-driver-dma-dma_core.c-18"></a>

### `dma_buf_bind`

```c
void dma_buf_bind(const dma_buf_ops_t *ops)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

保存传入的 dma_buf 操作表指针供核心层调用。

**位置**：`driver/dma/dma_core.c:18`

<a id="mod-driver-dma-dma_heap.c"></a>

## driver/dma/dma_heap.c — 堆后端实现(P0：全局 PMM)

`driver/dma/dma_heap.c` · 2 个函数 · 2 个有说明 · 66 行

见 dma_heap.h 里为什么这一层只有两个函数。

<a id="fn-driver-dma-dma_heap.c-22"></a>

### `dma_heap_alloc`

```c
int dma_heap_alloc(size_t size, uint64_t *paddr_out, void **kva_out, size_t *size_out)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

参数与 PMM 校验后按页向上取整分配物理连续内存并清零，输出物理地址、虚拟地址与实际字节数。

<details>
<summary>函数内备注（2 条）</summary>

**第 35 行**

> pmm_alloc_pages 返回的是物理连续的一段 —— 这正是无 IOMMU 的前提,
> 也是这里不能用 kalloc_pages 之外任何&quot;逐页拼&quot;分配器的原因。

**第 46 行**

> DMA coherent 语义：设备看到的是干净的零, 不会把上一次的残留读出去

</details>

**位置**：`driver/dma/dma_heap.c:22`

<a id="fn-driver-dma-dma_heap.c-57"></a>

### `dma_heap_free`

```c
void dma_heap_free(uint64_t paddr, size_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验地址、大小与 PMM 初始化状态后，按页数把物理内存归还 PMM。

**位置**：`driver/dma/dma_heap.c:57`

<a id="mod-driver-eth-cvitek_eth.c"></a>

## driver/eth/cvitek_eth.c — SG2002 / CV1812H 板载以太网驱动(C 实现)

`driver/eth/cvitek_eth.c` · 32 个函数 · 32 个有说明 · 945 行

硬件：Synopsys DesignWare MAC 3.70a(DWMAC)+ 内部 EPHY
GMAC 基址来自 platform.conf 的 eth.base, sg2002-riscv64 上是 0x04070000。

本文件是原 Rust 驱动(driver/cvitek-eth + rust/avatar_eth FFI 桥)的 C 重写,
去掉了 FFI 层, 直接实现 netdev_t。初始化顺序, 寄存器取值, PHY 自协商流程
与 Rust 原版逐条对应。

── 与 Rust 原版的三个关键差异(直译会翻车的地方)────────────────────────

1. 描述符地址。Rust 写的是 `buf.as_ptr() as u32` —— 它在 riscv64 上"碰巧"对,
因为 VA = PA + KERNEL_VMA 且 KERNEL_VMA = 0xffffffc000000000, 截断到 32 位
正好把整个偏移丢掉。这里必须显式走 virt_to_phys()。

2. 描述符 stride。DMA_BUS_MODE.DSL = 12 words(跳 48 字节)+ 描述符本身 16 字节
= 64 字节步长, 与 cvitek_dma_desc_t 的 64 字节对齐是一对, 见下面的
static_assert。同时 64 字节正好是 C906 的 cache line, 一条描述符独占一行。

3. 缓存维护。Rust 有, 而 virtio_net.c 没有 —— 在 C906 这种非相干 cache 的核上,
缺了它就是收发出错帧。收发路径里的 clean/invalidate 都有注释标明方向。

── 中断模型 ────────────────────────────────────────────────────────────
收包走中断(PLIC 源 31, level-high), 但中断里只做去断言 + 唤醒：
cvitek_eth_isr() 读回 DMA_STATUS 把置位原值写回(W1C, level-high 必须
真的把源清掉, 否则中断风暴), 然后 netdev_rx_wakeup() 叫醒 net-poll 任务。
所有包处理(pbuf, lwIP 输入)都在任务上下文 —— ISR 里做那些会把中断
延迟拉爆, 而且 lwIP 不是中断安全的。

发送仍然是"写描述符 + 踢 TX_POLL", 完成回收靠下次 send 时惰性做, 不开 TX 中断。

── 并发假设 ────────────────────────────────────────────────────────────
描述符环/帧缓冲只被 net-poll 任务访问, 不加锁(与 virtio_net.c 同假设)；
ISR 只碰 DMA_STATUS 寄存器, 不碰环。SG2002 的 RISC-V SMP bringup 本来就是
关的(kernel/task/cpu.c), g_num_cpus == 1。

<a id="fn-driver-eth-cvitek_eth.c-213"></a>

### `cvitek_read`

```c
static inline uint32_t cvitek_read(const struct cvitek_eth_nic *nic, uint32_t off)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取网卡寄存器基址加偏移处的 32 位值。

**位置**：`driver/eth/cvitek_eth.c:213`

<a id="fn-driver-eth-cvitek_eth.c-218"></a>

### `cvitek_write`

```c
static inline void cvitek_write(const struct cvitek_eth_nic *nic, uint32_t off, uint32_t val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

向网卡寄存器基址加偏移处写入 32 位值。

**位置**：`driver/eth/cvitek_eth.c:218`

<a id="fn-driver-eth-cvitek_eth.c-230"></a>

### `cvitek_mmio_va`

```c
static inline uintptr_t cvitek_mmio_va(uintptr_t pa)
```

硬编码的 SoC 物理地址 → 内核可访问的虚拟地址。
sg2002-riscv64 的 platform.conf 里 mmio_vma = true, MMIO 要走 KERNEL_VMA
高半别名；这个规则由 g_mmio_needs_vma 给出, 和 platform_get_mmio() 一致。

**位置**：`driver/eth/cvitek_eth.c:230`

<a id="fn-driver-eth-cvitek_eth.c-240"></a>

### `cvitek_dma_pa`

```c
static inline uint32_t cvitek_dma_pa(const void *va)
```

内核虚拟地址 → DMA 用的物理地址。
DWMAC 3.70a 只有 32 位 DMA 地址；SG2002 的 DRAM 在 0x80000000..0x8FFFFFFF,
装得下。init 里对每个缓冲都做了上界检查, 这里只做转换。

**位置**：`driver/eth/cvitek_eth.c:240`

<a id="fn-driver-eth-cvitek_eth.c-245"></a>

### `cvitek_buf`

```c
static inline uint8_t *cvitek_buf(uint8_t *pool, uint32_t idx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

由缓冲池首地址与索引计算第 idx 个固定大小帧缓冲的指针。

**位置**：`driver/eth/cvitek_eth.c:245`

<a id="fn-driver-eth-cvitek_eth.c-256"></a>

### `cvitek_deadline_us`

```c
static uint64_t cvitek_deadline_us(uint32_t us)
```

基于 timer_read_counter()(RISC-V 的 rdtime CSR), 不依赖定时器中断 ——
这样在关中断的上下文里调用也不会死等。

**位置**：`driver/eth/cvitek_eth.c:256`

<a id="fn-driver-eth-cvitek_eth.c-264"></a>

### `cvitek_expired`

```c
static inline bool cvitek_expired(uint64_t deadline)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用有符号差判断当前计数器是否已达到或越过给定截止值。

**位置**：`driver/eth/cvitek_eth.c:264`

<a id="fn-driver-eth-cvitek_eth.c-271"></a>

### `cvitek_clean_desc`

```c
static inline void cvitek_clean_desc(const cvitek_dma_desc_t *d)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

对单个 DMA 描述符做数据 cache 写回。

**位置**：`driver/eth/cvitek_eth.c:271`

<a id="fn-driver-eth-cvitek_eth.c-276"></a>

### `cvitek_inval_desc`

```c
static inline void cvitek_inval_desc(const cvitek_dma_desc_t *d)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

对单个 DMA 描述符做数据 cache 失效。

**位置**：`driver/eth/cvitek_eth.c:276`

<a id="fn-driver-eth-cvitek_eth.c-283"></a>

### `cvitek_clk_and_reset_enable`

```c
static void cvitek_clk_and_reset_enable(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打开 eth0 时钟门控、解除 MAC 与 EPHY 软复位，并延时等待 EPHY 稳定。

<details>
<summary>函数内备注（4 条）</summary>

**第 289 行**

> 放开 eth0 时钟门控

**第 292 行**

> 解除 ETH0 MAC 软复位(SOFT_RSTN_0 bit12)

**第 295 行**

> 解除 EPHY 相关复位(SOFT_RSTN_3 bit0/1)

**第 298 行**

> 等 EPHY 稳定

</details>

**位置**：`driver/eth/cvitek_eth.c:283`

<a id="fn-driver-eth-cvitek_eth.c-304"></a>

### `cvitek_mdio_wait`

```c
static int cvitek_mdio_wait(const struct cvitek_eth_nic *nic)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

轮询 MII 地址寄存器的 BUSY 位直到清零，超时告警并返回 -1。

**位置**：`driver/eth/cvitek_eth.c:304`

<a id="fn-driver-eth-cvitek_eth.c-317"></a>

### `cvitek_mdio_read`

```c
static int cvitek_mdio_read(const struct cvitek_eth_nic *nic, uint32_t phy, uint32_t reg, uint16_t *out)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

等空闲后一次性拼写 PHY/寄存器/时钟/BUSY 触发读事务，再等待并取出数据寄存器低 16 位。

<details>
<summary>函数内备注（1 条）</summary>

**第 322 行**

> MII_ADDR 是「写进去就生效」的整字寄存器, PHY/REG/CLK_CSR/BUSY 必须一次拼好,
> 不能用读改写 —— 读回的值带 BUSY, 写回去会重复触发事务。

</details>

**位置**：`driver/eth/cvitek_eth.c:317`

<a id="fn-driver-eth-cvitek_eth.c-338"></a>

### `cvitek_mdio_write`

```c
static int cvitek_mdio_write(const struct cvitek_eth_nic *nic, uint32_t phy, uint32_t reg, uint16_t val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先写数据寄存器，再拼写地址寄存器（带写标志与 BUSY）发起写事务，返回等待结果。

**位置**：`driver/eth/cvitek_eth.c:338`

<a id="fn-driver-eth-cvitek_eth.c-355"></a>

### `cvitek_dma_reset`

```c
static void cvitek_dma_reset(const struct cvitek_eth_nic *nic)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

写 DMA 软复位位并轮询其自清零，超时告警后继续。

**位置**：`driver/eth/cvitek_eth.c:355`

<a id="fn-driver-eth-cvitek_eth.c-368"></a>

### `cvitek_setup_tx_ring`

```c
static void cvitek_setup_tx_ring(struct cvitek_eth_nic *nic)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

逐项清零 TX 描述符，末项置终止标志，填入缓冲物理地址并写回 cache。

**位置**：`driver/eth/cvitek_eth.c:368`

<a id="fn-driver-eth-cvitek_eth.c-381"></a>

### `cvitek_setup_rx_ring`

```c
static void cvitek_setup_rx_ring(struct cvitek_eth_nic *nic)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

逐项设置 RX 描述符缓冲地址与长度（末项置链尾标志），屏障后置 OWN 位并写回 cache。

**位置**：`driver/eth/cvitek_eth.c:381`

<a id="fn-driver-eth-cvitek_eth.c-400"></a>

### `cvitek_requeue_rx`

```c
static void cvitek_requeue_rx(struct cvitek_eth_nic *nic, uint32_t slot)
```

把 RX 描述符还给 DMA(读完一帧后调用)

<details>
<summary>函数内备注（1 条）</summary>

**第 415 行**

> 清 RI/NIS(W1C)并催一次 RX 轮询

</details>

**位置**：`driver/eth/cvitek_eth.c:400`

<a id="fn-driver-eth-cvitek_eth.c-421"></a>

### `cvitek_reclaim_tx`

```c
static void cvitek_reclaim_tx(struct cvitek_eth_nic *nic)
```

回收已被 DMA 发完的 TX 描述符

<details>
<summary>函数内备注（1 条）</summary>

**第 428 行（行尾）**

> DMA 还没发完

</details>

**位置**：`driver/eth/cvitek_eth.c:421`

<a id="fn-driver-eth-cvitek_eth.c-437"></a>

### `cvitek_link_up_pred`

```c
static bool cvitek_link_up_pred(void *ctx)
```

timer_poll_until_us 的谓词：BMSR bit2 是 latch-low, 得连读两次

**位置**：`driver/eth/cvitek_eth.c:437`

<a id="fn-driver-eth-cvitek_eth.c-449"></a>

### `cvitek_phy_init`

```c
static void cvitek_phy_init(struct cvitek_eth_nic *nic, bool *speed_100m, bool *full_duplex)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

复位 PHY 并启动 100M 自协商，等 link up 后按本地与对端能力交集推导速率与双工输出。

<details>
<summary>函数内备注（5 条）</summary>

**第 454 行**

> 1. 软复位 PHY

**第 468 行**

> 2. 启动自协商：100M 全双工

**第 472 行**

> 3. 等 link up, 与 Rust 原版一样上限 ~2.5s

**第 487 行**

> 4. 由 ANAR ∩ LPA 定速率/双工

**第 504 行**

> 协商结果无法确定：Rust 原版这里无条件落 100M FD, 保持一致

</details>

**位置**：`driver/eth/cvitek_eth.c:449`

<a id="fn-driver-eth-cvitek_eth.c-516"></a>

### `cvitek_read_mac`

```c
static void cvitek_read_mac(const struct cvitek_eth_nic *nic, uint8_t mac[6])
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取 GMAC 地址低/高寄存器并按小端顺序拆成 6 字节 MAC。

**位置**：`driver/eth/cvitek_eth.c:516`

<a id="fn-driver-eth-cvitek_eth.c-529"></a>

### `cvitek_write_mac`

```c
static void cvitek_write_mac(const struct cvitek_eth_nic *nic, const uint8_t mac[6])
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 6 字节 MAC 拼成低/高寄存器写回，并置地址使能位。

<details>
<summary>函数内备注（1 条）</summary>

**第 535 行**

> bit31 是 AE(地址使能), 必须置 1

</details>

**位置**：`driver/eth/cvitek_eth.c:529`

<a id="fn-driver-eth-cvitek_eth.c-542"></a>

### `cvitek_eth_send`

```c
static int cvitek_eth_send(struct cvitek_eth_nic *nic, const uint8_t *frame, size_t len)
```

返回 0 成功, -1 描述符忙(让调用方下轮重试)/ 参数错

<details>
<summary>函数内备注（3 条）</summary>

**第 556 行（行尾）**

> 环满, DMA 还没回收

**第 565 行**

> CPU 写 → DMA 读：先把帧数据推到内存, 再交给 DMA

**第 576 行**

> 最后写 OWN, 把描述符交给 DMA；再 clean 一次让 OWN 确实落到内存

</details>

**位置**：`driver/eth/cvitek_eth.c:542`

<a id="fn-driver-eth-cvitek_eth.c-588"></a>

### `cvitek_eth_recv`

```c
static int cvitek_eth_recv(struct cvitek_eth_nic *nic, uint8_t *frame, size_t maxlen)
```

返回帧长(>0)；无帧返回 0；错误返回 -1。与 Rust eth_recv 语义一致

<details>
<summary>函数内备注（6 条）</summary>

**第 596 行**

> DMA 写 → CPU 读：先把描述符从内存捞回来

**第 601 行（行尾）**

> DMA 还没填完, 本轮无帧

**第 604 行**

> 坏帧：还回去, 跳过

**第 610 行**

> RDES0.FL 含 4 字节 FCS

**第 618 行**

> DMA 写 → CPU 读：把帧数据从内存捞回来

**第 628 行**

> 立刻还给 DMA(Rust 版靠 RxToken::drop 做, C 里手动)

</details>

**位置**：`driver/eth/cvitek_eth.c:588`

<a id="fn-driver-eth-cvitek_eth.c-636"></a>

### `cvitek_netdev_send`

```c
static int cvitek_netdev_send(void *ctx, const uint8_t *frame, size_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把参数直接转交给 cvitek_eth_send 发送一帧。

**位置**：`driver/eth/cvitek_eth.c:636`

<a id="fn-driver-eth-cvitek_eth.c-641"></a>

### `cvitek_netdev_recv`

```c
static int cvitek_netdev_recv(void *ctx, uint8_t *frame, size_t maxlen)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把参数直接转交给 cvitek_eth_recv 接收一帧。

**位置**：`driver/eth/cvitek_eth.c:641`

<a id="fn-driver-eth-cvitek_eth.c-660"></a>

### `cvitek_eth_isr`

```c
static void cvitek_eth_isr(void *frame, void *ctx)
```

GMAC 收包中断。跑在 PLIC → plic_external_irq → 这里的中断上下文里：
sstatus.SIE 已被硬件清零, 不会嵌套, 无锁。

这里只做两件事：去断言 + 唤醒。任何包处理(pbuf 分配, lwIP 输入)
都留在 net-poll 任务上下文 —— ISR 里做那些会把中断延迟拉爆, 而且
lwIP 不是中断安全的。

特别注意没有提前返回：哪怕驱动状态不正常, 也必须把中断源清掉。
level-high 的中断只要不 ack, PLIC 就会一直重触发, 那是比丢包严重得多的
故障(整机卡死)。所以这里不检查 nic->ready 之类的条件。

<details>
<summary>函数内备注（2 条）</summary>

**第 666 行**

> 清中断源。这里必须只写 W1C 的那几位, 不能把读到的整字写回去：
> DMA_STATUS 里除 W1C 位外还有只读的进程状态编码 —— TPS(发送进程状态,
> bits[2:1])和 RPS(接收进程状态, bit7)之类。收发包时 DMA 状态机在跑,
> 这些位恒为 1, 把 1 写回只读位的行为没有定义, 实测会把发送搞坏
> (症状：收得到帧, 回包发不出去)。

> 本驱动只使能了 RIE|NIE, 所以能触发中断的源只有 RI 和 NIS, ack 这两位
> 就足以让电平降下来。与 cvitek_requeue_rx() 里的做法保持一致。

**第 681 行**

> 通知网络层有新帧。目前 netdev 层没有任何注册者, 所以这是一个空转发 ——
> 因为&quot;中断唤醒阻塞的 net-poll 任务&quot;这条路走不通(原因见 kernel/net/net.c
> 的&quot;收包唤醒链路(暂缓)&quot;), net-poll 回到了协作式轮询, 不需要被唤醒。
> 保留这个调用是为了给将来留一个明确的接入点：修好异常返回路径之后,
> 在 netdev 层注册唤醒函数即可, 驱动这边不用再动。

</details>

**位置**：`driver/eth/cvitek_eth.c:660`

<a id="fn-driver-eth-cvitek_eth.c-693"></a>

### `cvitek_free_allocs`

```c
static void cvitek_free_allocs(struct cvitek_eth_nic *nic)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

依次释放 RX/TX 缓冲与描述符页，最后释放网卡结构本身。

**位置**：`driver/eth/cvitek_eth.c:693`

<a id="fn-driver-eth-cvitek_eth.c-706"></a>

### `cvitek_eth_init`

```c
static int cvitek_eth_init(uintptr_t base)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

分配并校验 DMA 内存与 32 位地址约束，完成 MAC 读写、时钟复位、描述符环、PHY/MAC 配置并注册使能收包中断。

<details>
<summary>函数内备注（18 条）</summary>

**第 721 行**

> 环和帧缓冲都要求「物理连续 + 64 字节对齐 + PA 落在 32 位内」：
> kalloc_pages 返回页对齐, 已清零, 物理连续的内存, 三条都满足,
> 而且比 Rust 版逐个 Box::new 分配 64 次更省事。

> RX 帧缓冲是 512KB(128 页)的连续请求, 而 PMM 那边是线性扫描 bitmap
> 找连续块 —— 碎片化时有可能失败, 所以失败时报清楚是哪一块, 要多少页。

**第 743 行**

> DWMAC 只有 32 位 DMA 地址, 缓冲的物理地址必须落在 4GB 以内

**第 755 行**

> 读硬件 MAC；全 0 / 全 F 说明没烧, 用兜底地址

**第 771 行**

> 时钟 + 复位

**第 774 行**

> DMA 软复位

**第 777 行**

> 建立描述符环

**第 781 行**

> 总线模式：PBL=8, DSL=12(64 字节步长), FB=1, AAL=1

**第 788 行**

> GMAC 侧掩掉 PCS/LPI 等无关中断。注意 DMA 的 RI/TI 不受这个寄存器管,
> 它只管 MAC 侧那一组；DMA 中断由下面的 DMA_INTR_ENA 控制。

**第 793 行**

> DMA 中断先全关, 等描述符环跑起来之后再开(见 init 末尾)

**第 798 行**

> 收所有帧(Promiscuous + ReceiveAll + hash 全 1)

**第 803 行**

> PHY 自协商

**第 807 行**

> MAC 配置

**第 814 行（行尾）**

> 冻结 MMC 计数器

**第 816 行**

> 启动 DMA：先 flush TX FIFO 并等它自清零, 再开 ST/SR

**第 831 行**

> ── 打开收包中断 ────────────────────────────────────────────────
> 顺序有讲究：
> 1. 先装 handler。若先开放 PLIC 源而中断恰好立刻到达, plic.c
> 找不到 handler, 会走&quot;未处理 IRQ&quot;的日志路径(虽然仍会 complete,
> 不会活锁, 但会刷屏)。
> 2. 再开放 PLIC 里这一个源。
> 3. 最后打开 DMA 侧的 RIE|NIE, 让 GMAC 真正拉高中断线。

> 前提是 plic_init() 已经跑过(kernel/main.c 的
> DRV_PHASE_IRQ 的 PLIC 驱动)—— 它负责挂上 S 模式外部中断的
> trap handler 并置 sie.SEIE, 缺了它这里配了也送不达。

**第 861 行**

> 告诉 netdev 层：本驱动有收包中断, 会用 netdev_rx_wakeup() 报告新帧。
> 上层据此用 netdev_rx_pending() 挡掉空轮询 —— 空轮询要碰描述符, 而
> 描述符的 cache invalidate 带两次 fence, 每秒 10 万次就是 ~9% 的 CPU。

**第 871 行**

> 注册到 netdev(注意 netdev_register 是单例, 再注册会覆盖前一个)

**第 879 行**

> recv 与 recv_into 是同一个函数：驱动本来就是把帧写进【调用方给的
> 缓冲】, 两者只差上层要不要多绕一次栈上中间缓冲。都注册上, 让上层
> 走 recv_into 以省掉那一次 1400 字节的拷贝。

</details>

**位置**：`driver/eth/cvitek_eth.c:706`

<a id="fn-driver-eth-cvitek_eth.c-893"></a>

### `cvitek_eth_init_from_platform`

```c
void cvitek_eth_init_from_platform(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从平台配置读取 eth 的 base 地址并调用 cvitek_eth_init 初始化网卡。

<details>
<summary>函数内备注（1 条）</summary>

**第 895 行**

> platform.conf 的 eth.base；sg2002 上 mmio_vma=true, 这里拿到的是高半别名

</details>

**位置**：`driver/eth/cvitek_eth.c:893`

<a id="fn-driver-eth-cvitek_eth.c-903"></a>

### `cvitek_eth_probe`

```c
static int cvitek_eth_probe(const device_t *dev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

忽略设备参数，调用平台初始化函数并返回 0。

<details>
<summary>函数内备注（1 条）</summary>

**第 905 行（行尾）**

> 阶段 1：仍用 platform_get_mmio；阶段 2 改读 net_cfg_t

</details>

**位置**：`driver/eth/cvitek_eth.c:903`

<a id="fn-driver-eth-cvitek_eth.c-918"></a>

### `cvitek_eth_send_probe`

```c
void cvitek_eth_send_probe(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

组装 64 字节广播自定义类型探针帧发送，并用 kprintf 打印返回码与收发计数。

<details>
<summary>函数内备注（4 条）</summary>

**第 927 行（行尾）**

> 目的地址：广播

**第 928 行（行尾）**

> 源地址：本机 MAC

**第 929 行（行尾）**

> 自定义 EtherType

**第 938 行**

> 探针是显式调用的 bring-up 工具, 输出走 kprintf 而不是 KLOG_INFO：
> 上板排障时常用 LOG=none 构建(热路径日志会污染性能测量), 而 KLOG_*
> 在 LOG=none 下被编译期抹除, 用它会让探针变成一声不吭的空操作。

</details>

**位置**：`driver/eth/cvitek_eth.c:918`

<a id="mod-driver-eth-virtio_net.c"></a>

## driver/eth/virtio_net.c

`driver/eth/virtio_net.c` · 26 个函数 · 26 个有说明 · 721 行

<a id="fn-driver-eth-virtio_net.c-159"></a>

### `virtio_netdev_send`

```c
static int virtio_netdev_send(void *ctx, const uint8_t *frame, size_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把参数直接转交 eth_send 发送一帧。

**位置**：`driver/eth/virtio_net.c:159`

<a id="fn-driver-eth-virtio_net.c-164"></a>

### `virtio_netdev_recv`

```c
static int virtio_netdev_recv(void *ctx, uint8_t *frame, size_t maxlen)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把参数直接转交 eth_recv 接收一帧。

**位置**：`driver/eth/virtio_net.c:164`

<a id="fn-driver-eth-virtio_net.c-169"></a>

### `vn_read`

```c
static inline uint32_t vn_read(struct virtio_net_nic *nic, uint32_t off)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取 virtio MMIO 基址加偏移处的 32 位值。

**位置**：`driver/eth/virtio_net.c:169`

<a id="fn-driver-eth-virtio_net.c-174"></a>

### `vn_write`

```c
static inline void vn_write(struct virtio_net_nic *nic, uint32_t off, uint32_t val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

向 virtio MMIO 基址加偏移处写入 32 位值。

**位置**：`driver/eth/virtio_net.c:174`

<a id="fn-driver-eth-virtio_net.c-179"></a>

### `virtio_read_features`

```c
static uint64_t virtio_read_features(struct virtio_net_nic *nic)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

legacy 版本只读一个 32 位特性字，现代版本分别选高低页读出并拼成 64 位。

**位置**：`driver/eth/virtio_net.c:179`

<a id="fn-driver-eth-virtio_net.c-193"></a>

### `virtio_write_features`

```c
static void virtio_write_features(struct virtio_net_nic *nic, uint64_t features)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先写 32 位低特性字，非 legacy 设备再选高页写高 32 位。

**位置**：`driver/eth/virtio_net.c:193`

<a id="fn-driver-eth-virtio_net.c-204"></a>

### `virtio_dma_addr`

```c
static uintptr_t virtio_dma_addr(const void *ptr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

虚拟地址落在内核高半区时减去 KERNEL_VMA 得到物理地址，否则原样返回。

**位置**：`driver/eth/virtio_net.c:204`

<a id="fn-driver-eth-virtio_net.c-212"></a>

### `virtio_status_or`

```c
static void virtio_status_or(struct virtio_net_nic *nic, uint32_t bits)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读出 status 寄存器与给定位或运算后写回。

**位置**：`driver/eth/virtio_net.c:212`

<a id="fn-driver-eth-virtio_net.c-218"></a>

### `virtio_status_reset_done`

```c
static bool virtio_status_reset_done(void *ctx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读出 status 寄存器并判断是否已归零，即复位是否完成。

**位置**：`driver/eth/virtio_net.c:218`

<a id="fn-driver-eth-virtio_net.c-223"></a>

### `virtio_reset`

```c
static int virtio_reset(struct virtio_net_nic *nic)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

写 status 为 0 并轮询等待其归零，超时打错误日志返回 -1。

**位置**：`driver/eth/virtio_net.c:223`

<a id="fn-driver-eth-virtio_net.c-232"></a>

### `virtq_init_free`

```c
static void virtq_init_free(virtio_queue_t *q)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

重置队列索引与 avail/used 标志，并把所有描述符串成以 NEXT 连接的空闲链。

**位置**：`driver/eth/virtio_net.c:232`

<a id="fn-driver-eth-virtio_net.c-250"></a>

### `virtq_alloc_desc`

```c
static int virtq_alloc_desc(virtio_queue_t *q)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从空闲链头摘取一个描述符并更新 free_head、清空其标志，链空返回 -1。

**位置**：`driver/eth/virtio_net.c:250`

<a id="fn-driver-eth-virtio_net.c-266"></a>

### `virtq_free_desc`

```c
static void virtq_free_desc(virtio_queue_t *q, uint16_t id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把指定描述符地址与长度清空后插回空闲链头。

**位置**：`driver/eth/virtio_net.c:266`

<a id="fn-driver-eth-virtio_net.c-275"></a>

### `virtq_free_chain`

```c
static void virtq_free_chain(virtio_queue_t *q, uint16_t id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

沿 NEXT 标志遍历整条描述符链并逐个归还到空闲链。

**位置**：`driver/eth/virtio_net.c:275`

<a id="fn-driver-eth-virtio_net.c-287"></a>

### `virtq_push_avail`

```c
static void virtq_push_avail(struct virtio_net_nic *nic, virtio_queue_t *q, uint16_t qsel, uint16_t id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把描述符 id 写入 avail 环槽位，屏障后递增 idx 并写队列通知寄存器。

**位置**：`driver/eth/virtio_net.c:287`

<a id="fn-driver-eth-virtio_net.c-297"></a>

### `virtio_setup_queue`

```c
static int virtio_setup_queue(struct virtio_net_nic *nic, virtio_queue_t *q, uint32_t qsel, const char *name)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验队列最大长度后初始化空闲链；legacy 写 PFN，现代版写三类环地址并置 ready。

**位置**：`driver/eth/virtio_net.c:297`

<a id="fn-driver-eth-virtio_net.c-341"></a>

### `virtio_net_refill_rx`

```c
static int virtio_net_refill_rx(struct virtio_net_nic *nic, bool notify)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环分配描述符填入接收缓冲地址并挂入 avail 环，可按需通知设备，返回新增数量。

**位置**：`driver/eth/virtio_net.c:341`

<a id="fn-driver-eth-virtio_net.c-373"></a>

### `virtio_net_reclaim_tx`

```c
static void virtio_net_reclaim_tx(struct virtio_net_nic *nic)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

屏障后遍历 used 环，把已完成发送的描述符链归还并清除对应 tx_busy 标记。

**位置**：`driver/eth/virtio_net.c:373`

<a id="fn-driver-eth-virtio_net.c-399"></a>

### `eth_init`

```c
VirtioNetNic_t *eth_init(uint64_t base)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

分配网卡结构并校验 MMIO 版本与设备号，完成特性协商、MAC 获取、双队列初始化后注册 netdev；失败返回 NULL。

**位置**：`driver/eth/virtio_net.c:399`

<a id="fn-driver-eth-virtio_net.c-532"></a>

### `eth_send`

```c
int eth_send(VirtioNetNic_t *opaque, const uint8_t *data, size_t len)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

先回收 TX 完成项，再分配头与帧描述符挂入 avail 环通知设备；描述符耗尽时丢包采样告警并返回 -1。

<details>
<summary>函数内备注（1 条）</summary>

**第 548 行**

> 这是真实丢包(TX 描述符耗尽, 帧被直接丢掉), 以前挂在 DEBUG,
> 默认等级下完全看不见 —— 一个正在丢包的网卡看起来和健康的一模一样。
> 升到 WARN 并采样：busy_count 是持久总数, 采样只影响打印频率,
> 不影响这个计数。

</details>

**位置**：`driver/eth/virtio_net.c:532`

<a id="fn-driver-eth-virtio_net.c-589"></a>

### `eth_recv`

```c
int eth_recv(VirtioNetNic_t *opaque, uint8_t *buf, size_t maxlen)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 used 环取一项并校验，拷贝帧数据给调用方、归还描述符并补充 RX 环，返回拷贝长度。

**位置**：`driver/eth/virtio_net.c:589`

<a id="fn-driver-eth-virtio_net.c-634"></a>

### `eth_mac_addr`

```c
void eth_mac_addr(VirtioNetNic_t *opaque, uint8_t mac[6])
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把网卡 MAC 拷贝到输出缓冲；网卡为空时把输出清零。

**位置**：`driver/eth/virtio_net.c:634`

<a id="fn-driver-eth-virtio_net.c-646"></a>

### `virtio_net_init_from_platform`

```c
void virtio_net_init_from_platform(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取编译期平台 eth 基址，为空则告警跳过，否则调用 eth_init 完成初始化。

**位置**：`driver/eth/virtio_net.c:646`

<a id="fn-driver-eth-virtio_net.c-658"></a>

### `virtio_net_send_probe`

```c
static void virtio_net_send_probe(struct virtio_net_nic *nic)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

组装 64 字节广播自定义类型探针帧调用 eth_send 发出，仅打 DEBUG 日志。

<details>
<summary>函数内备注（1 条）</summary>

**第 671 行**

> bring-up 探针：默认等级下不该占一行

</details>

**位置**：`driver/eth/virtio_net.c:658`

<a id="fn-driver-eth-virtio_net.c-675"></a>

### `virtio_net_probe`

```c
static int virtio_net_probe(const device_t *dev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

忽略设备参数，调用平台初始化函数并返回 0。

<details>
<summary>函数内备注（1 条）</summary>

**第 677 行（行尾）**

> 阶段 1：仍用 DEVICE_ETH_BASE_RAW；阶段 2 改读 net_cfg_t

</details>

**位置**：`driver/eth/virtio_net.c:675`

<a id="fn-driver-eth-virtio_net.c-690"></a>

### `virtio_net_poll_demo_task`

```c
void virtio_net_poll_demo_task(void *arg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

循环接收帧并打印、每满 1 秒发一次探针后让出 CPU；网卡未就绪则告警并退出任务。

**位置**：`driver/eth/virtio_net.c:690`

<a id="mod-driver-ion-ion.c"></a>

## driver/ion/ion.c — 裸机 Ion 内存分配器实现

`driver/ion/ion.c` · 11 个函数 · 11 个有说明 · 275 行

原理：
· 底层通过 pmm_alloc_pages(&g_pmm, n) 分配物理连续页
· 通过 phys_to_virt(pa) 获得内核虚拟地址(sv39 直接映射)
· 用固定大小的表(ION_MAX_BUFS 项)追踪 handle → (pa, size)
· handle = 表中下标 + 1(0 为无效哨兵)
· 线程安全：用 spinlock 保护分配表

对应 ref: rcore-os/tgoskits 中的 IonHeapManager + IonBufferManager。

<a id="fn-driver-ion-ion.c-43"></a>

### `ion_init_once`

```c
static void ion_init_once(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

若尚未初始化则清空全局 ion 表并置初始化标志。

**位置**：`driver/ion/ion.c:43`

<a id="fn-driver-ion-ion.c-52"></a>

### `size_to_pages`

```c
static inline uint32_t size_to_pages(size_t size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把字节数按 4K 页向上取整换算成页数。

**位置**：`driver/ion/ion.c:52`

<a id="fn-driver-ion-ion.c-58"></a>

### `ion_impl_alloc`

```c
static int ion_impl_alloc(size_t size, void **vaddr, uint64_t *paddr, ion_handle_t *handle)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

向 PMM 申请按页连续物理内存，持锁占用空闲表项并清零，输出虚拟地址、物理地址与 handle。

<details>
<summary>函数内备注（3 条）</summary>

**第 69 行**

> 分配物理内存

**第 81 行**

> 找空闲表项

**第 104 行**

> 清零缓冲区(DMA coherent 语义)

</details>

**位置**：`driver/ion/ion.c:58`

<a id="fn-driver-ion-ion.c-117"></a>

### `ion_impl_free`

```c
static int ion_impl_free(ion_handle_t handle)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 handle 后持锁减引用，最后一个引用时清空表项并在锁外把内存归还 PMM。

**位置**：`driver/ion/ion.c:117`

<a id="fn-driver-ion-ion.c-154"></a>

### `ion_impl_ref`

```c
static int ion_impl_ref(ion_handle_t handle)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验 handle 后持锁把该表项的引用计数加一。

**位置**：`driver/ion/ion.c:154`

<a id="fn-driver-ion-ion.c-179"></a>

### `ion_impl_get_buf`

```c
static int ion_impl_get_buf(ion_handle_t handle, void **vaddr, uint64_t *paddr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

持锁查表取出物理地址，按需输出其内核虚拟地址与物理地址。

**位置**：`driver/ion/ion.c:179`

<a id="fn-driver-ion-ion.c-207"></a>

### `ion_impl_get_size`

```c
static size_t ion_impl_get_size(ion_handle_t handle)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

持锁查表返回该 handle 对应的缓冲区大小，无效或未使用返回 0。

**位置**：`driver/ion/ion.c:207`

<a id="fn-driver-ion-ion.c-226"></a>

### `ion_fd_get_phys`

```c
static int ion_fd_get_phys(void *priv, uint64_t *paddr, uint64_t *size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

由 fd 私有 handle 查出物理地址与大小，任一查询失败返回 -1。

**位置**：`driver/ion/ion.c:226`

<a id="fn-driver-ion-ion.c-236"></a>

### `ion_fd_dup`

```c
static int ion_fd_dup(void *priv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

对 fd 私有 handle 增加一次引用计数并返回其结果。

**位置**：`driver/ion/ion.c:236`

<a id="fn-driver-ion-ion.c-241"></a>

### `ion_fd_close`

```c
static void ion_fd_close(void *priv)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

对 fd 私有 handle 释放一次引用并忽略返回值。

**位置**：`driver/ion/ion.c:241`

<a id="fn-driver-ion-ion.c-262"></a>

### `ion_probe`

```c
static int ion_probe(const device_t *dev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 ion 后端操作表绑定到核心层并返回 0。

<details>
<summary>函数内备注（1 条）</summary>

**第 264 行（行尾）**

> service：无设备；表在首次分配时惰性初始化

</details>

**位置**：`driver/ion/ion.c:262`

<a id="mod-driver-ion-ion_core.c"></a>

## driver/ion/ion_core.c — ION 服务注册点

`driver/ion/ion_core.c` · 1 个函数 · 1 个有说明 · 21 行

永远参与构建。ION=1 时 driver/ion/ion.c 的 probe 调 ion_bind()；
否则 g_ion_ops 保持 NULL，include/driver/ion.h 的内联包装统一返回失败。

<a id="fn-driver-ion-ion_core.c-18"></a>

### `ion_bind`

```c
void ion_bind(const ion_ops_t *ops)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

保存传入的 ion 操作表指针供核心层调用。

**位置**：`driver/ion/ion_core.c:18`

<a id="mod-driver-irq-gicv2.c"></a>

## driver/irq/gicv2.c — GICv2 模块内基地址

`driver/irq/gicv2.c` · 47 个函数 · 47 个有说明 · 612 行

<a id="fn-driver-irq-gicv2.c-20"></a>

### `gic_test_init`

```c
void gic_test_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取 GICD_CTLR 与 GICC_CTLR 判断使能状态，并打印 IRQ 数量与 CPU 数用于调试。

**位置**：`driver/irq/gicv2.c:20`

<a id="fn-driver-irq-gicv2.c-35"></a>

### `gicc_init`

```c
void gicc_init()
```

el1 kernel gicc的初始化。smp启动副核执行

<details>
<summary>函数内备注（2 条）</summary>

**第 39 行**

> 设置优先级 为 0xf8

**第 43 行**

> EOImodeNS, bit [9] Controls the behavior of Non-secure accesses to GICC_EOIR GICC_AEOIR, and GICC_DIR

</details>

**位置**：`driver/irq/gicv2.c:35`

<a id="fn-driver-irq-gicv2.c-49"></a>

### `gicc_el2_init`

```c
void gicc_el2_init()
```

el2 hypervisor gicc的初始化。smp启动副核执行

<details>
<summary>函数内备注（3 条）</summary>

**第 53 行**

> 设置优先级 为 0xf8

**第 57 行**

> EOImodeNS, bit [9] Controls the behavior of Non-secure accesses to GICC_EOIR GICC_AEOIR, and GICC_DIR
> 写 EOI 只清除 pending, 需要写 DIR 手动清除 active

**第 64 行**

> bit [2] 当虚拟中断列表寄存器中没有条目时, 会产生中断。

</details>

**位置**：`driver/irq/gicv2.c:49`

<a id="fn-driver-irq-gicv2.c-71"></a>

### `gic_init`

```c
void gic_init(void)
```

gicd g0, g1  gicc enable。smp启动首核执行

<details>
<summary>函数内备注（1 条）</summary>

**第 86 行**

> GICD 启用组0中断, 组1中断转发, 服从优先级规则

</details>

**位置**：`driver/irq/gicv2.c:71`

<a id="fn-driver-irq-gicv2.c-97"></a>

### `gic_init_secondary`

```c
void gic_init_secondary(void)
```

次级核只需要初始化本地 CPU interface(不重复初始化 distributor)

**位置**：`driver/irq/gicv2.c:97`

<a id="fn-driver-irq-gicv2.c-103"></a>

### `gic_virtual_init`

```c
void gic_virtual_init(void)
```

gicd g0, g1  gicc,  gich enable。 smp启动首核执行

<details>
<summary>函数内备注（3 条）</summary>

**第 107 行**

> 关键：必须先从平台拿 MMIO base, 否则后续所有寄存器宏都解析成
> (0 + offset), 写入物理地址 0(QEMU virt 那里是 RAM/device,
> 静默丢弃), 中断永远不会送达。
> gic_init() 已经做了这件事；EL2/VHE 路径走 gic_virtual_init,
> 之前漏写导致 timer IRQ 从未投递。

**第 119 行**

> 获得 gicd irq numbers

**第 127 行**

> GICD 启用组0中断, 组1中断转发, 服从优先级规则

</details>

**位置**：`driver/irq/gicv2.c:103`

<a id="fn-driver-irq-gicv2.c-145"></a>

### `gic_get_typer`

```c
uint32_t gic_get_typer(void)
```

下面是一些get, set函数。 读取寄存器值

**位置**：`driver/irq/gicv2.c:145`

<a id="fn-driver-irq-gicv2.c-150"></a>

### `gic_get_iidr`

```c
uint32_t gic_get_iidr(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取并返回 GICD_IIDR 寄存器值。

**位置**：`driver/irq/gicv2.c:150`

<a id="fn-driver-irq-gicv2.c-155"></a>

### `gic_read_iar`

```c
uint32_t gic_read_iar(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取 GICC_IAR 并返回原始应答值，对非虚假中断的空分支不做任何额外操作。

<details>
<summary>函数内备注（2 条）</summary>

**第 160 行**

> 只在非虚假中断时记录调试信息

**第 162 行**

> 这个太多了

</details>

**位置**：`driver/irq/gicv2.c:155`

<a id="fn-driver-irq-gicv2.c-168"></a>

### `gic_iar_irqnr`

```c
uint32_t gic_iar_irqnr(uint32_t iar)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用掩码从 IAR 值中取出中断号并返回。

**位置**：`driver/irq/gicv2.c:168`

<a id="fn-driver-irq-gicv2.c-173"></a>

### `gic_write_eoir`

```c
void gic_write_eoir(uint32_t irqstat)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把传入的 IAR 原值写入 GICC_EOIR 以结束当前中断。

<details>
<summary>函数内备注（2 条）</summary>

**第 178 行（行尾）**

> Suppress unused variable warning

**第 179 行**

> 这个太多了

</details>

**位置**：`driver/irq/gicv2.c:173`

<a id="fn-driver-irq-gicv2.c-182"></a>

### `gic_write_dir`

```c
void gic_write_dir(uint32_t irqstat)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把传入的 IAR 原值写入 GICC_DIR 以完成中断去激活。

<details>
<summary>函数内备注（2 条）</summary>

**第 187 行（行尾）**

> Suppress unused variable warning

**第 188 行**

> 这个太多了

</details>

**位置**：`driver/irq/gicv2.c:182`

<a id="fn-driver-irq-gicv2.c-192"></a>

### `gic_ipi_send_single`

```c
void gic_ipi_send_single(int32_t irq, int32_t cpu)
```

发送给特定的核(某个核)

**位置**：`driver/irq/gicv2.c:192`

<a id="fn-driver-irq-gicv2.c-208"></a>

### `cpu_num`

```c
uint32_t cpu_num(void)
```

The number of implemented CPU interfaces.

**位置**：`driver/irq/gicv2.c:208`

<a id="fn-driver-irq-gicv2.c-218"></a>

### `gic_enable_int`

```c
void gic_enable_int(int32_t vector, int32_t enabled)
```

Enables the given interrupt.
W1S	Write-1-to-Set	写入 1 会置位(set)对应位, 写 0 不影响
W1C	Write-1-to-Clear	写入 1 会清零(clear)对应位, 写 0 不影响

<details>
<summary>函数内备注（3 条）</summary>

**第 220 行（行尾）**

> vec / 32

**第 221 行（行尾）**

> vec % 32

**第 226 行**

> logger(&quot;set enable: reg: %d, mask: 0x%llx\n&quot;, reg, mask);

</details>

**位置**：`driver/irq/gicv2.c:218`

<a id="fn-driver-irq-gicv2.c-231"></a>

### `gic_get_enable`

```c
int32_t gic_get_enable(int32_t vector)
```

Check the given interrupt.

<details>
<summary>函数内备注（3 条）</summary>

**第 233 行（行尾）**

> vec / 32

**第 234 行（行尾）**

> vec % 32

**第 237 行**

> logger(&quot;get enable: reg: %llx, mask: %llx, value: %llx\n&quot;, reg, mask, val);

</details>

**位置**：`driver/irq/gicv2.c:231`

<a id="fn-driver-irq-gicv2.c-243"></a>

### `gic_is_sgi`

```c
static int32_t gic_is_sgi(int32_t int_id)
```

An auxiliary function for determining whether it is SGI, returning 1 indicates it is SGI and 0 indicates it is not

<details>
<summary>函数内备注（1 条）</summary>

**第 245 行（行尾）**

> SGI 一般是0-15号中断

</details>

**位置**：`driver/irq/gicv2.c:243`

<a id="fn-driver-irq-gicv2.c-249"></a>

### `gic_set_active`

```c
void gic_set_active(int32_t int_id, int32_t act)
```

Set the Active status of the interrupt. act=1 activates, act=0 clears the activation

**位置**：`driver/irq/gicv2.c:249`

<a id="fn-driver-irq-gicv2.c-268"></a>

### `gic_set_pending`

```c
void gic_set_pending(int32_t int_id, int32_t pend, int32_t target_cpu)
```

Set the Pending status of the interrupt. pend=1 sets Pending, pend=0 clears Pending

<details>
<summary>函数内备注（4 条）</summary>

**第 276 行（行尾）**

> 每个 GICD_SPENDSGIR 管 4 个 SGI

**第 277 行（行尾）**

> 每个 SGI 占 8 bit(一个字节, 每个 bit 表示一个 CPU)

**第 285 行**

> 设置指定 CPU 的 SGI pending 位

**第 288 行**

> 清除该 SGI 在所有 CPU 上的 pending(你也可以只清除指定 CPU 的位)

</details>

**位置**：`driver/irq/gicv2.c:268`

<a id="fn-driver-irq-gicv2.c-308"></a>

### `gic_set_ipriority`

```c
void gic_set_ipriority(uint32_t vector, uint32_t pri)
```

Set the interrupt priority

<details>
<summary>函数内备注（4 条）</summary>

**第 310 行（行尾）**

> 哪个寄存器

**第 311 行（行尾）**

> 寄存器内的哪个字节

**第 315 行**

> 设置第 m 字节的优先级

**第 316 行（行尾）**

> GICv2: [7]=1 表示 Group1 (非 secure)

</details>

**位置**：`driver/irq/gicv2.c:308`

<a id="fn-driver-irq-gicv2.c-325"></a>

### `gic_get_ipriority`

```c
int32_t gic_get_ipriority(int32_t vector)
```

Get the interrupt priority

<details>
<summary>函数内备注（4 条）</summary>

**第 327 行（行尾）**

> 每个 IPRIORITYR 寄存器存储 4 个中断(每个占 8 bit)

**第 328 行（行尾）**

> 当前中断在该寄存器中的偏移(0~3)

**第 329 行（行尾）**

> 读取整个 32-bit 寄存器

**第 330 行（行尾）**

> 提取目标中断的 8-bit 优先级

</details>

**位置**：`driver/irq/gicv2.c:325`

<a id="fn-driver-irq-gicv2.c-335"></a>

### `gic_get_target`

```c
int32_t gic_get_target(int32_t int_id)
```

Get the target CPU for a specific interrupt

**位置**：`driver/irq/gicv2.c:335`

<a id="fn-driver-irq-gicv2.c-344"></a>

### `gic_set_target`

```c
void gic_set_target(int32_t int_id, uint8_t target)
```

Set the target CPU for a specific interrupt

**位置**：`driver/irq/gicv2.c:344`

<a id="fn-driver-irq-gicv2.c-366"></a>

### `gic_set_icfgr`

```c
void gic_set_icfgr(int32_t int_id, uint8_t cfg)
```

Set the interrupt configuration (edge/level)

**位置**：`driver/irq/gicv2.c:366`

<a id="fn-driver-irq-gicv2.c-392"></a>

### `gic_make_virtual_hardware_interrupt`

```c
uint32_t gic_make_virtual_hardware_interrupt(uint32_t vector, uint32_t pintvec, int32_t pri, bool grp1)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 LR 位域拼装硬件中断描述字：置 grp0 硬件 pending 标志，并填入优先级、vector、pintvec 与 grp1 位。

<details>
<summary>函数内备注（1 条）</summary>

**第 394 行（行尾）**

> grp0 hw pending

</details>

**位置**：`driver/irq/gicv2.c:392`

<a id="fn-driver-irq-gicv2.c-399"></a>

### `gic_make_virtual_software_interrupt`

```c
uint32_t gic_make_virtual_software_interrupt(uint32_t vector, int32_t pri, bool grp1)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

拼装软件中断 LR 值：置 grp0 pending 标志并填入优先级、vector 与 grp1 位。

<details>
<summary>函数内备注（1 条）</summary>

**第 401 行（行尾）**

> grp0  pending

</details>

**位置**：`driver/irq/gicv2.c:399`

<a id="fn-driver-irq-gicv2.c-406"></a>

### `gic_make_virtual_software_sgi`

```c
uint32_t gic_make_virtual_software_sgi(uint32_t vector, int32_t cpu_id, int32_t pri, bool grp1)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在软件中断 LR 值基础上再填入左移 10 位的目标 cpu_id 后返回。

<details>
<summary>函数内备注（1 条）</summary>

**第 408 行（行尾）**

> grp0  pending

</details>

**位置**：`driver/irq/gicv2.c:406`

<a id="fn-driver-irq-gicv2.c-419"></a>

### `gic_read_lr`

```c
uint32_t gic_read_lr(int32_t n)
```

下面是关于 GICH 的函数

**位置**：`driver/irq/gicv2.c:419`

<a id="fn-driver-irq-gicv2.c-424"></a>

### `gic_lr_read_pri`

```c
int32_t gic_lr_read_pri(uint32_t lr_value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 LR 值中取出优先级位段并右移对齐后返回。

**位置**：`driver/irq/gicv2.c:424`

<a id="fn-driver-irq-gicv2.c-429"></a>

### `gic_lr_read_vid`

```c
uint32_t gic_lr_read_vid(uint32_t lr_value)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 LR 值中掩出低 9 位虚拟中断 ID 并返回。

**位置**：`driver/irq/gicv2.c:429`

<a id="fn-driver-irq-gicv2.c-434"></a>

### `gic_apr`

```c
uint32_t gic_apr()
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取并返回 GICH_APR 寄存器值。

**位置**：`driver/irq/gicv2.c:434`

<a id="fn-driver-irq-gicv2.c-439"></a>

### `gic_elsr0`

```c
uint32_t gic_elsr0()
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取并返回 GICH_ELSR0 寄存器值。

**位置**：`driver/irq/gicv2.c:439`

<a id="fn-driver-irq-gicv2.c-444"></a>

### `gic_elsr1`

```c
uint32_t gic_elsr1()
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取并返回 GICH_ELSR1 寄存器值。

**位置**：`driver/irq/gicv2.c:444`

<a id="fn-driver-irq-gicv2.c-449"></a>

### `gic_write_lr`

```c
void gic_write_lr(int32_t n, uint32_t mask)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验索引范围，越界时打印错误并返回，否则把 mask 写入对应的 GICH_LR 寄存器。

<details>
<summary>函数内备注（2 条）</summary>

**第 459 行（行尾）**

> Suppress unused variable warning

**第 460 行**

> 这个太多了

</details>

**位置**：`driver/irq/gicv2.c:449`

<a id="fn-driver-irq-gicv2.c-464"></a>

### `gic_set_np_int`

```c
void gic_set_np_int(void)
```

启用非优先级中断, 这种中断允许在虚拟化环境下处理一些低优先级的中断。

**位置**：`driver/irq/gicv2.c:464`

<a id="fn-driver-irq-gicv2.c-471"></a>

### `gic_clear_np_int`

```c
void gic_clear_np_int(void)
```

禁用非优先级中断, 确保虚拟机只处理优先级较高的中断。

**位置**：`driver/irq/gicv2.c:471`

<a id="fn-driver-irq-gicv2.c-481"></a>

### `gicv2_dispatch`

```c
static void gicv2_dispatch(void *frame)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读 IAR 取中断号，1020 及以上直接返回，否则分发给 irq_dispatch 后依次写 EOIR 与 DIR。

<details>
<summary>函数内备注（1 条）</summary>

**第 491 行**

> GICC_EOIR 期望 IAR 原值(含 CPUID)；GICC_DIR 在 EOImode=1 时做 deactivate

</details>

**位置**：`driver/irq/gicv2.c:481`

<a id="fn-driver-irq-gicv2.c-496"></a>

### `gicv2_enable_api`

```c
static int gicv2_enable_api(unsigned irq, bool on)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

中断号不小于 1020 时返回 -2，否则调用 gic_enable_int 开关该中断并返回 0。

**位置**：`driver/irq/gicv2.c:496`

<a id="fn-driver-irq-gicv2.c-504"></a>

### `gicv2_set_priority_api`

```c
static int gicv2_set_priority_api(unsigned irq, unsigned prio)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

中断号越界时返回 -2，否则把优先级的低 8 位写入 GIC 并返回 0。

**位置**：`driver/irq/gicv2.c:504`

<a id="fn-driver-irq-gicv2.c-512"></a>

### `gicv2_secondary_init_api`

```c
static int gicv2_secondary_init_api(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

调用 gic_init_secondary() 完成从核 GIC 初始化并返回 0。

**位置**：`driver/irq/gicv2.c:512`

<a id="fn-driver-irq-gicv2.c-518"></a>

### `gicv2_send_ipi_api`

```c
static int gicv2_send_ipi_api(uint32_t hw_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取 hw_id 低 8 位作目标，目标不小于 8 时返回 -2，否则发送重调度 SGI 并返回 0。

<details>
<summary>函数内备注（1 条）</summary>

**第 521 行（行尾）**

> GICD_SGIR 的 TargetList 只有 8 位

</details>

**位置**：`driver/irq/gicv2.c:518`

<a id="fn-driver-irq-gicv2.c-527"></a>

### `gicv2_register_ipi_api`

```c
static int gicv2_register_ipi_api(irq_handler_fn fn, void *ctx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

注册重调度 SGI 的处理函数，成功后把该中断优先级设为 0 并使能。

**位置**：`driver/irq/gicv2.c:527`

<a id="fn-driver-irq-gicv2.c-538"></a>

### `gicv2_register_timer_api`

```c
static int gicv2_register_timer_api(irq_handler_fn fn, void *ctx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验定时器号与 GIC 基址后注册 CNTP 定时器中断，设最高优先级并使能。

**位置**：`driver/irq/gicv2.c:538`

<a id="fn-driver-irq-gicv2.c-552"></a>

### `gicv2_timer_enable_api`

```c
static int gicv2_timer_enable_api(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

定时器号非法时返回 -1，否则使能该定时器中断并返回 0。

**位置**：`driver/irq/gicv2.c:552`

<a id="fn-driver-irq-gicv2.c-560"></a>

### `gicv2_cpu_hw_id_api`

```c
static uint64_t gicv2_cpu_hw_id_api(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取 MPIDR_EL1 并返回其低 24 位作为 CPU 硬件 ID。

**位置**：`driver/irq/gicv2.c:560`

<a id="fn-driver-irq-gicv2.c-567"></a>

### `gicv2_debug_dump_api`

```c
static void gicv2_debug_dump_api(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打印 GICD/GICC 基址及 CTLR、使能、pending、优先级、PMR、HPPIR、RPR 等寄存器值供 SMP 调试。

**位置**：`driver/irq/gicv2.c:567`

<a id="fn-driver-irq-gicv2.c-598"></a>

### `gicv2_probe`

```c
static int gicv2_probe(const device_t *dev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

忽略设备参数，把 GICv2 操作集绑定为中断后端并调用 gic_virtual_init()。

<details>
<summary>函数内备注（1 条）</summary>

**第 600 行（行尾）**

> 阶段 1：gic_virtual_init() 仍直接读平台配置

</details>

**位置**：`driver/irq/gicv2.c:598`

<a id="mod-driver-irq-gicv3.c"></a>

## driver/irq/gicv3.c — 宿主 GICv3 驱动(AArch64 @ EL2 / VHE)

`driver/irq/gicv3.c` · 38 个函数 · 38 个有说明 · 706 行

与 gicv2.c 的分工差异：
- GICv2：GICD 统管全部 INTID, GICC 是 MMIO 的 CPU interface,
vGIC 走 GICH/GICV 的 MMIO。
- GICv3：GICD 只管 SPI(ARE 模式), SGI/PPI 在 per-CPU 的 GICR 里,
CPU interface 是 ICC_* 系统寄存器, vGIC 走 ICH_* 系统寄存器。

VHE(E2H=1)说明：宿主跑在 EL2, 但访问 `ICC_PMR_EL1` / `ICC_IAR1_EL1`
这类 `S3_0_*` 编码时, E2H=1 会把它们重定向到 EL2 视角的物理 CPU
interface。因此这里的寄存器名和普通 EL1 内核一样, 不需要写 `_EL2` 版。
ICC_SRE_EL2 没有 EL1 别名语义(它是 EL2 专有寄存器), 用 op1=4 编码显式访问。

<a id="fn-driver-irq-gicv3.c-52"></a>

### `gicv3_find_cpu_gicr`

```c
static uintptr_t gicv3_find_cpu_gicr(void)
```

gicv3_find_cpu_gicr - 按 MPIDR 亲和性在 GICR 链中找到当前核的 Redistributor

GICR 是一串 128KB 对齐的块, 块的 GICR_TYPER[63:32] 保存该核的亲和性,
[4]=Last 表示链尾。找不到(或遍历到 Last)时回退到 gicv3_gicr_base。

<details>
<summary>函数内备注（2 条）</summary>

**第 57 行**

> MPIDR Aff3:Aff2:Aff1:Aff0 → GICR_TYPER[63:32] 同名字段

**第 66 行（行尾）**

> GICR_TYPER.Last

</details>

**位置**：`driver/irq/gicv3.c:52`

<a id="fn-driver-irq-gicv3.c-74"></a>

### `gicv3_wake_redistributor`

```c
static void gicv3_wake_redistributor(uintptr_t gicr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

清除 GICR_WAKER 的 ProcessorSleep 位并同步，轮询等待 ChildrenAsleep 清零，超时打印告警。

<details>
<summary>函数内备注（2 条）</summary>

**第 76 行（行尾）**

> GICR_WAKER

**第 79 行（行尾）**

> 必须确实到达 GIC 设备

</details>

**位置**：`driver/irq/gicv3.c:74`

<a id="fn-driver-irq-gicv3.c-96"></a>

### `gicv3_config_sgi_frame`

```c
static void gicv3_config_sgi_frame(uintptr_t gicr)
```

把本核 SGI/PPI 全部划到非安全 Group1 并设默认优先级。
PPI 默认属于 Group0；只有 ICC_IGRPEN1_EL1 使能时 Group0 的 PPI 会被
CPU interface 过滤掉, 宿主就永远收不到 timer。GICv2 那边靠
gic_set_ipriority() 里的 bit7 顺手做了这件事, GICv3 的 group 是
GICR_IGROUPR0 独立寄存器, 必须显式写。

<details>
<summary>函数内备注（4 条）</summary>

**第 100 行（行尾）**

> GICR_IGROUPR0

**第 101 行**

> IGRPMODR0 必须一起写 1。

> 中断的组由两个位共同决定：IGROUPR=1 且 IGRPMODR=0 → Group 1 Secure,
> 只有两个都为 1 才是 Guest 用的 Group 1 Non-secure。IGRPMODR 复位值是 0,
> 只写 IGROUPR 会把 PPI 全划进 Secure 组：无 EL3 时这类中断根本不会以
> IRQ 形式送到 CPU interface, 表现为「GICR_ISENABLER0 已置位, PPI 已
> pending, PMR/IGRPEN1 都对, 但永远收不到中断」(实测 QEMU trace 里
> gicv3_cpuif_set_irqs 始终是 IRQ 0)。

**第 111 行（行尾）**

> GICR_IGRPMODR0

**第 114 行（行尾）**

> GICR_IPRIORITYRn

</details>

**位置**：`driver/irq/gicv3.c:96`

<a id="fn-driver-irq-gicv3.c-127"></a>

### `gicv3_enable_cpuif_sysreg`

```c
static void gicv3_enable_cpuif_sysreg(void)
```

使能 CPU interface 的系统寄存器访问。

VHE 下不能访问 ICC_SRE_EL2 的 EL2 编码(UNDEFINED), EL2 视角的
ICC_SRE_EL1 就是它 —— Enable(bit3) 是「允许 EL1 访问 ICC_SRE_EL1」的闸门,
SRE(bit0) 选择系统寄存器接口, DIB/DFB 关掉两个内存屏障暗示。
没有 Enable, 后续对 ICC_PMR_EL1 / ICC_IGRPEN1_EL1 的写入不会生效。

<details>
<summary>函数内备注（1 条）</summary>

**第 133 行（行尾）**

> 写完 ICC_SRE 必须 isb 才对后续访问生效

</details>

**位置**：`driver/irq/gicv3.c:127`

<a id="fn-driver-irq-gicv3.c-137"></a>

### `gicv3_init`

```c
void gicv3_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取 GICD/GICR 基址，使能 Group1 与 ARE 并等 RWP 清零，设 SPI 优先级，唤醒本核 redistributor 并配 CPU 接口。

<details>
<summary>函数内备注（5 条）</summary>

**第 154 行**

> GICD_CTLR：ARE(亲和性路由, SPI 目标改用 IROUTER)+
> EnableGrp1NS + EnableGrp1A。

> EnableGrp1A(bit1) 是 Group1 中断「从 distributor 转发到 CPU interface」
> 的总闸门, 只写 EnableGrp1NS 不够：实测(`make GIC=v3` + SM 自检)
> 少了这一位时 PPI 26 在 GICR 里 pending/enabled 都正常, PMR/IGRPEN1
> 也对, 但 `ICC_IAR1_EL1` 永远返回 spurious(1023), timer ISR 一次都进不来。
> 逐位扫描 GICD_CTLR 确认：{0x31,0x11,0x41,0x51} 都收不到,
> {0x33,0x37,0x13}(都含 bit1)立刻开始 tick。
> Linux 的 gic_dist_init() 同样同时写 GICD_CTLR_ENABLE_G1 和 _G1A。

**第 169 行**

> ARE 生效需要等 RWP 清零, 否则后续 SPI 配置会被丢弃

**第 178 行**

> SPI：默认优先级 0xA0, Group1, 全部禁止

**第 185 行**

> 本核 Redistributor：先设 SRE, 再唤醒(顺序错会死锁, 见函数注释)

**第 192 行**

> CPU interface：EOImode=0(EOI 一步完成 drop + deactivate)

</details>

**位置**：`driver/irq/gicv3.c:137`

<a id="fn-driver-irq-gicv3.c-211"></a>

### `gicv3_init_secondary`

```c
void gicv3_init_secondary(void)
```

gicv3_init_secondary - 次级核 per-CPU GICv3 初始化

ARM GICv3 规范规定的唤醒顺序：
1. 先使能 CPU interface(ICC_SRE), 然后 ISB
2. 再清 GICR_WAKER.ProcessorSleep, 等 ChildrenAsleep=0
3. 最后配置 ICC_CTLR / ICC_PMR / ICC_IGRPEN1
若先等 ChildrenAsleep 而未设 ICC_SRE, CPU interface 未激活,
redistributor 的唤醒握手完不成 → 死锁。

**位置**：`driver/irq/gicv3.c:211`

<a id="fn-driver-irq-gicv3.c-232"></a>

### `gicv3_enable_int`

```c
void gicv3_enable_int(int int_id, bool enable)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按中断号区分 SGI/PPI 与 SPI，向对应 GICR 或 GICD 的使能/禁止寄存器写入位掩码。

<details>
<summary>函数内备注（1 条）</summary>

**第 237 行**

> SGI/PPI：per-CPU banked, 写本核 GICR SGI frame

</details>

**位置**：`driver/irq/gicv3.c:232`

<a id="fn-driver-irq-gicv3.c-247"></a>

### `gicv3_is_int_enabled`

```c
bool gicv3_is_int_enabled(int int_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按中断号读取对应的 GICR 或 GICD 使能寄存器，判断掩码位是否置位。

<details>
<summary>函数内备注（1 条）</summary>

**第 254 行（行尾）**

> GICR_ISENABLER0

</details>

**位置**：`driver/irq/gicv3.c:247`

<a id="fn-driver-irq-gicv3.c-261"></a>

### `gicv3_set_int_trigger`

```c
void gicv3_set_int_trigger(uint32_t int_id, int edge)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按中断号定位 GICR 或 GICD 的 ICFGR，读改写其中 2 位字段设置边沿或电平触发。

<details>
<summary>函数内备注（1 条）</summary>

**第 268 行**

> SGI/PPI 的 ICFGR 在 GICR SGI frame(每核一份, 共 2 个字)

</details>

**位置**：`driver/irq/gicv3.c:261`

<a id="fn-driver-irq-gicv3.c-295"></a>

### `gicv3_set_int_target`

```c
void gicv3_set_int_target(uint32_t int_id, uint8_t target_cpu_mask)
```

ARE 模式下 SPI 的目标不再是 8 位掩码 + ITARGETSR, 而是 GICD_IROUTER<n>
里的完整亲和性(Aff3:Aff2:Aff1:Aff0)+ IRM。
这里把 target_cpu_mask 的最低置位当作目标核编号, 换算成亲和性；
与 GICv2 语义(一位一个核)保持一致。

<details>
<summary>函数内备注（1 条）</summary>

**第 298 行（行尾）**

> SGI/PPI 是 per-CPU, 没有目标配置

</details>

**位置**：`driver/irq/gicv3.c:295`

<a id="fn-driver-irq-gicv3.c-311"></a>

### `gicv3_read_iar`

```c
uint32_t gicv3_read_iar(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取 ICC_IAR1_EL1 并返回中断应答值。

**位置**：`driver/irq/gicv3.c:311`

<a id="fn-driver-irq-gicv3.c-316"></a>

### `gicv3_iar_irqnr`

```c
uint32_t gicv3_iar_irqnr(uint32_t iar)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 ICC_IAR_INTID_MASK 从 IAR 值中掩出中断号。

**位置**：`driver/irq/gicv3.c:316`

<a id="fn-driver-irq-gicv3.c-321"></a>

### `gicv3_write_eoir`

```c
void gicv3_write_eoir(uint32_t irqstat)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把中断应答值写入 ICC_EOIR1_EL1 结束当前中断。

**位置**：`driver/irq/gicv3.c:321`

<a id="fn-driver-irq-gicv3.c-333"></a>

### `gic_set_ipriority`

```c
void gic_set_ipriority(uint32_t int_id, uint32_t priority)
```

GICv2 兼容接口：timer_aarch64_impl.h / exception.c 直接 extern 调用。

gic_set_ipriority：GICv3 下 INTID < 32 的优先级在 GICR 而不是 GICD。
之前这里无条件写 GICD_IPRIORITYR, 导致宿主 PPI 26(CNTHP)的优先级
根本没写进去。

<details>
<summary>函数内备注（1 条）</summary>

**第 342 行（行尾）**

> GICR_IPRIORITYRn

</details>

**位置**：`driver/irq/gicv3.c:333`

<a id="fn-driver-irq-gicv3.c-362"></a>

### `gic_write_dir`

```c
void gic_write_dir(uint32_t irqstat)
```

gic_write_dir：GICv2 的「撤销激活」。
GICv3 宿主用 EOImode=0, ICC_EOIR1_EL1 一步完成优先级下降 + deactivate,
没有 GICC_DIR 这个 MMIO 寄存器, 因此这里是空操作。

**位置**：`driver/irq/gicv3.c:362`

<a id="fn-driver-irq-gicv3.c-369"></a>

### `gicv3_vtr_nr_lrs`

```c
uint32_t gicv3_vtr_nr_lrs(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读 ICH_VTR_EL2 的 ListRegs 字段加一得到 LR 数量，并封顶到 GICV3_MAX_LRS。

<details>
<summary>函数内备注（1 条）</summary>

**第 372 行（行尾）**

> ListRegs[3:0]

</details>

**位置**：`driver/irq/gicv3.c:369`

<a id="fn-driver-irq-gicv3.c-379"></a>

### `gicv3_write_lr`

```c
void gicv3_write_lr(int32_t n, uint64_t value)
```

ICH_LR<n>_EL2 的寄存器编号不连续：n=0..7 在 CRm=12, n=8..15 在 CRm=13。

**位置**：`driver/irq/gicv3.c:379`

<a id="fn-driver-irq-gicv3.c-438"></a>

### `gicv3_read_lr`

```c
uint64_t gicv3_read_lr(int32_t n)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按索引 0 到 15 选择对应的 ICH_LR 系统寄存器读取并返回，索引越界时返回 0。

**位置**：`driver/irq/gicv3.c:438`

<a id="fn-driver-irq-gicv3.c-478"></a>

### `gicv3_read_elrsr`

```c
uint64_t gicv3_read_elrsr(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取并返回 ICH_ELRSR_EL2 寄存器值。

**位置**：`driver/irq/gicv3.c:478`

<a id="fn-driver-irq-gicv3.c-482"></a>

### `gicv3_read_eisr`

```c
uint64_t gicv3_read_eisr(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取并返回 ICH_EISR_EL2 寄存器值。

**位置**：`driver/irq/gicv3.c:482`

<a id="fn-driver-irq-gicv3.c-486"></a>

### `gicv3_read_misr`

```c
uint64_t gicv3_read_misr(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取并返回 ICH_MISR_EL2 寄存器值。

**位置**：`driver/irq/gicv3.c:486`

<a id="fn-driver-irq-gicv3.c-490"></a>

### `gicv3_read_hcr`

```c
uint64_t gicv3_read_hcr(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取并返回 ICH_HCR_EL2 寄存器值。

**位置**：`driver/irq/gicv3.c:490`

<a id="fn-driver-irq-gicv3.c-494"></a>

### `gicv3_write_hcr`

```c
void gicv3_write_hcr(uint64_t v)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把值写入 ICH_HCR_EL2，随后执行指令同步屏障。

**位置**：`driver/irq/gicv3.c:494`

<a id="fn-driver-irq-gicv3.c-502"></a>

### `gicv3_get_typer`

```c
uint32_t gicv3_get_typer(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取并返回 GICD_TYPER 寄存器值。

**位置**：`driver/irq/gicv3.c:502`

<a id="fn-driver-irq-gicv3.c-507"></a>

### `gicv3_get_iidr`

```c
uint32_t gicv3_get_iidr(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取并返回 GICD_IIDR 寄存器值。

**位置**：`driver/irq/gicv3.c:507`

<a id="fn-driver-irq-gicv3.c-512"></a>

### `cpu_num`

```c
uint32_t cpu_num(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 GICD_TYPER 中解析 CPU 数量字段并返回。

**位置**：`driver/irq/gicv3.c:512`

<a id="fn-driver-irq-gicv3.c-533"></a>

### `gicv3_ipi_send_single`

```c
void gicv3_ipi_send_single(int32_t irq, int32_t cpu)
```

GICv3 用 ICC_SGI1R_EL1 生成 SGI(GICv2 的 GICD_SGIR 已废弃)

ICC_SGI1R_EL1 字段(ARM IHI 0069)：
[15:0]  TargetList —— Aff0 位图(RS=0 时)
[23:16] Aff1
[27:24] INTID      —— SGI 号, 0..15
[39:32] Aff2
[40]    IRM        —— 0 = 只投 TargetList
[47:44] RS         —— 0 = 单个 target list(不按范围选择)
[55:48] Aff3

注意： 历史 bug：旧实现写成 `irq | (1 << (16 + cpu))` —— 把 INTID 放进了
TargetList, 把 Aff0 放进了 Aff1, 结果 SGI 根本投不出去(之前没有调用者,
所以一直没暴露)。多核 resched IPI 是第一个用户。

<details>
<summary>函数内备注（2 条）</summary>

**第 540 行（行尾）**

> INTID [27:24]

**第 541 行（行尾）**

> TargetList [15:0]

</details>

**位置**：`driver/irq/gicv3.c:533`

<a id="fn-driver-irq-gicv3.c-557"></a>

### `gicv3_dispatch`

```c
static void gicv3_dispatch(void *frame)
```

gicv3_dispatch - 架构 IRQ 入口的完整一次中断

时序必须严格是 IAR → handler → EOIR(→ DIR)。EOImode=0 时 EOIR 同时
完成优先级下降和 deactivate；GICv3 没有 GICC_DIR, gic_write_dir 是空操作。
IAR 原值整个写回 EOIR(而不是只写 INTID)：GICv2 的 IAR 带 CPUID,
只写 INTID 在部分实现上不能正确匹配。

<details>
<summary>函数内备注（1 条）</summary>

**第 562 行**

> 1020..1023 是 spurious/特殊 INTID，不参与 EOI

</details>

**位置**：`driver/irq/gicv3.c:557`

<a id="fn-driver-irq-gicv3.c-572"></a>

### `gicv3_enable_api`

```c
static int gicv3_enable_api(unsigned irq, bool on)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

中断号不小于 1020 时返回 -2，否则调用 gicv3_enable_int 开关中断并返回 0。

**位置**：`driver/irq/gicv3.c:572`

<a id="fn-driver-irq-gicv3.c-580"></a>

### `gicv3_set_priority_api`

```c
static int gicv3_set_priority_api(unsigned irq, unsigned prio)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

中断号越界时返回 -2，否则把优先级的低 8 位写入 GIC 并返回 0。

**位置**：`driver/irq/gicv3.c:580`

<a id="fn-driver-irq-gicv3.c-588"></a>

### `gicv3_secondary_init_api`

```c
static int gicv3_secondary_init_api(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

调用 gicv3_init_secondary() 完成从核初始化并返回 0。

**位置**：`driver/irq/gicv3.c:588`

<a id="fn-driver-irq-gicv3.c-600"></a>

### `gicv3_send_ipi_api`

```c
static int gicv3_send_ipi_api(uint32_t hw_id)
```

GICv3 的 SGI 目标必须按完整亲和性编码。旧接口把逻辑 CPU 号当 Aff0
写 TargetList，QEMU virt 单簇下成立，RK3588 多簇上 IPI 会投错核。
hw_id 是 cpu_t.hw_id(MPIDR 低 24 位)，这里拆成 Aff1/Aff2 填进
ICC_SGI1R_EL1[23:16]/[39:32]。

**位置**：`driver/irq/gicv3.c:600`

<a id="fn-driver-irq-gicv3.c-615"></a>

### `gicv3_register_ipi_api`

```c
static int gicv3_register_ipi_api(irq_handler_fn fn, void *ctx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

注册重调度 SGI 的处理函数，成功后设优先级 0 并使能该中断。

**位置**：`driver/irq/gicv3.c:615`

<a id="fn-driver-irq-gicv3.c-626"></a>

### `gicv3_register_timer_api`

```c
static int gicv3_register_timer_api(irq_handler_fn fn, void *ctx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验定时器号与 GIC 基址后注册 CNTP 定时器中断，设最高优先级并使能。

**位置**：`driver/irq/gicv3.c:626`

<a id="fn-driver-irq-gicv3.c-640"></a>

### `gicv3_timer_enable_api`

```c
static int gicv3_timer_enable_api(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

定时器号非法时返回 -1，否则使能 CNTP 定时器中断并返回 0。

**位置**：`driver/irq/gicv3.c:640`

<a id="fn-driver-irq-gicv3.c-648"></a>

### `gicv3_cpu_hw_id_api`

```c
static uint64_t gicv3_cpu_hw_id_api(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取 MPIDR_EL1 并返回其低 24 位作为 CPU 硬件 ID。

**位置**：`driver/irq/gicv3.c:648`

<a id="fn-driver-irq-gicv3.c-656"></a>

### `gicv3_debug_dump_api`

```c
static void gicv3_debug_dump_api(void)
```

SMP 自检用：把「中断卡在哪一层」需要看的寄存器集中在这里

<details>
<summary>函数内备注（4 条）</summary>

**第 664 行**

> PPI 实际开关 / 组配置 / 优先级在 GICR SGI frame, 不在 GICD

**第 670 行（行尾）**

> ICC_PMR_EL1

**第 671 行（行尾）**

> ICC_IGRPEN1_EL1

**第 672 行（行尾）**

> ICC_CTLR_EL1

</details>

**位置**：`driver/irq/gicv3.c:656`

<a id="fn-driver-irq-gicv3.c-692"></a>

### `gicv3_probe`

```c
static int gicv3_probe(const device_t *dev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

忽略设备参数，把 GICv3 操作集绑定为中断后端并调用 gicv3_init()。

<details>
<summary>函数内备注（1 条）</summary>

**第 694 行（行尾）**

> 阶段 1：gicv3_init() 仍直接读平台配置

</details>

**位置**：`driver/irq/gicv3.c:692`

<a id="mod-driver-irq-irq.c"></a>

## driver/irq/irq.c — 中断控制器核心

`driver/irq/irq.c` · 14 个函数 · 14 个有说明 · 146 行

只做三件事：
1. 保存控制器后端(core 由 IRQ 类驱动 probe 时 irq_bind)；
2. 维护「设备中断号 → (handler, ctx)」表，号空间由后端定义；
3. 把内核/驱动调用转发给后端。

中断上下文不加锁：注册只发生在相应设备 probe 阶段（开中断之前），
之后这张表是只读的。可见性用 wmb()/rmb() 保证。

<a id="fn-driver-irq-irq.c-32"></a>

### `irq_bind`

```c
void irq_bind(const irq_ops_t *ops)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

忽略 NULL，在前后写屏障保护下把全局中断操作集指针替换为 ops。

**位置**：`driver/irq/irq.c:32`

<a id="fn-driver-irq-irq.c-42"></a>

### `irq_register`

```c
int irq_register(unsigned irq, irq_handler_fn fn, void *ctx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

未绑定控制器或中断号与处理函数非法时返回负值，否则先写上下文再写函数指针完成注册。

**位置**：`driver/irq/irq.c:42`

<a id="fn-driver-irq-irq.c-56"></a>

### `irq_dispatch`

```c
int irq_dispatch(unsigned irq, void *frame)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验中断号，无处理函数时以限流告警返回 -1，否则调用处理函数并返回 0。

<details>
<summary>函数内备注（1 条）</summary>

**第 63 行**

> 中断风暴时每个中断一行会堵死串口；KLOG_WARN_SAMPLE 自带限流。
> 这里在 ISR 上下文，不能打未采样的日志。

</details>

**位置**：`driver/irq/irq.c:56`

<a id="fn-driver-irq-irq.c-75"></a>

### `irq_dispatch_frame`

```c
void irq_dispatch_frame(void *frame)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

若已绑定控制器且其 dispatch 回调非空，则用它分发该异常帧。

**位置**：`driver/irq/irq.c:75`

<a id="fn-driver-irq-irq.c-81"></a>

### `irq_enable`

```c
int irq_enable(unsigned irq, bool on)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

未绑定控制器或缺少 enable 回调时返回 -1，否则转发给控制器实现。

**位置**：`driver/irq/irq.c:81`

<a id="fn-driver-irq-irq.c-88"></a>

### `irq_set_priority`

```c
int irq_set_priority(unsigned irq, unsigned prio)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

未绑定控制器或缺少回调时返回 -1，否则转发给控制器设置优先级。

**位置**：`driver/irq/irq.c:88`

<a id="fn-driver-irq-irq.c-95"></a>

### `irq_secondary_init`

```c
int irq_secondary_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

控制器不需要每核初始化时返回 0，否则调用其 secondary_init 回调。

<details>
<summary>函数内备注（1 条）</summary>

**第 98 行（行尾）**

> 该控制器不需要 per-CPU 初始化

</details>

**位置**：`driver/irq/irq.c:95`

<a id="fn-driver-irq-irq.c-102"></a>

### `irq_send_ipi`

```c
int irq_send_ipi(uint32_t hw_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

未绑定控制器或缺少回调时返回 -1，否则按硬件 ID 转发发送 IPI。

**位置**：`driver/irq/irq.c:102`

<a id="fn-driver-irq-irq.c-109"></a>

### `irq_register_ipi`

```c
int irq_register_ipi(irq_handler_fn fn, void *ctx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

缺少控制器或回调时返回 -1，否则把处理函数注册到控制器的 IPI 通道。

**位置**：`driver/irq/irq.c:109`

<a id="fn-driver-irq-irq.c-116"></a>

### `irq_register_timer`

```c
int irq_register_timer(irq_handler_fn fn, void *ctx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

缺少控制器或回调时返回 -1，否则把处理函数注册到控制器的定时器通道。

**位置**：`driver/irq/irq.c:116`

<a id="fn-driver-irq-irq.c-123"></a>

### `irq_timer_enable`

```c
int irq_timer_enable(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

无控制器或不支持时返回 0，否则调用控制器的 timer_enable 回调。

**位置**：`driver/irq/irq.c:123`

<a id="fn-driver-irq-irq.c-130"></a>

### `irq_cpu_hw_id`

```c
uint64_t irq_cpu_hw_id(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

有控制器与回调时返回其 cpu_hw_id 结果，否则返回 0。

**位置**：`driver/irq/irq.c:130`

<a id="fn-driver-irq-irq.c-137"></a>

### `irq_debug_dump`

```c
void irq_debug_dump(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

若控制器提供 debug_dump 回调则调用它输出调试信息。

**位置**：`driver/irq/irq.c:137`

<a id="fn-driver-irq-irq.c-143"></a>

### `irq_virt_supported`

```c
bool irq_virt_supported(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回控制器已绑定且其 virt_supported 标志为真。

**位置**：`driver/irq/irq.c:143`

<a id="mod-driver-irq-lapic.c"></a>

## driver/irq/lapic.c — x86_64 Local APIC 驱动实现

`driver/irq/lapic.c` · 34 个函数 · 34 个有说明 · 614 行

使用 PIT Channel 2 作为参考时钟来校准 LAPIC timer 频率：
1. 以 PIT 频率(1.193182 MHz)计时 10ms
2. 读取这 10ms 内 LAPIC 计数器的变化量
3. 推算 LAPIC bus 频率, 设置 Periodic 模式的初始计数

物理内存映射：LAPIC 基址 0xFEE00000 已在 boot.S 的 1GB huge page
中被映射到虚拟地址 0xffff800000000000 + 0xFEE00000。

<a id="fn-driver-irq-lapic.c-53"></a>

### `lapic_read`

```c
static inline uint32_t lapic_read(uint32_t reg)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 volatile 读访问 LAPIC 虚拟地址加寄存器偏移处的 32 位值。

**位置**：`driver/irq/lapic.c:53`

<a id="fn-driver-irq-lapic.c-59"></a>

### `lapic_write`

```c
static inline void lapic_write(uint32_t reg, uint32_t val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

以 volatile 写把值写入 LAPIC 虚拟地址加偏移处的 32 位寄存器。

**位置**：`driver/irq/lapic.c:59`

<a id="fn-driver-irq-lapic.c-68"></a>

### `ioapic_write`

```c
static void ioapic_write(uint32_t reg, uint32_t val)
```

── IOAPIC 寄存器访问 ──────────────────────────────────────────
间接寻址：先写 IOREGSEL，再写 IOWIN。

**位置**：`driver/irq/lapic.c:68`

<a id="fn-driver-irq-lapic.c-86"></a>

### `ioapic_set_redir`

```c
static int ioapic_set_redir(unsigned gsi, uint8_t vector, uint8_t dest, bool masked)
```

设置一条 GSI 的重定向表项。

ISA 串口是电平信号，但 ISA 总线按边沿触发上报；QEMU 的 isa-serial
也按 4-edge 工作（与 Linux 在 QEMU 上的 IOAPIC 配置一致）。因此这里
使用 fixed delivery / physical destination / active-high / edge。

<details>
<summary>函数内备注（2 条）</summary>

**第 92 行（行尾）**

> fixed, physical, active-high, edge

**第 95 行**

> 更新期间先屏蔽，避免写入一半的表项被投递

</details>

**位置**：`driver/irq/lapic.c:86`

<a id="fn-driver-irq-lapic.c-119"></a>

### `lapic_cpu_index`

```c
static uint32_t lapic_cpu_index(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

取当前 CPU ID，超出 AVATAR_MAX_CPUS 时回退为 0。

**位置**：`driver/irq/lapic.c:119`

<a id="fn-driver-irq-lapic.c-125"></a>

### `_lapic_rdtsc`

```c
static inline uint64_t _lapic_rdtsc(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用 rdtsc 指令读取时间戳计数器并拼成 64 位值返回。

**位置**：`driver/irq/lapic.c:125`

<a id="fn-driver-irq-lapic.c-132"></a>

### `_lapic_wrmsr`

```c
static inline void _lapic_wrmsr(uint32_t msr, uint64_t val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 64 位值拆成高低 32 位，用 wrmsr 写入指定 MSR。

**位置**：`driver/irq/lapic.c:132`

<a id="fn-driver-irq-lapic.c-160"></a>

### `pit_start_oneshot`

```c
static void pit_start_oneshot(void)
```

pit_start_oneshot - 让 channel 2 以 mode 0 从 0xFFFF 开始倒计数

mode 0 (Interrupt on Terminal Count), 命令字 0xB0：
bits[7:6] = 10  (channel 2)
bits[5:4] = 11  (lo/hi byte)
bits[3:1] = 000 (mode 0)
bit[0]   = 0    (binary)
满量程 0xFFFF / 1.193182MHz ≈ 54.9ms。

**位置**：`driver/irq/lapic.c:160`

<a id="fn-driver-irq-lapic.c-173"></a>

### `pit_latch_read`

```c
static uint32_t pit_latch_read(void)
```

pit_latch_read - latch channel 2 并读回当前计数值

0x80 = latch channel 2；随后从数据口读两次得到 lo/hi。
全程只用 PIT 自己的端口, 不需要 port 0x61。

**位置**：`driver/irq/lapic.c:173`

<a id="fn-driver-irq-lapic.c-200"></a>

### `pit_poll_tsc_freq`

```c
static bool pit_poll_tsc_freq(uint64_t *out_hz)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

启动 PIT 单次计时并用 TSC 等待固定时长，读计数器估算 TSC 频率，读数不可信时告警并返回 false。

<details>
<summary>函数内备注（2 条）</summary>

**第 211 行**

> 计数器纹丝不动 → PIT 没在跑(gate 未拉高或设备不存在)

**第 217 行（行尾）**

> 走得太少, 量化误差会很大

</details>

**位置**：`driver/irq/lapic.c:200`

<a id="fn-driver-irq-lapic.c-239"></a>

### `pit_wait_10ms`

```c
static bool pit_wait_10ms(void)
```

pit_wait_10ms - 用 PIT 计数器轮询等待约 10ms(不依赖 port 0x61)

只在 TSC 频率未知时用于量 LAPIC 的 10ms 窗口。轮询周期受 port I/O
开销限制(每次 latch+读两次, 微秒量级), 精度足够做 10ms 量级的窗口。

**位置**：`driver/irq/lapic.c:239`

<a id="fn-driver-irq-lapic.c-268"></a>

### `_lapic_cpuid`

```c
static inline void _lapic_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
```

── TSC 频率探测 ─────────────────────────────────────────────

先试 CPUID, 再退回 PIT 实测：
- CPUID.15H 给出 TSC/晶振比值与晶振频率, 结果精确
- CPUID.16H 只给标称基频(MHz), 精度差但可用
- 两者都拿不到时用 pit_poll_tsc_freq() 实测

实测：QEMU 8.2 的 -cpu host 在 x86_64 客户机里把 15H/16H 全部置零
(宿主本来是 15H=[2,126,38400000], 16H.base=2400), 所以本项目的
QEMU 场景实际都会走 PIT 那条路；CPUID 这条留着给裸机 / 其它 VMM。

低于 TSC_MIN_PLAUSIBLE_HZ 的结果一律视为标定失败, 不再采用。

**位置**：`driver/irq/lapic.c:268`

<a id="fn-driver-irq-lapic.c-274"></a>

### `tsc_deadline_supported`

```c
static bool tsc_deadline_supported(void)
```

CPUID.01H:ECX.TSC_Deadline[bit 24]：LAPIC 能否用 IA32_TSC_DEADLINE

**位置**：`driver/irq/lapic.c:274`

<a id="fn-driver-irq-lapic.c-281"></a>

### `tsc_freq_from_cpuid`

```c
static bool tsc_freq_from_cpuid(uint64_t *out_hz)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

依次尝试 CPUID 0x15 与 0x16 叶推算 TSC 频率，均不可用时打印原始值并返回 false。

<details>
<summary>函数内备注（1 条）</summary>

**第 309 行**

> 报失败时连同原始 CPUID 值一起打, 便于判断是&quot;叶不存在&quot;还是&quot;值不可用&quot;

</details>

**位置**：`driver/irq/lapic.c:281`

<a id="fn-driver-irq-lapic.c-326"></a>

### `lapic_init`

```c
void lapic_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 LAPIC 任务优先级设为 0，写 SVR 使能 APIC 并设伪中断向量 0xFF，随后打印 ID 与版本。

<details>
<summary>函数内备注（2 条）</summary>

**第 328 行**

> 设置 Task Priority 为 0(接收所有中断)

**第 331 行**

> 设置 Spurious Vector Register：使能 APIC + 伪中断向量 0xFF

</details>

**位置**：`driver/irq/lapic.c:326`

<a id="fn-driver-irq-lapic.c-337"></a>

### `lapic_id`

```c
uint32_t lapic_id(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取 LAPIC ID 寄存器并右移 24 位返回 APIC ID。

**位置**：`driver/irq/lapic.c:337`

<a id="fn-driver-irq-lapic.c-342"></a>

### `lapic_wait_icr_idle`

```c
static void lapic_wait_icr_idle(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

轮询 ICR 低寄存器的投递状态位直到清零。

**位置**：`driver/irq/lapic.c:342`

<a id="fn-driver-irq-lapic.c-348"></a>

### `lapic_send_init`

```c
void lapic_send_init(uint32_t apic_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

等待 ICR 空闲后写目标 APIC ID 与 INIT 命令字，再等待投递完成。

<details>
<summary>函数内备注（1 条）</summary>

**第 352 行（行尾）**

> INIT, level assert, physical

</details>

**位置**：`driver/irq/lapic.c:348`

<a id="fn-driver-irq-lapic.c-356"></a>

### `lapic_send_sipi`

```c
void lapic_send_sipi(uint32_t apic_id, uint8_t vector)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

等待 ICR 空闲后写目标 APIC ID 与带向量的 STARTUP IPI，再等待投递完成。

<details>
<summary>函数内备注（1 条）</summary>

**第 360 行（行尾）**

> STARTUP IPI

</details>

**位置**：`driver/irq/lapic.c:356`

<a id="fn-driver-irq-lapic.c-364"></a>

### `lapic_send_ipi`

```c
void lapic_send_ipi(uint32_t apic_id, uint8_t vector)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

等 ICR 空闲后写目标 APIC ID，再写 fixed 投递、电平断言加向量的 ICR 低字并等待完成。

<details>
<summary>函数内备注（1 条）</summary>

**第 368 行**

> fixed delivery(000) | physical destination(0) | level assert(bit14)
> | edge trigger(bit15=0) | vector。bit14 必须置：QEMU 对电平断言位
> 检查得比真机严格, 不置时固定投递可能被静默丢弃。

</details>

**位置**：`driver/irq/lapic.c:364`

<a id="fn-driver-irq-lapic.c-379"></a>

### `lapic_timer_init`

```c
void lapic_timer_init(uint8_t vector)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

设分频与屏蔽，用 CPUID 或 PIT 标定 TSC 频率，支持则切 TSC-deadline 单次模式，否则校准并配周期模式。

<details>
<summary>函数内备注（12 条）</summary>

**第 381 行**

> 分频 = 16, 减少误差

**第 384 行**

> 先屏蔽 timer, 避免校准期间产生中断

**第 387 行**

> 确定 TSC 频率：先 CPUID, 失败才退回 PIT 计时反推。
> tsc_freq_hz == 0 表示两者都不可用, 此时 timer_counter_to_ns() 会
> 退化成按 g_system_ticks 算的粗粒度时钟(量级正确, 只是分辨率差),
> 比用一个错的值好得多。

**第 410 行**

> TSC-deadline 模式：不需要 LAPIC 计数器校准，直接用绝对 TSC 值武装。
> 定时器由 hrtimer 在 tick 和 sleep deadline 之间取最小值编程。

**第 416 行（行尾）**

> 先解除武装

**第 426 行**

> ── 校准：测量 10ms 内 LAPIC 计数器减少了多少 ──

**第 427 行（行尾）**

> 设置最大初始值开始倒计数

**第 431 行**

> 已知 TSC 频率：直接用它精确等 10ms, 不再依赖 PIT

**第 436 行**

> 没有可用的 TSC 频率：只能用 PIT 计数器尽力等一个 10ms 量级

**第 448 行**

> 计算每次 tick 中断所需的计数值
> TIMER_FREQUENCY_HZ = 中断频率 (如 100 Hz → 10ms/tick)
> ticks_per_interrupt = ticks_in_10ms * 10ms / (1000ms / TIMER_FREQUENCY_HZ)
> 即: ticks_per_interrupt = ticks_in_10ms * TIMER_FREQUENCY_HZ / 100

**第 455 行（行尾）**

> 保底

**第 459 行**

> 配置 Periodic 模式 + 向量

</details>

**位置**：`driver/irq/lapic.c:379`

<a id="fn-driver-irq-lapic.c-464"></a>

### `lapic_timer_oneshot_supported`

```c
bool lapic_timer_oneshot_supported(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回本 CPU 记录的 LAPIC 定时器是否处于单次模式。

**位置**：`driver/irq/lapic.c:464`

<a id="fn-driver-irq-lapic.c-469"></a>

### `lapic_timer_program_ns`

```c
void lapic_timer_program_ns(uint64_t deadline_ns)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

非单次模式直接返回；deadline 为 0 则解除武装，否则把纳秒差换算成 TSC 拍数并写 TSC_DEADLINE。

<details>
<summary>函数内备注（1 条）</summary>

**第 482 行（行尾）**

> 至少 1us，避免写进去时已经过期

</details>

**位置**：`driver/irq/lapic.c:469`

<a id="fn-driver-irq-lapic.c-494"></a>

### `lapic_eoi`

```c
void lapic_eoi(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

向 LAPIC EOI 寄存器写 0 结束当前中断。

**位置**：`driver/irq/lapic.c:494`

<a id="fn-driver-irq-lapic.c-501"></a>

### `lapic_timer_stop`

```c
void lapic_timer_stop(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

屏蔽 LVT 定时器，若处于单次模式再把 TSC_DEADLINE 写 0 解除武装。

**位置**：`driver/irq/lapic.c:501`

<a id="fn-driver-irq-lapic.c-510"></a>

### `lapic_dispatch`

```c
static void lapic_dispatch(void *frame)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从异常帧取向量，0xFF 伪中断直接返回，否则分发中断后再写 EOI。

<details>
<summary>函数内备注（1 条）</summary>

**第 515 行**

> 0xFF = spurious：没有 in-service 位，不能写 EOI

</details>

**位置**：`driver/irq/lapic.c:510`

<a id="fn-driver-irq-lapic.c-523"></a>

### `lapic_enable_api`

```c
static int lapic_enable_api(unsigned irq, bool on)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

仅接受 GSI 1 到 15 对应的外部向量，经 IOAPIC 重定向表设置目标与屏蔽位。

<details>
<summary>函数内备注（1 条）</summary>

**第 525 行**

> 外部设备线：irq 是 IDT 向量，GSI = irq - IDT_IRQ_BASE。
> 向量 0x20 是 LAPIC timer，0x21-0x2F 对应 GSI 1-15。
> LAPIC 自身的 timer/IPI 不需要走这里。

</details>

**位置**：`driver/irq/lapic.c:523`

<a id="fn-driver-irq-lapic.c-543"></a>

### `lapic_set_priority_api`

```c
static int lapic_set_priority_api(unsigned irq, unsigned prio)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

忽略参数恒返回 0，不修改任何硬件状态。

**位置**：`driver/irq/lapic.c:543`

<a id="fn-driver-irq-lapic.c-550"></a>

### `lapic_secondary_init_api`

```c
static int lapic_secondary_init_api(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

直接返回 0，从核 IDT 与 LAPIC 已由异常初始化完成。

<details>
<summary>函数内备注（1 条）</summary>

**第 552 行**

> exception_init_secondary() 已经重建 IDT 并 lapic_init()

</details>

**位置**：`driver/irq/lapic.c:550`

<a id="fn-driver-irq-lapic.c-556"></a>

### `lapic_send_ipi_api`

```c
static int lapic_send_ipi_api(uint32_t hw_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用重调度 IPI 向量向指定硬件 ID 发送 IPI 并返回 0。

**位置**：`driver/irq/lapic.c:556`

<a id="fn-driver-irq-lapic.c-562"></a>

### `lapic_register_ipi_api`

```c
static int lapic_register_ipi_api(irq_handler_fn fn, void *ctx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把处理函数注册到重调度 IPI 的 IDT 向量并返回其结果。

<details>
<summary>函数内备注（1 条）</summary>

**第 564 行**

> IDT 门由 exception_setup_idt() 装好，这里只挂 C handler

</details>

**位置**：`driver/irq/lapic.c:562`

<a id="fn-driver-irq-lapic.c-568"></a>

### `lapic_register_timer_api`

```c
static int lapic_register_timer_api(irq_handler_fn fn, void *ctx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把处理函数注册到 LAPIC 定时器的 IDT 向量并返回其结果。

**位置**：`driver/irq/lapic.c:568`

<a id="fn-driver-irq-lapic.c-573"></a>

### `lapic_cpu_hw_id_api`

```c
static uint64_t lapic_cpu_hw_id_api(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 lapic_id() 读到的本核 APIC ID。

**位置**：`driver/irq/lapic.c:573`

<a id="fn-driver-irq-lapic.c-590"></a>

### `lapic_probe`

```c
static int lapic_probe(const device_t *dev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按设备配置设置 IOAPIC 基址（无配置时置 0），绑定 LAPIC 操作集并初始化 LAPIC。

<details>
<summary>函数内备注（1 条）</summary>

**第 594 行**

> LAPIC 基址固定（exception_init() 已使能）；IOAPIC 基址由平台配置
> 提供。没有 IOAPIC 的平台保持 0，irq_enable() 对外部线返回 -1。

</details>

**位置**：`driver/irq/lapic.c:590`

<a id="mod-driver-irq-plic.c"></a>

## driver/irq/plic.c

`driver/irq/plic.c` · 19 个函数 · 19 个有说明 · 260 行

<a id="fn-driver-irq-plic.c-42"></a>

### `plic_read32`

```c
static inline uint32_t plic_read32(uintptr_t off)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取 PLIC 基址加偏移处的 32 位寄存器值并返回。

**位置**：`driver/irq/plic.c:42`

<a id="fn-driver-irq-plic.c-47"></a>

### `plic_write32`

```c
static inline void plic_write32(uintptr_t off, uint32_t val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把值写入 PLIC 基址加偏移处的 32 位寄存器。

**位置**：`driver/irq/plic.c:47`

<a id="fn-driver-irq-plic.c-52"></a>

### `plic_context_base`

```c
static inline uintptr_t plic_context_base(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

用上下文步长乘以 S 模式上下文序号计算上下文寄存器基偏移。

**位置**：`driver/irq/plic.c:52`

<a id="fn-driver-irq-plic.c-63"></a>

### `plic_dispatch`

```c
static void plic_dispatch(void *frame)
```

plic_dispatch - 外部中断的 claim → handler → complete

未注册的源线由 irq_dispatch() 统一采样报警；这里必须无条件 complete,
否则 level-high 源会把 PLIC 挂死(整机卡在 ISR 里)。

**位置**：`driver/irq/plic.c:63`

<a id="fn-driver-irq-plic.c-74"></a>

### `plic_external_irq`

```c
static void plic_external_irq(void *frame)
```

S 模式外部中断的 scause 钩子：真正的 claim/complete 在 plic_dispatch

**位置**：`driver/irq/plic.c:74`

<a id="fn-driver-irq-plic.c-79"></a>

### `plic_ipi_entry`

```c
static void plic_ipi_entry(void *frame)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

有注册回调则先调用它，随后清除 sip 的 SSIE 位避免电平 pending 重复陷入。

<details>
<summary>函数内备注（1 条）</summary>

**第 84 行**

> SSIP 是电平型 pending 位，不清会在返回后立刻再次陷入

</details>

**位置**：`driver/irq/plic.c:79`

<a id="fn-driver-irq-plic.c-88"></a>

### `plic_timer_entry`

```c
static void plic_timer_entry(void *frame)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

若已注册定时器回调则用保存的上下文调用它。

**位置**：`driver/irq/plic.c:88`

<a id="fn-driver-irq-plic.c-94"></a>

### `plic_init`

```c
void plic_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

已初始化则返回；取 PLIC 基址，成功则设阈值为 0、安装外部中断处理并开启 SEIE。

<details>
<summary>函数内备注（1 条）</summary>

**第 101 行**

> 平台事实, 一次性, 而且它回答了&quot;为什么没有外部中断&quot; —— 这是操作者
> 需要看到的信息, 不是警告。

</details>

**位置**：`driver/irq/plic.c:94`

<a id="fn-driver-irq-plic.c-115"></a>

### `plic_enable_irq`

```c
void plic_enable_irq(uint32_t irq, uint32_t priority)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

必要时先初始化，校验中断号后以给定优先级（0 视为 1）写优先级并置位使能位。

**位置**：`driver/irq/plic.c:115`

<a id="fn-driver-irq-plic.c-134"></a>

### `plic_disable_irq`

```c
void plic_disable_irq(uint32_t irq)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验基址与中断号后清除对应使能寄存器中的该中断位。

**位置**：`driver/irq/plic.c:134`

<a id="fn-driver-irq-plic.c-145"></a>

### `plic_write_priority`

```c
static void plic_write_priority(uint32_t irq, uint32_t prio)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验中断号，把 0 改成 1 后写入该中断的优先级寄存器。

**位置**：`driver/irq/plic.c:145`

<a id="fn-driver-irq-plic.c-156"></a>

### `plic_enable_api`

```c
static int plic_enable_api(unsigned irq, bool on)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

中断号非法返回 -2；关闭时直接禁用，开启时确保 PLIC 初始化并用保存的优先级使能。

**位置**：`driver/irq/plic.c:156`

<a id="fn-driver-irq-plic.c-175"></a>

### `plic_set_priority_api`

```c
static int plic_set_priority_api(unsigned irq, unsigned prio)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验中断号与基址，保存优先级到数组并写入硬件优先级寄存器。

**位置**：`driver/irq/plic.c:175`

<a id="fn-driver-irq-plic.c-190"></a>

### `plic_send_ipi_api`

```c
static int plic_send_ipi_api(uint32_t hw_id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

hw_id 不小于 64 时返回 -2，否则经 SBI ecall 向目标核发送 IPI，按返回寄存器判成败。

**位置**：`driver/irq/plic.c:190`

<a id="fn-driver-irq-plic.c-203"></a>

### `plic_register_ipi_api`

```c
static int plic_register_ipi_api(irq_handler_fn fn, void *ctx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

保存 IPI 回调与上下文，安装 S 模式软件中断入口并使能 SSIE。

**位置**：`driver/irq/plic.c:203`

<a id="fn-driver-irq-plic.c-212"></a>

### `plic_register_timer_api`

```c
static int plic_register_timer_api(irq_handler_fn fn, void *ctx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

保存定时器回调与上下文，把定时器入口安装到 S 模式定时器 cause。

**位置**：`driver/irq/plic.c:212`

<a id="fn-driver-irq-plic.c-220"></a>

### `plic_timer_enable_api`

```c
static int plic_timer_enable_api(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空实现恒返回 0，定时器中断使能由 timer_arch_enable 写 sie.STIE 完成。

<details>
<summary>函数内备注（1 条）</summary>

**第 222 行**

> timer_arch_enable() 自己写 sie.STIE；AP 的 timer_init_secondary 也走它

</details>

**位置**：`driver/irq/plic.c:220`

<a id="fn-driver-irq-plic.c-226"></a>

### `plic_debug_dump_api`

```c
static void plic_debug_dump_api(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

PLIC 已初始化时读取当前阈值，打印基址与阈值供 SMP 调试。

**位置**：`driver/irq/plic.c:226`

<a id="fn-driver-irq-plic.c-246"></a>

### `plic_probe`

```c
static int plic_probe(const device_t *dev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

忽略设备参数，把 PLIC 操作集绑定为中断后端并调用 plic_init()。

<details>
<summary>函数内备注（1 条）</summary>

**第 248 行（行尾）**

> 阶段 1：plic_init() 仍直接读平台配置

</details>

**位置**：`driver/irq/plic.c:246`

<a id="mod-driver-npu-npu_core.c"></a>

## driver/npu/npu_core.c — NPU 服务注册点

`driver/npu/npu_core.c` · 1 个函数 · 1 个有说明 · 22 行

永远参与构建。NPU=rknpu 时 driver/npu/rknpu.c 的 probe 调 npu_bind()；
否则 g_npu_ops 保持 NULL，include/driver/npu.h 的内联包装统一返回
失败/空操作。

<a id="fn-driver-npu-npu_core.c-19"></a>

### `npu_bind`

```c
void npu_bind(const npu_ops_t *ops)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

保存传入的 NPU 操作表指针供核心层调用。

**位置**：`driver/npu/npu_core.c:19`

<a id="mod-driver-npu-npu_mmu.c"></a>

## driver/npu/npu_mmu.c — driver/npu/npu_mmu.c

`driver/npu/npu_mmu.c` · 14 个函数 · 14 个有说明 · 540 行

RK3588 NPU 的 IOMMU(rockchip,iommu-v2)—— 宿主独占实现。
为什么要有它, 以及为什么不能让 guest 碰, 见 include/npu/npu_mmu.h 顶上那段。

寄存器与页表格式抄自 mainline drivers/iommu/rockchip-iommu.c(原生支持 v2)。
这里只实现我们需要的那一小块：建表, 填表, 开/关翻译, 整表 TLB 失效 ——
没有 domain 抽象, 没有缺页中断处理, 没有 stall。

缺页为什么不处理：提交前我们会把整段 guest RAM 里已被 stage-2 映射的页
全部填进去(npu_mmu_impl_sync), 所以 NPU 会碰到的页都已经在了。真出现缺页说明
它访问了一个 guest 压根没映射过的地址 —— 那是 guest 那边的问题, 宁可让它
翻译失败(表里无效项 → 硬件报 BUS_ERROR), 也不要宿主替它猜。

<a id="fn-driver-npu-npu_mmu.c-84"></a>

### `bank_reg`

```c
static inline volatile void *bank_reg(int bank, uint32_t off)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

由指定 bank 的基址加偏移返回对应寄存器指针。

**位置**：`driver/npu/npu_mmu.c:84`

<a id="fn-driver-npu-npu_mmu.c-89"></a>

### `mmu_write`

```c
static inline void mmu_write(int bank, uint32_t off, uint32_t val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

向指定 bank 的偏移处写 32 位寄存器值。

**位置**：`driver/npu/npu_mmu.c:89`

<a id="fn-driver-npu-npu_mmu.c-94"></a>

### `mmu_read`

```c
static inline uint32_t mmu_read(int bank, uint32_t off)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从指定 bank 的偏移处读 32 位寄存器值。

**位置**：`driver/npu/npu_mmu.c:94`

<a id="fn-driver-npu-npu_mmu.c-100"></a>

### `mmu_cmd_all`

```c
static void mmu_cmd_all(uint32_t cmd)
```

命令要写进每一组寄存器 —— 否则另外三组不知道表换了

**位置**：`driver/npu/npu_mmu.c:100`

<a id="fn-driver-npu-npu_mmu.c-106"></a>

### `npu_mmu_impl_init`

```c
int npu_mmu_impl_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按内置映射表从平台配置取四个 MMU bank 基址，缺失即返回 -1，全部取到后置就绪标志并打印。

<details>
<summary>函数内备注（5 条）</summary>

**第 112 行（行尾）**

> FDAB9000

**第 113 行（行尾）**

> FDABA000

**第 114 行（行尾）**

> FDACA000

**第 115 行（行尾）**

> FDADA000

**第 132 行（行尾）**

> 本平台没有 NPU

</details>

**位置**：`driver/npu/npu_mmu.c:106`

<a id="fn-driver-npu-npu_mmu.c-141"></a>

### `pt_for`

```c
static int pt_for(uint64_t ipa, int alloc)
```

取(必要时分配)ipa 所在的那张 PT, 返回它的下标；失败返回 -1

**位置**：`driver/npu/npu_mmu.c:141`

<a id="fn-driver-npu-npu_mmu.c-166"></a>

### `pte_set`

```c
static inline void pte_set(npu_pt_t *t, uint64_t ipa, uint64_t pa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 ipa 页号位置写入一条可写 PTE，首次填充该槽时递增已填计数。

<details>
<summary>函数内备注（1 条）</summary>

**第 172 行（行尾）**

> 可写：NPU 要把结果写回去

</details>

**位置**：`driver/npu/npu_mmu.c:166`

<a id="fn-driver-npu-npu_mmu.c-175"></a>

### `npu_mmu_impl_enable`

```c
int npu_mmu_impl_enable(uint64_t ipa_base, uint64_t ipa_size)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

释放旧页表重建目录并关自动门控，回读校验写入各 bank 的目录地址，再发使能命令并轮询确认翻译已开启。

<details>
<summary>函数内备注（5 条）</summary>

**第 180 行**

> 换区间 = 换表：把目录和 PT 全部作废重来。别想着增量复用 ——
> 上一轮的 PT 留着会把已经不属于本 VM 的物理页继续映射着。

**第 197 行**

> AUTO_GATING 必须关：空闲时它会自作主张省电, 翻译随机失效 ——
> 症状是&quot;大部分时候对, 偶尔读到别人的页&quot;, 比彻底坏掉难查得多。

**第 206 行**

> 回读确认。mainline 里就有这个检查 —— 说明硬件会不认账(写入被
> 丢弃), 而&quot;表地址没生效&quot;的症状是全部翻译失败或翻到旧表,
> 从别处完全看不出来。

**第 223 行**

> 注意： 必须回读 STATUS 确认翻译真的开了。

> mainline 的 rk_iommu_enable_paging() 发完命令就 `readx_poll_timeout`
> 等 PAGING_ENABLED 置位, 超时报错 —— 说明这个命令可能不生效。
> 而&quot;没生效&quot;和&quot;生效了&quot;从外面看一模一样：MMU 关着时 NPU 按物理地址
> 访问, 我们把 IPA→PA 的表建得再对也没用, 它照样什么都读不到
> (真机实测 raw status = 0xc0000000, 和&quot;NPU 没开工&quot;完全一致)。

> 第一版就是发完命令就走, 于是连&quot;到底是表错了还是根本没开翻译&quot;
> 都分不出来。

**第 257 行**

> 把四组寄存器的实际状态打出来 —— 这一行本来就有, 只是一直没被调用过。
> 它就是&quot;翻译到底开没开, 有没有缺页&quot;的直接判据。

</details>

**位置**：`driver/npu/npu_mmu.c:175`

<a id="fn-driver-npu-npu_mmu.c-264"></a>

### `npu_mmu_impl_disable`

```c
void npu_mmu_impl_disable(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

发送关闭分页命令，释放全部二级页表，清空目录并重置记录的 IPA 区间。

<details>
<summary>函数内备注（1 条）</summary>

**第 269 行**

> 注意： 必须真的关掉：宿主自己驱动 NPU 时用的是物理地址,
> 翻译还开着的话, 它写的每一个地址都会被再翻译一次 → 全错。

</details>

**位置**：`driver/npu/npu_mmu.c:264`

<a id="fn-driver-npu-npu_mmu.c-295"></a>

### `npu_mmu_impl_sync`

```c
int npu_mmu_impl_sync(vm_t *vm)
```

把 stage-2 的映射镜像进来。

遍历的是整段 guest RAM(不是 NPU 窗口)—— 因为要修的正是"缓冲落在
窗口外"这件事：现在 NPU 能到哪儿由这张表决定, 不再由恒等窗口决定。

只填 stage-2 里确实存在的项：guest 没碰过的页本来就不该让 NPU 访问到。
一次 192 MiB 的 guest RAM 是 49152 次 stage2_lookup, 微秒量级, 可以接受。

<details>
<summary>函数内备注（10 条）</summary>

**第 299 行（行尾）**

> 为 NPU 补进 stage-2 的页数

**第 308 行**

> 注意： stage2_lookup 命中返回 1, 未映射返回 0(见它的声明)——
> 不是&quot;0 表示成功&quot;。我按后者写过一版, 结果把所有已映射的页
> 全跳过了, 表一个项都没填, NPU 连 regcmd 都读不到
> (raw status 从&quot;跑完了&quot;的 0xc0000151 掉回&quot;什么都没干&quot;的
> 0xc0000000)。而日志里只是少了一行, 从别处完全看不出来。
> el2_run.c 那边不看返回值, 只查 pa, 所以没暴露过这个问题。

**第 316 行**

> NPU 要读的页, guest 的 CPU 未必碰过：没写过的输出缓冲,
> 由 DMA 填进去的权重, 都是驱动分配完就直接交给硬件的。

> 这种页在 stage-2 里还没有映射。MMU 关着时这不成问题 —— NPU 按
> 物理地址硬读, 读到垃圾但访问&quot;成功&quot;, 作业照跑(这也是为什么
> 之前 raw status 是 0xc0000151)。开了 MMU 之后同样的访问变成
> 硬缺页, 作业当场挂(0xc0000000)。真机实测抓到了：

> bank1 STATUS=0x0000018b  翻译开 缺页(读)

> 所以这里给它补上。注意： 补进 stage-2, 不是补进我这张表 ——
> stage-2 是唯一的真值：我这张表要是自己分配页, guest 之后自己
> 缺页时会拿到另一个页, 两边永久不一致, 而且两边都不报错。

**第 332 行（行尾）**

> 清零：NPU 读之前不该有内容

**第 334 行**

> 重新查一次拿 PA —— 不看 stage2_map_page 的返回值,
> 它的约定和上面 stage2_lookup 那个坑是同一类。

**第 339 行（行尾）**

> 补不上(PMM 不够), 这一页只能让 NPU 缺页

**第 358 行**

> 注意：注意： 表页必须冲缓存 —— 这是整件事最关键的一步。

> `g_mmu_dir` 和 PT 都是普通可缓存内存, CPU 写它们走的是 cache。
> 而 NPU 的 MMU 是从内存里取表的(设备的总线读不会来看 CPU 的
> cache)—— 不冲的话它看到的是写之前的内容。

> 这正是 dma_alloc_coherent 存在的理由, 也是这类设备驱动里最容易漏的
> 一步。真机症状极具迷惑性：

> · 缺页地址在同一页内漂移(0x79711540 → 0x79711840 → 0x79711640,
> 三次是三个不同的值)。缓存里哪些行碰巧被回写了, 每次不同 →
> MMU 能解出来的项也不同 → 缺的地址就乱跳。
> · 探针说&quot;表是对的&quot;, 硬件说&quot;缺页&quot; —— 因为探针读的是缓存里的新值,
> 硬件读的是内存里的旧值。两边都没撒谎, 它们看的不是同一份东西。

> 顺序：先冲表(让 MMU 能看到), 再 ZAP(让 IOTLB 忘掉旧的), 后清故障。

**第 382 行**

> dsb：确保真的落到内存 —— 只 clean 不屏障, NPU 仍可能先读到

**第 387 行**

> 只在真有改动时失效 —— 一次作业几千页, 每页一次 TLB 失效不可接受,
> 整表失效一次则几乎无代价。

**第 394 行**

> 每次都报(前两次), 而不是&quot;有变化才报&quot;。

> 上面那个反向判断的 bug, 症状就是&quot;日志里少一行&quot; —— 而&quot;少一行&quot;和
> &quot;还没跑到&quot;在日志上长得一模一样。把计数打出来, 0 就是 0。

</details>

**位置**：`driver/npu/npu_mmu.c:295`

<a id="fn-driver-npu-npu_mmu.c-421"></a>

### `npu_mmu_impl_probe`

```c
void npu_mmu_impl_probe(vm_t *vm, uint64_t ipa)
```

npu_mmu_impl_probe — 拿一个具体地址对账：我的表翻到哪 vs stage-2 翻到哪。

为什么需要它：池那块内存本来就是恒等映射的(PA == IPA), 所以 NPU 的表里
那条翻译应当和 stage-2 一模一样, 开 MMU 对 regcmd 应当是个 no-op。
真机上开了之后反而从"能跑"(0xc0000151)掉回"什么都没干"(0xc0000000)——
说明这条翻译不是恒等的, 我表里那个地址翻到别处去了。

stage2 与 PTE 指向同一个页  → 表是对的, NPU 的访问没走这个 MMU
两者不同                    → 表错了, 对着两个 PA 就能定位是哪一步

**位置**：`driver/npu/npu_mmu.c:421`

<a id="fn-driver-npu-npu_mmu.c-454"></a>

### `npu_mmu_impl_clear_fault`

```c
void npu_mmu_impl_clear_fault(void)
```

清掉粘住的缺页(写 PAGE_FAULT_DONE)。四组都要清 —— 缺页是按 bank 记的。
mainline 在缺页中断处理里就是这么收尾的。

<details>
<summary>函数内备注（2 条）</summary>

**第 459 行**

> 注意： 顺序不能反：先冲 IOTLB, 再清故障。

> mainline 在缺页中断里就是这两行, 这个顺序(rockchip-iommu.c:655-656),
> 上面那句注释写得很直白：
> &quot;we always zap cache and clear the page fault anyway&quot;

> 因为 IOTLB 里缓存着那次失败的翻译。只清故障状态的话, 那条失败的
> 表项还在, MMU 立刻照着它重新报同一个缺页 —— 表里明明已经补好了也没用。

> 真机实测就是这个症状：缺页地址始终是同一个 0x79711540,
> 而我的表里那条翻译是对的(PTE=0x79711007 -&gt; pa=0x79711000,
> 和 stage-2 一致)。清一次, 冲一次, 它才肯重新查表。

> 我第一版只写了 PAGE_FAULT_DONE, 而且放在同步之前 —— 顺序正好颠倒。

**第 478 行**

> 注意： 还要解除 stall：缺页会把 MMU 置于 stall, 而分页在 stall 期间
> 是停着的。mainline 的 DISABLE_STALL 注释就是 &quot;Stop stall re-enables
> paging&quot; —— 清了故障不解除 stall, 分页仍然不动。

> 真机实测的两个现象合起来指向这里：把清故障修对之后, 缺页地址动了
> (0x79711540 → 0x79711840), 说明 MMU 确实往前走了；但新地址仍然缺页,
> 而它和旧地址在同一个 4 KiB 页里, 那一页探针刚确认映射正确。
> → 缺的不是翻译, 是&quot;它还在 stall 里没恢复&quot;。

</details>

**位置**：`driver/npu/npu_mmu.c:454`

<a id="fn-driver-npu-npu_mmu.c-491"></a>

### `npu_mmu_impl_any_fault`

```c
bool npu_mmu_impl_any_fault(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

逐个 bank 检查状态寄存器的缺页激活位，任一置位即返回 true。

**位置**：`driver/npu/npu_mmu.c:491`

<a id="fn-driver-npu-npu_mmu.c-503"></a>

### `npu_mmu_impl_dump`

```c
void npu_mmu_impl_dump(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

打印 IPA 区间与各 bank 的目录地址、状态及翻译/缺页标志，缺页时读取并打印缺页地址。

<details>
<summary>函数内备注（1 条）</summary>

**第 523 行**

> 注意： 这个寄存器一直只定义, 没读过 —— 而它直接记着出错的那个地址。
> 表全填满之后仍然报缺页, 那缺的地址就一定在 guest RAM 之外,
> 读出来就知道 NPU 到底在要什么。

</details>

**位置**：`driver/npu/npu_mmu.c:503`

<a id="mod-driver-npu-rknpu.c"></a>

## driver/npu/rknpu.c — driver/npu/rknpu.c

`driver/npu/rknpu.c` · 21 个函数 · 21 个有说明 · 1033 行

Rockchip RK3588 NPU 驱动

初始化流程：
1. 通过 rkpm 上电四个 NPU 电源域
2. 清除遗留中断状态
3. 读取并验证版本寄存器(魔数 0x46495245 = "FIRE")
4. 向 GICv3 注册 NPU0 IRQ 处理程序并使能中断

中断处理：
读本核 INT_STATUS → 与期望的 fuzz 掩码比对 → 记下 PC_TASK_STATUS 的完成数
→ 清本核 INT_CLEAR。三个核各有独立中断线(142/143/144)。

框架集成：
- 通过 platform_get_mmio("npu", "base0") 获取 MMIO 基地址
(不能用 platform_get_uintptr —— 那是裸物理地址, 见 rknpu_init 里的说明)
- 通过 platform_get_uintptr("npu", "irq0") 获取中断号(中断号不是地址)
- 由平台 C 初始化序列在 drivers 阶段调用

<a id="fn-driver-npu-rknpu.c-111"></a>

### `cru_xlate`

```c
static uintptr_t cru_xlate(uintptr_t pa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

平台 MMIO 需要 VMA 时把物理地址加上 KERNEL_VMA，否则原样返回。

**位置**：`driver/npu/rknpu.c:111`

<a id="fn-driver-npu-rknpu.c-116"></a>

### `cru_read`

```c
static inline uint32_t cru_read(uintptr_t pa)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 CRU 物理地址换算后读取 32 位寄存器值。

**位置**：`driver/npu/rknpu.c:116`

<a id="fn-driver-npu-rknpu.c-122"></a>

### `rknpu_dump_cru`

```c
static void rknpu_dump_cru(void)
```

一次性把 NPU 相关 gate/reset/mux 打出来(只读, 不改任何状态)

<details>
<summary>函数内备注（1 条）</summary>

**第 137 行**

> 逐条判定。0 = 时钟开 / 已出复位, 1 = 关着 / 还在复位。
> bit 位置来自参考项目 src/dev/cru.c。

</details>

**位置**：`driver/npu/rknpu.c:122`

<a id="fn-driver-npu-rknpu.c-149"></a>

### `npu_read`

```c
static inline uint32_t npu_read(int core, uint32_t off)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取指定 NPU 核心基址加偏移处的 32 位寄存器。

**位置**：`driver/npu/rknpu.c:149`

<a id="fn-driver-npu-rknpu.c-154"></a>

### `npu_write`

```c
static inline void npu_write(int core, uint32_t val, uint32_t off)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

向指定 NPU 核心基址加偏移处写入 32 位寄存器值。

**位置**：`driver/npu/rknpu.c:154`

<a id="fn-driver-npu-rknpu.c-177"></a>

### `rknpu_fuzz_status`

```c
static inline uint32_t rknpu_fuzz_status(uint32_t status)
```

rknpu_fuzz_status —— 把状态按 2 位一组取饱和：每对位里只要有一个非零,
就把这对位整体置满, 其余位丢掉。

为什么需要它：我们比的是"哪些块报了中断", 不是状态的原始位。低 12 位里
含有会随作业进度变化的部分, 直接拿 status 去比 int_mask 会永远不等 ——
症状是"作业明显跑完了, 却报 invalid irq status", 日志完全不指向真因。
按 2 位一组取饱和就等于只看"这一组有没有报", 把变化的部分吸收掉。

与参考项目 testos-reflector/src/npu/rknpu.c 的实现逐行一致(真机验证过)。

注意： 参考项目注释里说低 12 位是"任务计数" —— 这个说法在本内核上没有验证过,
而且和实测矛盾：INT_MASK 只放行 bit8/9, 若 status 真带 bit10/11, fuzz 会
变成 0xF00, 与 expect=0x300 不等而报错, 但真机上从不报错。所以别把低 12 位
当计数器用(要用完成数请读 PC_TASK_STATUS = 0x3C)。

**位置**：`driver/npu/rknpu.c:177`

<a id="fn-driver-npu-rknpu.c-223"></a>

### `rknpu_irq_common`

```c
static void rknpu_irq_common(int core)
```

三个核的中断线是独立的(142/143/144), 各自的 ISR 只处理自己的核。
公共 IRQ 接口已经能带 ctx, 但 NPU 的核号在编译期固定、且每核 ISR 还会
打印自己的 IRQ 号, 保留三个薄包装比在 ctx 里塞核号更直白。

<details>
<summary>函数内备注（6 条）</summary>

**第 226 行**

> 注意： INT_STATUS 的低 12 位是状态位, 不是完成计数 —— 别当计数器用。
> (想拿完成数得读 PC_TASK_STATUS=0x3C, 但它作业一完成就被硬件清零,
> 只有作业卡住时才有值。详见 g_core_status 的说明。)

**第 234 行**

> 存本核 INT_STATUS 原值 —— 这是&quot;这个核报了中断&quot;的直接证据。
> 不报任务完成数：那个数在作业完成后读不到, 理由见 g_core_status 的说明。

**第 239 行**

> 每个核第一次收到中断时报一行(一次启动共 3 行)。

> 这是&quot;三个核都在工作&quot;最直白的证据：中断号是那个核专属的 GIC SPI
> (142/143/144), 跑的是它自己的 handler, status 是从它自己的
> INT_STATUS 读的。core1/core2 这两行出现, 就说明它们各自跑完了活,
> 拉起了自己的中断线 —— 而不是&quot;我们写了寄存器, 硬件其实没动&quot;。

**第 254 行**

> 不匹配才打：这是真异常, 且要看清 raw 与计数

**第 268 行**

> 清除本核的中断位

**第 271 行**

> 通知 GICv3 完成(EOI 由异常框架在 irq_handler 返回后写入)

</details>

**位置**：`driver/npu/rknpu.c:223`

<a id="fn-driver-npu-rknpu.c-274"></a>

### `rknpu_irq_handler0`

```c
static void rknpu_irq_handler0(void *frame, void *ctx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

忽略中断帧与上下文参数，转调核心 0 的公共中断处理。

**位置**：`driver/npu/rknpu.c:274`

<a id="fn-driver-npu-rknpu.c-281"></a>

### `rknpu_irq_handler1`

```c
static void rknpu_irq_handler1(void *frame, void *ctx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

忽略中断帧与上下文参数，转调核心 1 的公共中断处理。

**位置**：`driver/npu/rknpu.c:281`

<a id="fn-driver-npu-rknpu.c-288"></a>

### `rknpu_irq_handler2`

```c
static void rknpu_irq_handler2(void *frame, void *ctx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

忽略中断帧与上下文参数，转调核心 2 的公共中断处理。

**位置**：`driver/npu/rknpu.c:288`

<a id="fn-driver-npu-rknpu.c-302"></a>

### `rknpu_validate_version`

```c
bool rknpu_validate_version(int core)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取并打印版本与版本号寄存器，版本不等于预期魔数时打错误并返回 false。

**位置**：`driver/npu/rknpu.c:302`

<a id="fn-driver-npu-rknpu.c-318"></a>

### `rknpu_init`

```c
void rknpu_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从平台配置取三核基址与中断号，开启 NPU 各电源域，逐核清中断、校验版本、注册并启用 IRQ，最后记录 IOMMU 并置就绪。

<details>
<summary>函数内备注（8 条）</summary>

**第 322 行**

> 1. 从平台配置获取运行时参数

**第 323 行**

> 注意： 必须用 platform_get_mmio(= 原始值 + KERNEL_VMA, 当 platform.conf
> 的 mmio_vma 为真时), 不能用 platform_get_uintptr 拿裸物理地址。

> 踩过的坑(rk3588 真机, 2026-10)：init 阶段读版本寄存器一切正常,
> 因为 boot 页表里有 MMIO 的恒等映射；等 busybox 跑起来, CPU 切到
> 用户进程的页表(create_uvm 建的)之后恒等映射就没了, 于是第一次
> SUBMIT 写 PC_DATA_ADDR 直接
> EL1 exception EC=0x25 ESR=0x96000045 FAR=0xfdab0010
> DFSC=0x5 (translation fault)
> ——同一个 4 KiB 页, init 能读, submit 写就炸, 看着完全不像缺映射。
> QEMU 上测不出来：那里 npu.base0 为 0, 本函数早就 return 了。

> GICv3 一直是对的(用的就是 platform_get_mmio), 照它来。

**第 359 行**

> 2. 上电 NPU 电源域

**第 361 行**

> 同样要走 VMA —— 理由见上面 base0 那段

**第 364 行**

> 回退到硬编码地址；它也是物理地址, 同样要按平台规则换算

**第 389 行**

> 3. 逐核：清遗留中断 + 校验版本 + 注册中断

**第 407 行**

> 3b. 只读地看一眼 NPU 时钟门控 —— 版本寄存器读得出来只证明 APB 通,
> &quot;作业不启动&quot;最像的就是核心/AXI 时钟还关着。见 rknpu_dump_cru()。
> 三个核共用一组门控, 看一次就够。

**第 413 行**

> 记下 NPU IOMMU 的四组寄存器位置。只记位置, 不开翻译 ——
> 宿主自己驱动 NPU 时走的是物理地址, 翻译必须关着；
> 等 guest 接管时 vnpu_init 才调 npu_mmu_enable()。

</details>

**位置**：`driver/npu/rknpu.c:318`

<a id="fn-driver-npu-rknpu.c-443"></a>

### `rknpu_impl_host_release`

```c
void rknpu_impl_host_release(void)
```

rknpu_impl_host_release —— 把 NPU 让给 guest(VMM 透传用)。

为什么必须做：本驱动在 boot 时装了 142/143/144 的 ISR 并在 GIC 里使能
了它们。guest 提交作业 → NPU 拉高物理线 → 那条线是宿主的 GIC 在监视,
于是宿主的 ISR 抢先跑掉, 而它的正常收尾就是写 INT_CLEAR —— guest 的中断
还没进 vGIC 就被擦掉了。症状是 guest 里驱动一路 probe 正常, 任务提交也
正常, 然后永远等不到中断直到超时, 且日志里一行错误都没有。

这是"两个驱动等同一个中断"的经典局面, 谁先 ack 谁赢, 没有仲裁。
所以让者主动退出：关掉宿主 GIC 上这三条线(线还挂着, 只是不再投递给
宿主 CPU), 再把寄存器留给 guest。中断之后由 VMM 在每次进入 guest 前
轮询 INT_STATUS 并按电平语义注入 vGIC(见 vmm_vnpu.h / el2_run.c)。

g_npu_ready 也一并置回 false：宿主侧 rknpu_submit_task 从此拒绝作业。
这不是保守 —— guest 在写同一组寄存器, 两边同时提交必然是互相破坏,
而拒绝是可诊断的, 静默破坏不是。

幂等；未初始化(无 NPU)时是空操作。

<details>
<summary>函数内备注（2 条）</summary>

**第 452 行**

> 与 init 里的 irq_enable() 成对：线还挂着, 只是不再投递给宿主 CPU

**第 456 行**

> 给 guest 一个干净的起点：状态位全清, 不屏蔽任何位。
> INT_MASK 交给 guest 自己配(它会按任务描述符写掩码)。

</details>

**位置**：`driver/npu/rknpu.c:443`

<a id="fn-driver-npu-rknpu.c-481"></a>

### `rknpu_impl_host_reclaim`

```c
void rknpu_impl_host_reclaim(void)
```

rknpu_impl_host_reclaim —— 从 guest 手里收回 NPU(vmm_arch_vm_destroy 时)。

与 rknpu_impl_host_release 严格逆序：先清状态位再开中断线。顺序不能反 ——
guest 退出时 NPU 可能还挂着一个没被清的中断, 先开线的话宿主 ISR 会在
enable 的那一刻立刻被拉起来(电平语义), 虽然只是白跑一次 ISR, 但
"开线的瞬间执行了一次中断处理"对排障很不友好。先清掉就没有这个窗口。

只回收当初确实初始化过的核(g_npu_cores>0)：QEMU 之类没有 NPU 的
平台上 rknpu_init 早就 return 了, 这里必须是彻底的 no-op, 尤其不能把
g_npu_ready 置成 true —— 那会让 rknpu_submit_task 以为硬件可用。

幂等；与 release 配对使用。

**位置**：`driver/npu/rknpu.c:481`

<a id="fn-driver-npu-rknpu.c-517"></a>

### `rknpu_job_commit`

```c
static void rknpu_job_commit(int core, void *task_base, uint32_t task_start, uint32_t task_number, uint32_t flags)
```

对一个核提交它自己的那一片任务。

序列对齐 vendor `rknpu_job_subcore_commit()` + `_commit_pc()`(rknpu_job.c
377/272), 也与 testos-reflector 的 job_commit_pc() 一致(真机跑过)。
三处曾经"有意分歧"的地方现在都以 vendor 为准：

1. 先写 PC_DATA_ADDR = 1 切 slave 模式。vendor 无条件写(只在
pc_dma_ctrl 配置下包了个锁, 写的还是同一条)。
2. 0x1004 / 0x3004 是 CNA/CORE 的 S_POINTER, 值里带核号：
`0xe + 0x10000000 * core`。vendor 写的就是这个(参考项目只写 0xe,
因为它的用例只跑 core 0, 看不出差别)。目标仍是该核自己的 base。
3. PC_DMA_BASE_ADDR 写 0 —— librknnrt 把 args->task_base_addr 设成
0, vendor 原样写下去, 等价。

<details>
<summary>函数内备注（9 条）</summary>

**第 527 行**

> 切 slave 模式(本核)

**第 530 行**

> 切到本核的第 0 组寄存器。值里带核号：vendor 写的是
> `0xe + 0x10000000 * core_index`, 不是常量 0xe。

> core 0 时两者都是 0xe, 所以单核跑通完全看不出差别 —— 多核时写错
> 就会让 core1/core2 的 CNA/CORE 指针指到错误的位置, 症状是&quot;作业
> 提交了但一个任务都不完成&quot;。

**第 542 行**

> regcmd 地址 + 数据量(+EXTRA 再按 scale 向上取整)

**第 545 行**

> 一次性把 regcmd 头几个字打出来 —— 用来和 guest 侧的同一份 dump 对比。

> 为什么要这个：guest 里跑 NPU 时, 宿主侧已经验过&quot;描述符地址在恒等窗口里,
> 内容非零&quot;, 但作业仍然不完成；而 guest 的 librknnrt 会多打一条宿主上
> 从没见过的 `REGTASK: The bit width of field value exceeds the limit`。
> 两边跑的是同一个模型, 同一个 librknnrt —— 那么 regcmd 该逐字节相同。
> 不同就说明问题在 librknnrt 生成 regcmd 的那条路上(模型/版本/环境),
> 相同就说明问题在 NPU 的执行环境里。这一行日志就是那个判据。

> 只打一次：提交路径在热路径上, 不能每次都刷。

**第 564 行**

> 和 guest 侧同样的扫描(见 kernel/vmm/vdev/npu/vnpu.c)。

> 判据是一次 A/B：guest 报出 regcmd[126] = 0x78015044 是个
> &quot;窗口外地址&quot;, 但扫描是启发式的(任何落在 0x70000000..0x7FFFFFFF
> 的值都会被拎出来), 一个常量也可能碰巧长这样。

> · 宿主这里 regcmd[126] 也是 0x78015044 → 它是数据常量,
> guest 那条报警是误报, 得换个方向查
> · 宿主这里是别的地址(宿主的 RAM 某处)→ 它是真的缓冲区地址,
> guest 的报警成立 —— 那个缓冲没从预留池里出

> 模型和 librknnrt 两边完全相同, 所以这个对比是干净的。

**第 596 行**

> 期望中断掩码 / 清中断

**第 600 行**

> 任务控制：命令 0x6 | pingpong, 任务数在低 N 位

**第 606 行**

> 提交：OP_EN 1→0

**第 610 行**

> ── 这里曾经有一段&quot;写完立刻回读, 不一致就报错&quot;, 已删除。留个记录,
> 免得以后有人又想加回来。

> 想法是好的：把&quot;寄存器没写进去&quot;(时钟/电源问题)和&quot;写进去了但作业
> 不跑&quot;(描述符问题)分开 —— 这两种情况的现场都是&quot;超时 + task_status=0&quot;。

> 但它在这个硬件上根本不成立：PC 侧的 DATA_ADDR / TASK_CONTROL
> 是硬件消费型寄存器, PC 取走之后值就变了, 而我们的回读是跟它抢时间。
> 真机上的两个反例正好互补, 而且两次推理结果都完全正确：

> 第一次提交：DATA_ADDR 写 0x5b4abc0 回读 0x0, TASK_CONTROL 正常
> 第二次提交：TASK_CONTROL 写 0xa 回读 0x0, DATA_ADDR 正常

> 也就是说这个&quot;不一致&quot;是竞态, 不是故障 —— 每次跑的命中位置都不一样。
> 代价还很实在：一次误报 5 行日志, 在 1.5 Mbaud 的 UART 上约 3.8ms,
> 而它落在应用侧 `start.elapsed()` 的计时窗口里。实测同一份内核连跑两次
> 得到 2.57ms / 6.40ms, 差值就是这段日志。

> 真要&quot;写没进去&quot;的诊断, 看超时路径：那里已经逐核 dump 了完整的
> PC 寄存器快照 —— 作业没跑完才需要现场, 跑完了就不需要。

</details>

**位置**：`driver/npu/rknpu.c:517`

<a id="fn-driver-npu-rknpu.c-645"></a>

### `rknpu_dump_task`

```c
static void rknpu_dump_task(const char *tag, const rknpu_task_t *t)
```

── 排障：超时时把「我们到底给硬件喂了什么」全打出来 ──────────────────

为什么需要：任务描述符是用户态(librknnrt)填的, 我们只读不写。
以前只在 QEMU 上跑过 DRM 层 —— 那里 npu.base0 为 0, rknpu_submit_task
直接返回 -ENODEV, 这条数据通路从来没真正执行过。所以真机上第一次
超时时, 最值得怀疑的就是描述符本身：regcmd_addr 是不是设备地址？
regcmd 内容是不是对的？这片内存我们和用户态看到的是不是同一份？

只在失败路径调用, 正常跑完不产生任何输出。

**位置**：`driver/npu/rknpu.c:645`

<a id="fn-driver-npu-rknpu.c-655"></a>

### `rknpu_dump_regcmd`

```c
static void rknpu_dump_regcmd(const rknpu_task_t *t)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在 dma-buf 中定位任务的 regcmd 偏移，打印至多 8 个 64 位命令字，地址无效或越界时报错。

<details>
<summary>函数内备注（1 条）</summary>

**第 672 行**

> 按 64 位字看 —— regcmd 就是 u64 数组(见参考项目 npu_regs)

</details>

**位置**：`driver/npu/rknpu.c:655`

<a id="fn-driver-npu-rknpu.c-690"></a>

### `rknpu_dump_regs`

```c
static void rknpu_dump_regs(int core)
```

超时时把某个核的 PC 侧寄存器整片读回来 —— 一眼看出"跑到哪, 卡在哪"

**位置**：`driver/npu/rknpu.c:690`

<a id="fn-driver-npu-rknpu.c-717"></a>

### `rknpu_pick_cores`

```c
static int rknpu_pick_cores(uint32_t core_mask, int *cores, int *ncores)
```

core_mask → 要驱动的核。

vendor(rknpu_job_commit)只认这几种组合, 其余一律 "Unknown core mask"
并且一个核都不提交：
0x1 / 0x2 / 0x4 / 0x3(core0+1) / 0x7(三核)
注意 0x5, 0x6 是不支持的 —— 别自作主张替它兜底, 照实拒绝,
否则行为会偏离 librknnrt 的预期。

AUTO(0) 走 vendor 的 rknpu_schedule_core_index()：挑累计任务数最少
的核, 并强制单核。我们目前是同步提交(一路忙等到完成才返回), 设备永远
处于空闲, 所以"负载最小的核"恒为 core 0 —— 这里如实反映这一点, 不假装
做了负载均衡。将来改成异步(队列 + 每核 todo list)时, 这里要接上真的
每核 task_num 记账。

<details>
<summary>函数内备注（1 条）</summary>

**第 745 行**

> 要的核必须在 platform.conf 里配了基址, 否则这个提交没法完整执行

</details>

**位置**：`driver/npu/rknpu.c:717`

<a id="fn-driver-npu-rknpu.c-757"></a>

### `rknpu_submit_task`

```c
int rknpu_submit_task(void *task_base, size_t task_bytes, const struct rknpu_submit *s)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验参数并按 subcore_task 切片算出各核任务范围与期望中断掩码，持锁向各核提交后轮询等待全部完成。

<details>
<summary>函数内备注（15 条）</summary>

**第 760 行（行尾）**

> ENODEV

**第 762 行（行尾）**

> EINVAL

**第 766 行**

> ── 任务按核切片。这一段是照 vendor 的 rknpu_job_subcore_commit_pc()
> (rknpu_job.c:307-330)补的, 之前漏了 —— 正是真机上&quot;跑了 44 个就
> 卡住&quot;的根因。

> vendor 在 num_irqs &gt; 1 的平台上(rk3588 是 3 个 IRQ)永远用
> subcore_task[] 覆盖 args-&gt;task_start/task_number：

> switch (job-&gt;use_core_num) {
> case 1: case 2: slice = subcore_task[core_index];
> case 3:         slice = subcore_task[core_index + 2];
> }

> 也就是说它从不把 args 里的完整任务范围提交给某一个核。librknnrt
> 把模型的任务数组按核切成若干片填进 subcore_task[], 每个核只跑自己那片。
> 索引规则必须照抄：3 核时各片填在 [2..4], 单核/双核填在 [0..1]。

**第 788 行**

> vendor 用请求的 core_mask 算 use_core_num, 切片索引也跟着它走
> —— 所以这个数在挑核之后按原始 core_mask 重新算一遍, 不能用 ncores
> 顶替(AUTO 时 vendor 已经把 mask 改写成单核, 两者才一致)。

**第 799 行**

> 注意： 不要拿 task_start/task_number 去校验切片范围 —— 这是我之前
> 自己发明的规则, 真机上证明是错的。

> 一度以为 `subcore_task[i]` 是&quot;job 范围内的一个子区间&quot;, 于是写了
> &quot;切片必须落在 [task_start, task_start+task_number)&quot; 的校验。真机上
> 直接把正确的切片判成了非法, 退回单核完整范围, 然后作业跑一半卡死：

> job 范围 = [676, 688)
> subcore_task[0..4] = 672+4, 923+4, 672+4, 923+4, 998+4

> 注意 4+4+4 = 12 正好等于该 job 的 task_number —— 每个核的任务片被
> 放在任务数组里各自的区域, 并不落在 task_start 起的那个区间内。
> vendor 对此不做任何校验(rknpu_job_subcore_commit_pc 直接取值),
> 照抄它就对了。

> 唯一保留的是缓冲区边界检查：切片描述符是要真解引用的, 用户态给个
> start=1000000 不能变成内核读越界。这一条是内核自己的安全底线, 不是
> 对 librknnrt 语义的猜测。不合法就直接失败而不是硬凑 —— 硬凑出来
> 的作业会把 NPU 卡在半路, 比当场报错难查得多。

**第 835 行**

> 每个核的期望中断掩码取自它自己那一片的最后一个任务
> (vendor：`job-&gt;int_mask[core_index] = last_task-&gt;int_mask`)。
> 三片可以各不相同 —— 共用一个 expect 会让先完成的核把后完成的判成
> &quot;状态不匹配&quot;。

**第 847 行**

> 注意： 这一行落在被计时的窗口里 —— 应用侧的 `start.elapsed()` 把整个
> SUBMIT 系统调用都算进去, 而 UART 是最贵的一环：实测板子跑 1.5 Mbaud,
> 每字符约 6.7µs。真机上曾经因为多打约 770 字符让 MobileNetV1 从 2.57ms
> 涨到 7.10ms —— 日志本身就是 4~5ms, 比推理还慢。

> 所以这里只在多核时出声：单核(AUTO, 实测 librknnrt 给的就是 0x0)
> 是常态, 没什么可看的；多核是新路径, 每次提交都值得一行。

**第 861 行**

> 原始值一并打出来。切片的语义只能从 librknnrt 实际填的东西
> 反推, 别靠猜 —— 第一次就是猜错了方向(以为切片必定落在 job
> 范围内), 白跑一趟真机还顺带把 core0 卡住了。

**第 884 行**

> 写 expect / 清状态 / 启动所有核, 必须在同一把锁里完成。
> 分开做的话, NPU 快到中断落在&quot;expect 还没写&quot;的窗口里, 就会被判成
> 状态不匹配 —— 那是个只有快机器上才偶发的假故障。

> 多核时更要注意：先把所有核的 expect/state 都写好, 再统一放行。
> 边写边提交的话, 先提交的核可能在最后一颗核的 expect 还没落笔时就
> 打中断回来。

**第 897 行**

> 归零, 让&quot;多核完成&quot;那行报的是本次作业的数字而不是上次的 ——
> 累计值没法证明哪个核刚刚干过活。

**第 907 行**

> 等所有核完成。vendor 也是这个语义 —— 它的 job 在网络里挂着一个
> interrupt_count(初值 = use_core_num), 归零才 wake_up 提交方。

> 1us 步进紧密轮询(作业在微秒级完成, 睡一觉反而更慢), 上限 tmo_ms 毫秒。

**第 920 行（行尾）**

> EIO：中断状态不匹配, ISR 已打详情

**第 925 行**

> ── 多核取证：证明三个核真的都在工作, 而不是我们只写了
> 寄存器, 硬件其实只跑了一个。

> 两列数字都是从每个核自己的寄存器/中断线读来的, 不是我们
> 写下去的回显 —— 这才是证据：

> irq  : 该核独立中断线(142/143/144)上的中断次数, 由
> 各自的 ISR 累加。core1/core2 的 IRQ 响了, 就说明它们
> 各自跑完了自己的活。
> done : 该核 PC_TASK_STATUS 低 12 位 = 它自己完成的任务数,
> 由硬件递增。

> 单核时这两列没什么信息量, 所以只在多核时打 —— 这样常态不花
> UART 时间(见上面 KLOG_INFO 那段关于 6.7µs/字符的说明)。

**第 966 行**

> 描述符是用户态填的 —— 把首尾两个原样打出来, 这是最可疑的一环

**第 987 行**

> 抽看任务表的 op 分布(只对第一个核做, 免得三份日志互相比对)：
> librknnrt 是按 op 分段填这张表的, 段边界就是它切核的地方。切片取对了
> 的话, 该核 range 的末尾应当正好落在某个 op 的最后一个任务上。

</details>

**位置**：`driver/npu/rknpu.c:757`

<a id="fn-driver-npu-rknpu.c-1019"></a>

### `rknpu_probe`

```c
static int rknpu_probe(const device_t *dev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

绑定 NPU 操作表并调用 rknpu_init，返回 0。

<details>
<summary>函数内备注（1 条）</summary>

**第 1021 行（行尾）**

> 阶段 1：rknpu_init() 仍直接读平台配置

</details>

**位置**：`driver/npu/rknpu.c:1019`

<a id="mod-driver-npu-rknpu_drm.c"></a>

## driver/npu/rknpu_drm.c — RK3588 NPU 的 DRM 渲染节点(兼容 shim)

`driver/npu/rknpu_drm.c` · 20 个函数 · 20 个有说明 · 665 行

注意： 这不是 DRM。我们只实现 librknnrt 用到的那一小撮 ioctl 与 GEM 语义,
让官方运行时能连上来。没有 CRTC/connector/framebuffer, 也不打算有。

── 为什么 handle 要按"打开的文件"隔离 ─────────────────────────────────
DRM 里 GEM handle 是 per-drm_file 的(`drm_file` 就是一次 open)。用一张
全局表的话, 两个进程各自 open 渲染节点, 各自从 1 开始发 handle, 就会串号
—— 而且症状是"另一个进程的缓冲区被当成自己的", 极难查。

所以每文件一份上下文。识别用的键是 `vfs_file_t*`(pseudofs 那对
_file_fn 回调把它传下来, 见 pseudofs_internal.h)。

── 缓冲区本体来自 dma-buf ──────────────────────────────────────────────
官方运行时的主路径是 dma_heap 分配 → dmabuf fd → PRIME_FD_TO_HANDLE。
我们只管把 fd 换成 handle, 再换回来, 内存本身归 dma_buf 层。

<a id="fn-driver-npu-rknpu_drm.c-72"></a>

### `drm_file_find`

```c
static rknpu_drm_file_t *drm_file_find(void *file)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

遍历全局文件表，返回 file 指针匹配且已使用的上下文项，找不到返回 NULL。

**位置**：`driver/npu/rknpu_drm.c:72`

<a id="fn-driver-npu-rknpu_drm.c-82"></a>

### `drm_file_get_or_create`

```c
static rknpu_drm_file_t *drm_file_get_or_create(void *file)
```

懒分配：pseudofs 没有 open 钩子, 所以第一次用到时建。

**位置**：`driver/npu/rknpu_drm.c:82`

<a id="fn-driver-npu-rknpu_drm.c-100"></a>

### `rknpu_drm_impl_close_file`

```c
int rknpu_drm_impl_close_file(void *file)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

持锁查文件上下文，逐个释放其 GEM 持有的 dmabuf 引用后 memset 清零该表项。

<details>
<summary>函数内备注（1 条）</summary>

**第 105 行**

> 进程退出时没显式 DESTROY 的 handle 在这里回收 —— 否则每开一次
> 就漏一批缓冲区(fd 池只有 128 槽, 很快耗尽)。

</details>

**位置**：`driver/npu/rknpu_drm.c:100`

<a id="fn-driver-npu-rknpu_drm.c-124"></a>

### `gem_find`

```c
static rknpu_gem_t *gem_find(rknpu_drm_file_t *ctx, uint64_t handle)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

在文件上下文的 GEM 表中按 handle 查找已使用项，找不到返回 NULL。

**位置**：`driver/npu/rknpu_drm.c:124`

<a id="fn-driver-npu-rknpu_drm.c-134"></a>

### `gem_create`

```c
static rknpu_gem_t *gem_create(rknpu_drm_file_t *ctx, uint32_t dma_idx, size_t size)
```

新建一个引用 dma_idx 的对象。失败返回 NULL。

<details>
<summary>函数内备注（1 条）</summary>

**第 143 行（行尾）**

> 对象自己持一份引用

</details>

**位置**：`driver/npu/rknpu_drm.c:134`

<a id="fn-driver-npu-rknpu_drm.c-153"></a>

### `drm_ioctl_version`

```c
static int drm_ioctl_version(void *argp)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

拷入用户 drm_version，按各缓冲长度截断回填驱动名/日期/描述，并回填真实长度与版本号。

<details>
<summary>函数内备注（2 条）</summary>

**第 159 行**

> 注意： 驱动名必须是 &quot;rknpu&quot;, 不是我们自己的什么名字 —— librknnrt
> 拿它(和 /proc/dri/0/name)做身份匹配, 名字不对就当&quot;不是 RKNPU
> 设备&quot;。

**第 188 行**

> 长度一律回串的真实长度, 不是这次实际写入的长度。

> 这是 libdrm 的&quot;两阶段&quot;协议：drmGetVersion() 第一次传
> name=NULL / name_len=0 只为问长度, 拿到之后再 malloc 缓冲,
> 第二次调用取串。要是这里回 0(&quot;没写进去所以长度是 0&quot;), libdrm
> 就不分配, 调用方拿到 name=NULL 还去 strstr —— 空指针崩。

> 实测：改之前 librknnrt 连打两次 DRM_IOCTL_VERSION 都成功, 紧接着
> 就在自己代码里 SIGSEGV(va=0x10), 而日志只显示 ioctl 全部返回 0。

</details>

**位置**：`driver/npu/rknpu_drm.c:153`

<a id="fn-driver-npu-rknpu_drm.c-223"></a>

### `drm_ioctl_set_version`

```c
static int drm_ioctl_set_version(void *argp)
```

DRM_IOCTL_SET_VERSION —— libdrm 的 drmSetVersion()：用户态声明它按哪个
DRM 接口版本说话, 驱动回自己的版本。

内核真实实现(drm_setversion)会在 drm_di_major 不是 DRM_IF_MAJOR 时回
-EINVAL。我们故意不做这个校验：这是兼容层, 不是 DRM, 拒绝一个我们
根本不在意的字段只会给"还没跑通主流程"的阶段多造一个失败点。

只填 dd_*(驱动版本), di_* 原样回显。与 DRM_IOCTL_VERSION 报的
1.0.0 保持一致 —— 两处对不上会让 libdrm 认为驱动在说谎。

<details>
<summary>函数内备注（1 条）</summary>

**第 229 行（行尾）**

> 与 drm_ioctl_version 的 1.0.0 对齐

</details>

**位置**：`driver/npu/rknpu_drm.c:223`

<a id="fn-driver-npu-rknpu_drm.c-244"></a>

### `drm_ioctl_get_unique`

```c
static int drm_ioctl_get_unique(void *argp)
```

DRM_IOCTL_GET_UNIQUE —— 与 VERSION 同一套"两阶段"协议：第一次传
unique=NULL/unique_len=0 只为问长度, 第二次才带缓冲取串。
librknnrt 实测会调它(0xc0106401)。

<details>
<summary>函数内备注（1 条）</summary>

**第 258 行（行尾）**

> 同 VERSION：回真实长度, 不是写入长度

</details>

**位置**：`driver/npu/rknpu_drm.c:244`

<a id="fn-driver-npu-rknpu_drm.c-264"></a>

### `drm_ioctl_get_magic`

```c
static int drm_ioctl_get_magic(void *argp)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把固定的 32 位 magic 值拷贝回用户空间，失败返回 -22。

<details>
<summary>函数内备注（1 条）</summary>

**第 266 行（行尾）**

> &quot;AKAR&quot; —— 只是回一个稳定值

</details>

**位置**：`driver/npu/rknpu_drm.c:264`

<a id="fn-driver-npu-rknpu_drm.c-274"></a>

### `drm_ioctl_gem_flink`

```c
static int drm_ioctl_gem_flink(void *file, void *argp)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

不支持全局名机制，只打印一次告警并返回 -22。

<details>
<summary>函数内备注（1 条）</summary>

**第 276 行**

> GEM_FLINK 给对象起一个全局名, 用于跨进程共享。
> 我们的缓冲区已经能通过 dmabuf fd 共享(PRIME), flink 是更老的机制。
> librknnrt 会在某些版本里探测它 —— 明确回 -EINVAL 表示&quot;本驱动不支持
> 全局名&quot;, 比返回一个假名字安全(假名字会让对端拿到错误的缓冲区)。

</details>

**位置**：`driver/npu/rknpu_drm.c:274`

<a id="fn-driver-npu-rknpu_drm.c-288"></a>

### `drm_ioctl_prime_handle_to_fd`

```c
static int drm_ioctl_prime_handle_to_fd(void *file, void *argp)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

持锁由 handle 查到 dma_buf 后释放锁，为其创建 dmabuf fd 并回写用户结构，失败返回 -22。

<details>
<summary>函数内备注（1 条）</summary>

**第 294 行**

> 只在锁里查表；建 fd 是别的子系统的活, 别抱着自旋锁做
> (那会在关中断的情况下走 fd 池的锁)。

</details>

**位置**：`driver/npu/rknpu_drm.c:288`

<a id="fn-driver-npu-rknpu_drm.c-320"></a>

### `drm_ioctl_prime_fd_to_handle`

```c
static int drm_ioctl_prime_fd_to_handle(void *file, void *argp)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

由用户 fd 取 dma_buf，持锁获取或创建文件上下文与 GEM 对象并回写 handle，失败返回 -22 或 -12。

<details>
<summary>函数内备注（1 条）</summary>

**第 340 行（行尾）**

> ENOMEM

</details>

**位置**：`driver/npu/rknpu_drm.c:320`

<a id="fn-driver-npu-rknpu_drm.c-352"></a>

### `rknpu_ioctl_action`

```c
static int rknpu_ioctl_action(void *argp)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 flags 分支回填 IOMMU、硬件/驱动版本、SRAM 与电源等值，未知 action 采样告警后回 0，最后拷回用户。

<details>
<summary>函数内备注（6 条）</summary>

**第 361 行**

> 必须回 0。librknnrt 先问这个再决定内存策略(有 IOMMU 才可能
> 用非连续缓冲)。我们没实现 SMMU, 谎报 1 会让它按 IOVA 走,
> 之后每次提交都是错的地址。

**第 369 行（行尾）**

> &quot;FIRE&quot;, 与版本寄存器一致

**第 372 行（行尾）**

> 1.00.00.00, vendor 的编码方式

**第 376 行（行尾）**

> 不支持片上 SRAM

**第 380 行**

> 电源域在 rknpu_init() 里已经常开, 这里当作幂等成功

**第 384 行**

> 认不出来的 action：不要回 -EINVAL。很多是查询类
> (频率/带宽/带宽优先级), librknnrt 拿到 0 会走保守路径；
> 回错误可能让它在初始化阶段直接失败。打一条日志, 等上板看
> 日志补齐语义。

</details>

**位置**：`driver/npu/rknpu_drm.c:352`

<a id="fn-driver-npu-rknpu_drm.c-400"></a>

### `rknpu_ioctl_mem_create`

```c
static int rknpu_ioctl_mem_create(void *file, void *argp)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

拒绝非连续内存请求，分配 dma_buf 并持锁创建 GEM 对象，回填 handle、obj_addr、设备地址与 sram_size。

<details>
<summary>函数内备注（6 条）</summary>

**第 408 行**

> 没有 IOMMU 时非连续缓冲直接拒绝 —— 与 vendor 驱动
> (rknpu_gem.c:707) 的语义一致。我们的堆本来就只给物理连续内存,
> 但必须在这里明确拒绝：静默降级成连续会让 librknnrt 以为拿到了
> 非连续缓冲, 之后按 sg 列表算地址就全错了。

**第 421 行（行尾）**

> ENOMEM

**第 424 行（行尾）**

> 无 IOMMU 时即物理地址

**第 432 行（行尾）**

> 创建那份引用交给 gem 了

**第 438 行（行尾）**

> 用户态会原样回传给 SYNC/DESTROY

**第 439 行（行尾）**

> 设备可见地址：唯一出口 dma_buf_dev_addr()

</details>

**位置**：`driver/npu/rknpu_drm.c:400`

<a id="fn-driver-npu-rknpu_drm.c-449"></a>

### `rknpu_ioctl_mem_map`

```c
static int rknpu_ioctl_mem_map(void *file, void *argp)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

持锁按 handle 查 GEM，回填由 GEM id 编码出的 mmap 假偏移。

<details>
<summary>函数内备注（1 条）</summary>

**第 466 行**

> DRM 的约定：这里回一个不透明偏移, 用户态拿它去
> mmap(fd, ..., offset)。sys_mmap 再经 rknpu_drm_impl_mmap_lookup() 解回来。
> 页粒度是硬要求, 见 RKNPU_MMAP_BASE 的说明。

</details>

**位置**：`driver/npu/rknpu_drm.c:449`

<a id="fn-driver-npu-rknpu_drm.c-477"></a>

### `rknpu_ioctl_mem_destroy`

```c
static int rknpu_ioctl_mem_destroy(void *file, void *argp)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

持锁按 handle 或 obj_addr 查 GEM 并标记为未使用，锁外释放其持有的 dmabuf 引用。

<details>
<summary>函数内备注（1 条）</summary>

**第 499 行（行尾）**

> 还掉 gem 那份引用

</details>

**位置**：`driver/npu/rknpu_drm.c:477`

<a id="fn-driver-npu-rknpu_drm.c-503"></a>

### `rknpu_ioctl_mem_sync`

```c
static int rknpu_ioctl_mem_sync(void *file, void *argp)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

持锁由 obj_addr 查到 dma_buf，锁外按 TO_DEVICE/FROM_DEVICE 标志做 cache 写回或失效。

<details>
<summary>函数内备注（1 条）</summary>

**第 509 行**

> 同样：查表在锁里, 缓存维护在锁外 —— 一个几十 MB 的缓冲区
> clean 一遍不能抱着自旋锁做(那是全程关中断)。

</details>

**位置**：`driver/npu/rknpu_drm.c:503`

<a id="fn-driver-npu-rknpu_drm.c-532"></a>

### `rknpu_ioctl_submit`

```c
static int rknpu_ioctl_submit(void *file, void *argp)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按 task_obj_addr 查 dma_buf，先写回整块再调用 rknpu_submit_task，并把 task_counter 回写用户。

<details>
<summary>函数内备注（2 条）</summary>

**第 538 行**

> task_obj_addr 是用户态从 MEM_CREATE / PRIME 拿到的 obj_addr
> (我们回的是 GEM 对象 id)。vendor 驱动这里存的也是这种不透明令牌
> —— 不是设备地址, 别搞混。

**第 558 行**

> 任务描述符是设备直接读的：提交前必须 clean 一遍, 否则 NPU 可能
> 读到 Cache 里的旧内容。用户态一般会自己先 MEM_SYNC, 但那不是我们能
> 依赖的前提 —— 描述符读错的症状是设备行为完全跑偏, 而日志一声不吭。

</details>

**位置**：`driver/npu/rknpu_drm.c:532`

<a id="fn-driver-npu-rknpu_drm.c-578"></a>

### `rknpu_drm_impl_mmap_lookup`

```c
int rknpu_drm_impl_mmap_lookup(void *file, uint64_t offset, uint64_t *paddr_out, uint64_t *size_out)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验偏移落在假基址且页对齐，解码出 GEM id 查 dma_buf，输出设备地址与大小。

<details>
<summary>函数内备注（1 条）</summary>

**第 583 行（行尾）**

> 非页对齐的不是我们的假偏移

</details>

**位置**：`driver/npu/rknpu_drm.c:578`

<a id="fn-driver-npu-rknpu_drm.c-605"></a>

### `rknpu_drm_impl_ioctl_file`

```c
int rknpu_drm_impl_ioctl_file(void *file, uint64_t req, void *argp)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按请求码分派到各 DRM/NPU ioctl 处理函数，未实现的打告警日志并返回 -22。

<details>
<summary>函数内备注（2 条）</summary>

**第 617 行**

> master 与 AGP 都是老 DRM 惯例, 现代渲染节点不需要。
> 但 librknnrt 会发它们做探测(实测 0x641e/0x641f 带 EINTR/EAGAIN
> 重试循环, 0x6430/0x6431 是 AGP)—— 认不出来就回 -EINVAL 是最省事的,
> 但如果它把失败当成&quot;设备不可用&quot;, 就会在初始化阶段直接放弃。

> 渲染节点本来就不需要 master, 回 0 是语义正确的(DRM 里 render
> node 上 setmaster 直接成功)。AGP 我们确实没有, 回 -EINVAL。

**第 654 行**

> 注意： 这张表是静态分析出来的下界(104 个调用点里只还原了 61 个),
> 上板后总会有没见过的号打到这里。所以：打日志 + 回 -EINVAL,
> 而不是静默。日志就是补全 ABI 的唯一手段 —— 见
> docs/npu/RK3588_NPU_DRIVER.md §8 验证策略。

</details>

**位置**：`driver/npu/rknpu_drm.c:605`

<a id="mod-driver-npu-rkpm.c"></a>

## driver/npu/rkpm.c — driver/npu/rkpm.c

`driver/npu/rkpm.c` · 3 个函数 · 3 个有说明 · 138 行

Rockchip RK3588 电源域管理实现

仅实现 NPU 所需的上电路径(PD_NPU / PD_NPUTOP / PD_NPU1 / PD_NPU2)。
遵循 PMU1 的写掩码寄存器协议：高 16 位为写使能掩码, 低 16 位为数据。

<a id="fn-driver-npu-rkpm.c-72"></a>

### `rkpm_wait_on`

```c
static int rkpm_wait_on(rkpm_t *pm, const rkpm_domain_t *d)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

轮询 PMU 的 repair_status 或 status 位直到满足条件，超时告警并返回 RKPM_ERR_TIMEOUT。

<details>
<summary>函数内备注（4 条）</summary>

**第 76 行**

> 优先用 repair_status(有则等待对应位置 1 表示就绪)

**第 83 行**

> 轮询超时是&quot;降级但继续&quot;：调用方拿到错误码自行决定, 内核没有坏。
> 同一类超时在 rknpu.c 的调用点用的是 WARN —— 统一到 WARN。

**第 90 行**

> 无 repair_status：用 status_mask(0 表示上电完成)

**第 101 行**

> 既无 repair_status 也无 status_mask：认为立即完成

</details>

**位置**：`driver/npu/rkpm.c:72`

<a id="fn-driver-npu-rkpm.c-107"></a>

### `rkpm_init`

```c
void rkpm_init(rkpm_t *pm, uintptr_t base)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

把 PMU 寄存器基址记录到句柄结构中。

**位置**：`driver/npu/rkpm.c:107`

<a id="fn-driver-npu-rkpm.c-112"></a>

### `rkpm_power_on`

```c
int rkpm_power_on(rkpm_t *pm, uint32_t id)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

校验电源域 id 后按写掩码协议清除其掉电位，再调用等待函数确认该域就绪。

<details>
<summary>函数内备注（3 条）</summary>

**第 124 行**

> 写掩码协议：高 16 位置 1 表示允许写对应低位。
> 上电 = 清零 pwr_mask 位, 同时写使能 pwr_w_mask(高16位)。

**第 129 行**

> 写入 &quot;高16位=pwr_w_mask 低16位=0&quot;(清除 pwr 位 = 上电)

**第 132 行**

> 无写掩码：读改写

</details>

**位置**：`driver/npu/rkpm.c:112`

<a id="mod-driver-npu-rkpm.h"></a>

## driver/npu/rkpm.h — driver/npu/rkpm.h

`driver/npu/rkpm.h` · 2 个函数 · 2 个有说明 · 87 行

Rockchip RK3588 电源域管理(PMU)

封装 PMU1 MMIO 寄存器操作, 用于在 NPU 初始化前上电
NPU 相关电源域(PD_NPU / PD_NPUTOP / PD_NPU1 / PD_NPU2)。

参考: testos-reflector/src/npu/rkpm.h + Linux rk3588_pm_domains

<a id="fn-driver-npu-rkpm.h-60"></a>

### `rkpm_write`

```c
static inline void rkpm_write(rkpm_t *pm, uint32_t off, uint32_t val)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

向 PMU 基址加偏移处写入 32 位寄存器值。

**位置**：`driver/npu/rkpm.h:60`

<a id="fn-driver-npu-rkpm.h-65"></a>

### `rkpm_read`

```c
static inline uint32_t rkpm_read(rkpm_t *pm, uint32_t off)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从 PMU 基址加偏移处读取 32 位寄存器值。

**位置**：`driver/npu/rkpm.h:65`

<a id="mod-driver-serial-serial.c"></a>

## driver/serial/serial.c — 串口 class 核心

`driver/serial/serial.c` · 6 个函数 · 6 个有说明 · 56 行

g_serial_ops 在串口 probe 之前指向 null 实现，保证 klog/panic 在最早
阶段调用 serial_putc 不会空指针。后端 probe 时通过 serial_bind() 换掉。

<a id="fn-driver-serial-serial.c-16"></a>

### `serial_null_putc`

```c
static void serial_null_putc(char c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空实现，直接丢弃传入字符。

**位置**：`driver/serial/serial.c:16`

<a id="fn-driver-serial-serial.c-21"></a>

### `serial_null_puts`

```c
static void serial_null_puts(const char *s)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空实现，直接丢弃传入字符串。

**位置**：`driver/serial/serial.c:21`

<a id="fn-driver-serial-serial.c-26"></a>

### `serial_null_getc`

```c
static char serial_null_getc(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空实现，恒返回字符 0。

**位置**：`driver/serial/serial.c:26`

<a id="fn-driver-serial-serial.c-31"></a>

### `serial_null_getc_nb`

```c
static bool serial_null_getc_nb(char *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空实现，不输出字符并恒返回 false。

**位置**：`driver/serial/serial.c:31`

<a id="fn-driver-serial-serial.c-37"></a>

### `serial_null_rx_ready`

```c
static bool serial_null_rx_ready(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

空实现，恒返回 false 表示没有可读数据。

**位置**：`driver/serial/serial.c:37`

<a id="fn-driver-serial-serial.c-52"></a>

### `serial_bind`

```c
void serial_bind(const serial_ops_t *ops)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

若 ops 非空则把全局串口操作集替换为 ops。

**位置**：`driver/serial/serial.c:52`

<a id="mod-driver-time-timer.c"></a>

## driver/time/timer.c — 全局变量定义(在架构实现包含之前, 使 timer_handler 可见)

`driver/time/timer.c` · 25 个函数 · 14 个有说明 · 366 行

<a id="fn-driver-time-timer.c-32"></a>

### `timer_set_tick_cb`

```c
void timer_set_tick_cb(timer_tick_cb_t cb)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`driver/time/timer.c:32`

<a id="fn-driver-time-timer.c-49"></a>

### `timer_tick_owner`

```c
static inline bool timer_tick_owner(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`driver/time/timer.c:49`

<a id="fn-driver-time-timer.c-80"></a>

### `timer_delay_counter_frequency`

```c
static uint64_t timer_delay_counter_frequency(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`driver/time/timer.c:80`

<a id="fn-driver-time-timer.c-96"></a>

### `timer_init`

```c
void timer_init(void)
```

初始化定时器

<details>
<summary>函数内备注（2 条）</summary>

**第 98 行**

> 从平台配置中读取定时器参数

**第 104 行**

> 调用架构特定的初始化

</details>

**位置**：`driver/time/timer.c:96`

<a id="fn-driver-time-timer.c-111"></a>

### `timer_enable`

```c
void timer_enable(void)
```

启用定时器

**位置**：`driver/time/timer.c:111`

<a id="fn-driver-time-timer.c-116"></a>

### `timer_init_secondary`

```c
void timer_init_secondary(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（2 条）</summary>

**第 119 行**

> 次级核需要独立 enable 本地 banked PPI(GICv2 GICD_ISENABLER0 每 CPU 独立)。
> 主核 timer_arch_init 注册时只对主核生效, 这里由控制器补本核的那一份。

**第 126 行（行尾）**

> enable, unmask

</details>

**位置**：`driver/time/timer.c:116`

<a id="fn-driver-time-timer.c-135"></a>

### `timer_disable`

```c
void timer_disable(void)
```

禁用定时器

**位置**：`driver/time/timer.c:135`

<a id="fn-driver-time-timer.c-141"></a>

### `timer_set_next_interrupt`

```c
void timer_set_next_interrupt(uint64_t ticks_from_now)
```

设置下一次中断

**位置**：`driver/time/timer.c:141`

<a id="fn-driver-time-timer.c-147"></a>

### `timer_schedule_next_tick`

```c
void timer_schedule_next_tick(void)
```

调度下一个tick

**位置**：`driver/time/timer.c:147`

<a id="fn-driver-time-timer.c-159"></a>

### `timer_read_counter`

```c
uint64_t timer_read_counter(void)
```

时间相关函数

**位置**：`driver/time/timer.c:159`

<a id="fn-driver-time-timer.c-172"></a>

### `timer_counter_frequency`

```c
uint64_t timer_counter_frequency(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`driver/time/timer.c:172`

<a id="fn-driver-time-timer.c-192"></a>

### `timer_counter_to_ns`

```c
uint64_t timer_counter_to_ns(uint64_t ticks)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`driver/time/timer.c:192`

<a id="fn-driver-time-timer.c-206"></a>

### `timer_spin`

```c
void timer_spin(uint32_t iterations)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`driver/time/timer.c:206`

<a id="fn-driver-time-timer.c-212"></a>

### `timer_poll_until`

```c
int timer_poll_until(timer_poll_predicate_t pred, void *ctx, uint32_t max_polls, uint32_t relax_iters)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`driver/time/timer.c:212`

<a id="fn-driver-time-timer.c-226"></a>

### `timer_poll_until_us`

```c
int timer_poll_until_us(timer_poll_predicate_t pred, void *ctx, uint64_t timeout_us, uint32_t relax_iters)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`driver/time/timer.c:226`

<a id="fn-driver-time-timer.c-248"></a>

### `timer_get_system_ticks`

```c
uint64_t timer_get_system_ticks(void)
```

获取系统tick数

**位置**：`driver/time/timer.c:248`

<a id="fn-driver-time-timer.c-254"></a>

### `timer_get_uptime_ms`

```c
uint64_t timer_get_uptime_ms(void)
```

获取系统运行时间(毫秒)

<details>
<summary>函数内备注（1 条）</summary>

**第 256 行**

> 用单调硬件计数，而不是 g_system_ticks * tick_ms：
> - SMP 下全局 tick 只能由一个核算账，不能代表真实经过的时间；
> - 即使 BSP 的 tick ISR 迟到，硬件计数也不受影响。
> x86 未标定 TSC 时 timer_get_ns() 内部会退化到 g_system_ticks
> (那时 tick 是唯一的时钟，见 timer_counter_to_ns)。

</details>

**位置**：`driver/time/timer.c:254`

<a id="fn-driver-time-timer.c-267"></a>

### `timer_get_frequency`

```c
uint64_t timer_get_frequency(void)
```

获取定时器频率

**位置**：`driver/time/timer.c:267`

<a id="fn-driver-time-timer.c-273"></a>

### `timer_delay_ms`

```c
void timer_delay_ms(uint32_t ms)
```

毫秒延时(忙等待)

**位置**：`driver/time/timer.c:273`

<a id="fn-driver-time-timer.c-292"></a>

### `timer_delay_us`

```c
void timer_delay_us(uint32_t us)
```

微秒延时(忙等待)

**位置**：`driver/time/timer.c:292`

<a id="fn-driver-time-timer.c-315"></a>

### `timer_get_stats`

```c
void timer_get_stats(timer_stats_t *stats)
```

获取统计信息

**位置**：`driver/time/timer.c:315`

<a id="fn-driver-time-timer.c-323"></a>

### `timer_reset_stats`

```c
void timer_reset_stats(void)
```

重置统计信息

**位置**：`driver/time/timer.c:323`

<a id="fn-driver-time-timer.c-332"></a>

### `timer_dump_info`

```c
void timer_dump_info(void)
```

打印定时器信息

**位置**：`driver/time/timer.c:332`

<a id="fn-driver-time-timer.c-345"></a>

### `timer_probe`

```c
static int timer_probe(const device_t *dev)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 347 行（行尾）**

> 阶段 1：timer_init() 仍直接读平台配置

</details>

**位置**：`driver/time/timer.c:345`

<a id="fn-driver-time-timer.c-352"></a>

### `timer_start`

```c
static int timer_start(const device_t *dev)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`driver/time/timer.c:352`

<a id="mod-driver-time-timer_aarch64_impl.h"></a>

## driver/time/timer_aarch64_impl.h — 定时器寄存器访问宏

`driver/time/timer_aarch64_impl.h` · 6 个函数 · 1 个有说明 · 133 行

<a id="fn-driver-time-timer_aarch64_impl.h-39"></a>

### `timer_irq_entry`

```c
static void timer_irq_entry(void *frame, void *ctx)
```

公共 IRQ 接口的处理器签名是 (frame, ctx)；定时器不使用 ctx

**位置**：`driver/time/timer_aarch64_impl.h:39`

<a id="fn-driver-time-timer_aarch64_impl.h-45"></a>

### `timer_arch_init`

```c
void timer_arch_init(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（5 条）</summary>

**第 47 行**

> 读取定时器频率

**第 50 行**

> 时钟配错是经典 bug 来源 —— 值得一行；原来是四行

**第 53 行**

> 禁用定时器

**第 56 行**

> 清零统计信息

**第 59 行**

> 控制器负责优先级、注册与使能(PPI 26/30 由平台配置给出)

</details>

**位置**：`driver/time/timer_aarch64_impl.h:45`

<a id="fn-driver-time-timer_aarch64_impl.h-66"></a>

### `timer_arch_enable`

```c
void timer_arch_enable(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（3 条）</summary>

**第 68 行**

> 计算下一次中断的时间

**第 71 行**

> 设置定时器值

**第 74 行**

> 启用定时器, 不屏蔽中断

</details>

**位置**：`driver/time/timer_aarch64_impl.h:66`

<a id="fn-driver-time-timer_aarch64_impl.h-81"></a>

### `timer_arch_disable`

```c
void timer_arch_disable(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 83 行**

> 禁用定时器并屏蔽中断

</details>

**位置**：`driver/time/timer_aarch64_impl.h:81`

<a id="fn-driver-time-timer_aarch64_impl.h-89"></a>

### `timer_arch_set_next_interrupt`

```c
void timer_arch_set_next_interrupt(uint64_t ticks_from_now)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`driver/time/timer_aarch64_impl.h:89`

<a id="fn-driver-time-timer_aarch64_impl.h-103"></a>

### `timer_handler`

```c
void timer_handler(uint64_t *stack_pointer)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（6 条）</summary>

**第 105 行（行尾）**

> Suppress unused parameter warning

**第 107 行**

> 每个 tick 排空 UART, 确保 Ctrl+C 能及时被检测

**第 110 行**

> 全局 tick 只由 BSP 推进；每核计数在 cpu_t::local_ticks

**第 116 行**

> 更新统计信息

**第 120 行**

> 调度下一个tick

**第 123 行**

> 调用 tick 回调(调度器 sched_tick)
> 注意SMP 启动期间 secondary timer 可能在 g_tick_cb 被设之前就已触发
> 中断。这里 silent skip, 避免刷屏 ERROR。

</details>

**位置**：`driver/time/timer_aarch64_impl.h:103`

<a id="mod-driver-time-timer_riscv64_impl.h"></a>

## driver/time/timer_riscv64_impl.h — SBI v0.2+ TIME extension and legacy timer extension IDs

`driver/time/timer_riscv64_impl.h` · 6 个函数 · 1 个有说明 · 242 行

<a id="fn-driver-time-timer_riscv64_impl.h-47"></a>

### `timer_irq_entry`

```c
static void timer_irq_entry(void *frame, void *ctx)
```

公共 IRQ 接口的处理器签名是 (frame, ctx)；定时器不使用 ctx

**位置**：`driver/time/timer_riscv64_impl.h:47`

<a id="fn-driver-time-timer_riscv64_impl.h-53"></a>

### `timer_arch_init`

```c
void timer_arch_init(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（5 条）</summary>

**第 55 行**

> 设置定时器频率

**第 58 行**

> 时钟配错是经典 bug 来源 —— 值得一行；原来是四行

**第 61 行**

> 禁用定时器

**第 64 行**

> 清零统计信息

**第 67 行**

> 控制器负责把 handler 挂到 S 模式定时器中断(scause=5)

</details>

**位置**：`driver/time/timer_riscv64_impl.h:53`

<a id="fn-driver-time-timer_riscv64_impl.h-94"></a>

### `timer_arch_enable`

```c
void timer_arch_enable(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（5 条）</summary>

**第 96 行**

> 计算下一次中断的时间

**第 101 行**

> 设时间原点。用 &quot;now - 已过周期数*step&quot; 而不是直接取 now：
> disable/enable 再次调用时(SMP 启动路径)g_system_ticks 不会倒退。

**第 108 行**

> 首先设置下一次中断时间

**第 111 行**

> 然后启用Machine模式定时器中断

**第 113 行（行尾）**

> 启用机器定时器中断

</details>

**位置**：`driver/time/timer_riscv64_impl.h:94`

<a id="fn-driver-time-timer_riscv64_impl.h-119"></a>

### `timer_arch_disable`

```c
void timer_arch_disable(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（2 条）</summary>

**第 121 行**

> 禁用Machine模式定时器中断

**第 123 行（行尾）**

> 禁用机器定时器中断

</details>

**位置**：`driver/time/timer_riscv64_impl.h:119`

<a id="fn-driver-time-timer_riscv64_impl.h-129"></a>

### `timer_arch_set_next_interrupt`

```c
void timer_arch_set_next_interrupt(uint64_t ticks_from_now)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（4 条）</summary>

**第 135 行**

> 兜底：频率未知(s_tick_step 还没设)。退化成老行为 —— 会漂,
> 但至少能走。正常情况下 timer_arch_enable 已经设好了。

**第 141 行**

> 绝对截止时间：从时间原点 + 整数个周期算出来, 不是从现在起算。
> ISR 迟到只影响本次, 不会累积到下一次。

**第 148 行**

> Prefer SBI v0.2+ TIME extension first; if firmware does not support it,
> fall back to the legacy timer extension.

**第 160 行**

> Legacy SBI: EID=0, arg0=stime_value

</details>

**位置**：`driver/time/timer_riscv64_impl.h:129`

<a id="fn-driver-time-timer_riscv64_impl.h-177"></a>

### `timer_handler`

```c
void timer_handler(void *frame)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（7 条）</summary>

**第 181 行**

> S-mode IRQ can arrive at arbitrary kernel instructions. Keep that path
> short and avoid signal/TTY work there until kernel preemption and locks
> are fully audited. U-mode timer IRQ keeps the old responsive Ctrl+C path.

**第 189 行**

> 更新系统 tick 计数。

> 按真实时间算, 不能自增：ISR 迟到时自增会少算, 而
> timer_get_uptime_ms() = g_system_ticks * TIMER_TICK_MS
> 就是它乘出来的 —— 少算就等于内核时钟走慢(实测负载下慢到 1/1.5,
> 会让 bwtest 在 100Mbps 链路上报出 140Mbps)。除以 s_tick_step 得到
> &quot;已过多少个周期&quot;, 迟到的部分天然被补回来。

**第 205 行**

> 兜底：频率未知, 退化成老行为

**第 216 行**

> 更新统计信息

**第 221 行**

> 调度下一个tick

**第 224 行**

> 调用 tick 回调(调度器 sched_tick)

**第 229 行**

> VMM 的 VS-mode WFI 唤醒钩子。

> guest 在 VS-mode 执行 WFI 后，vCPU 任务挂在自己的 completion 上；
> 它的虚拟定时器截止时间没有宿主中断可用，只能靠这个 10ms 的宿主
> tick 兜底唤醒、重新检查 deadline。没有 VMM（未链接 hext_run.o）
> 时是弱符号 NULL，这里什么都不做。

</details>

**位置**：`driver/time/timer_riscv64_impl.h:177`

<a id="mod-driver-time-timer_x86_64_impl.h"></a>

## driver/time/timer_x86_64_impl.h — x86_64 定时器实现 — LAPIC Timer

`driver/time/timer_x86_64_impl.h` · 8 个函数 · 1 个有说明 · 141 行

优先使用 TSC-deadline oneshot 模式（CPUID.01H:ECX[24]），由
kernel/time/hrtimer.c 在“下一个 100Hz tick”和“最早 sleeper 截止时间”
之间取最小值编程；CPU/环境不支持时退回 Periodic 模式（由 PIT 校准）。

函数签名与 AArch64/RISC-V impl 一致(无 static inline),
由 timer.c #include 本文件来获得定义。

<a id="fn-driver-time-timer_x86_64_impl.h-37"></a>

### `timer_irq_entry`

```c
static void timer_irq_entry(void *frame, void *ctx)
```

公共 IRQ 接口的处理器签名是 (frame, ctx)；定时器不使用 ctx

**位置**：`driver/time/timer_x86_64_impl.h:37`

<a id="fn-driver-time-timer_x86_64_impl.h-45"></a>

### `timer_arch_init`

```c
void timer_arch_init(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（4 条）</summary>

**第 47 行**

> x86_64 上 LAPIC 是计时源, 实际频率由校准决定；
> g_timer_frequency 设一个占位值(校准后 lapic.c 内部使用)

**第 50 行（行尾）**

> 将由 lapic_timer_init 校准

**第 52 行**

> 时钟配错是经典 bug 来源 —— 值得一行；原来是三行

**第 57 行**

> 控制器负责把 handler 挂到 LAPIC timer 向量

</details>

**位置**：`driver/time/timer_x86_64_impl.h:45`

<a id="fn-driver-time-timer_x86_64_impl.h-62"></a>

### `timer_arch_enable`

```c
void timer_arch_enable(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 64 行**

> lapic_timer_init 会优先把 LAPIC 切到 TSC-deadline oneshot 模式；
> 只有 CPU/环境不支持时才退回 Periodic。随后由 hrtimer 负责把
> “下一个 100Hz tick”和本核最早的 sleep deadline 取最小值武装。

</details>

**位置**：`driver/time/timer_x86_64_impl.h:62`

<a id="fn-driver-time-timer_x86_64_impl.h-75"></a>

### `timer_arch_oneshot_supported`

```c
bool timer_arch_oneshot_supported(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`driver/time/timer_x86_64_impl.h:75`

<a id="fn-driver-time-timer_x86_64_impl.h-80"></a>

### `timer_arch_program_oneshot_ns`

```c
void timer_arch_program_oneshot_ns(uint64_t deadline_ns)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`driver/time/timer_x86_64_impl.h:80`

<a id="fn-driver-time-timer_x86_64_impl.h-85"></a>

### `timer_arch_disable`

```c
void timer_arch_disable(void)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

**位置**：`driver/time/timer_x86_64_impl.h:85`

<a id="fn-driver-time-timer_x86_64_impl.h-91"></a>

### `timer_arch_set_next_interrupt`

```c
void timer_arch_set_next_interrupt(uint64_t ticks_from_now)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（1 条）</summary>

**第 93 行**

> LAPIC Periodic 模式硬件自动重载, 无需软件干预

</details>

**位置**：`driver/time/timer_x86_64_impl.h:91`

<a id="fn-driver-time-timer_x86_64_impl.h-102"></a>

### `timer_handler`

```c
void timer_handler(void *frame)
```

> ⚠️ 尚无文档注释。请按 [注释规范](README.md) 在函数上方补充。

<details>
<summary>函数内备注（5 条）</summary>

**第 106 行**

> TSC-deadline 模式下，这个向量同时服务“100Hz tick”和“hrtimer 到期”。
> hrtimer_isr() 先判断 tick 是否到期并兑现 sleeper；只有 tick 到期时
> 才跑下面的 tick 记账/调度回调。

**第 114 行**

> 每个 tick 排空 UART 输入, 确保 Ctrl+C 能及时被检测

**第 117 行**

> 全局 tick / 秒计数只由 BSP 推进；每核计数在 cpu_t::local_ticks

**第 133 行**

> EOI 由 LAPIC dispatch 在 handler 返回后统一发出

**第 135 行**

> 调用 tick 回调(调度器 sched_tick)

</details>

**位置**：`driver/time/timer_x86_64_impl.h:102`

<a id="mod-driver-tpu-cvi_tpu.c"></a>

## driver/tpu/cvi_tpu.c — SOPHGO/Cvitek TPU 裸机驱动

`driver/tpu/cvi_tpu.c` · 14 个函数 · 14 个有说明 · 347 行

改写自 ref/tpu-ana/hal/mars/tpu_platform.c

主要变化(相比 Linux 原版)：
· wait_for_completion_interruptible_timeout → 轮询 TDMA_INT_MASK(同步等待)
· 移除 spinlock / mutex / clk_* / reset_control_* / dma_mapping
· 移除 kernel thread(直接同步调用)
· pr_debug / pr_err → KLOG_DEBUG / KLOG_ERROR
· ioremap → platform_get_mmio

<a id="fn-driver-tpu-cvi_tpu.c-37"></a>

### `tdma_read`

```c
static inline uint32_t tdma_read(uint32_t off)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取 TPU TDMA 基址加偏移处的 32 位寄存器。

**位置**：`driver/tpu/cvi_tpu.c:37`

<a id="fn-driver-tpu-cvi_tpu.c-41"></a>

### `tdma_write`

```c
static inline void tdma_write(uint32_t val, uint32_t off)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

向 TPU TDMA 基址加偏移处写入 32 位寄存器。

**位置**：`driver/tpu/cvi_tpu.c:41`

<a id="fn-driver-tpu-cvi_tpu.c-45"></a>

### `tiu_read`

```c
static inline uint32_t tiu_read(uint32_t off)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取 TPU TIU 基址加偏移处的 32 位寄存器。

**位置**：`driver/tpu/cvi_tpu.c:45`

<a id="fn-driver-tpu-cvi_tpu.c-49"></a>

### `tiu_write`

```c
static inline void tiu_write(uint32_t val, uint32_t off)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

向 TPU TIU 基址加偏移处写入 32 位寄存器。

**位置**：`driver/tpu/cvi_tpu.c:49`

<a id="fn-driver-tpu-cvi_tpu.c-60"></a>

### `resync_cmd_id`

```c
static void resync_cmd_id(void)
```

resync_cmd_id — 复位 TIU/TDMA 指令 ID 及中断状态
对应 Linux 中的 resync_cmd_id()

<details>
<summary>函数内备注（5 条）</summary>

**第 64 行**

> 复位 TIU ID 计数器

**第 69 行**

> 停止 TIU

**第 73 行**

> 清 TIU 中断状态 (bit 1)

**第 77 行**

> 复位 TDMA sync ID

**第 81 行**

> 清 TDMA 中断状态

</details>

**位置**：`driver/tpu/cvi_tpu.c:60`

<a id="fn-driver-tpu-cvi_tpu.c-88"></a>

### `set_array_bases`

```c
static void set_array_bases(const struct cvi_tpu_dma_hdr *hdr)
```

set_array_bases — 将 dmabuf 头中的 8 个数组基地址写入 TDMA 寄存器

<details>
<summary>函数内备注（1 条）</summary>

**第 98 行**

> 高32位：SG2002 物理地址在 32 位范围内, 高位填 0

</details>

**位置**：`driver/tpu/cvi_tpu.c:88`

<a id="fn-driver-tpu-cvi_tpu.c-107"></a>

### `fire_tdma`

```c
static void fire_tdma(uint64_t desc_offset, uint32_t num_tdma)
```

fire_tdma — 启动 TDMA 描述符执行
对应 Linux 中的 set_tdma_descriptor_Fire()

**位置**：`driver/tpu/cvi_tpu.c:107`

<a id="fn-driver-tpu-cvi_tpu.c-123"></a>

### `fire_tiu`

```c
static void fire_tiu(uint64_t desc_offset)
```

fire_tiu — 启动 TIU 描述符执行
对应 Linux 中的 set_tiu_descriptor()

<details>
<summary>函数内备注（3 条）</summary>

**第 132 行**

> 禁用 pre_exe

**第 136 行**

> 设置 1 array, lane=8 (bits[29:22] = 3)

**第 141 行**

> 触发 TIU

</details>

**位置**：`driver/tpu/cvi_tpu.c:123`

<a id="fn-driver-tpu-cvi_tpu.c-154"></a>

### `poll_tdma_done`

```c
static int poll_tdma_done(void)
```

poll_tdma_done — 轮询等待 TDMA 完成(替换 wait_for_completion_timeout)

TDMA_INT_MASK[31:16] 是中断状态, 屏蔽 TDMA_MASK_INIT 位后：
TDMA_INT_EOD   (0x01) — 描述符执行完成
TDMA_INT_EOPMU (0x8000) — PMU 缓冲满
其他非零值 — 错误

<details>
<summary>函数内备注（1 条）</summary>

**第 162 行（行尾）**

> 清中断

</details>

**位置**：`driver/tpu/cvi_tpu.c:154`

<a id="fn-driver-tpu-cvi_tpu.c-179"></a>

### `poll_tiu_done`

```c
static int poll_tiu_done(uint32_t bd_cmd_id)
```

poll_tiu_done — 轮询等待 TIU 完成指定 BD 数
对应 Linux 中的 poll_cmdbuf_done() TIU 部分

<details>
<summary>函数内备注（1 条）</summary>

**第 189 行（行尾）**

> 清中断

</details>

**位置**：`driver/tpu/cvi_tpu.c:179`

<a id="fn-driver-tpu-cvi_tpu.c-202"></a>

### `cvi_tpu_impl_init`

```c
static void cvi_tpu_impl_init(void)
```

cvi_tpu_init — 从 platform_cfg 读取 MMIO 基地址, 完成 TPU 基本初始化

<details>
<summary>函数内备注（1 条）</summary>

**第 215 行**

> 清残留中断, 复位指令 ID

</details>

**位置**：`driver/tpu/cvi_tpu.c:202`

<a id="fn-driver-tpu-cvi_tpu.c-223"></a>

### `cvi_tpu_impl_is_ready`

```c
static bool cvi_tpu_impl_is_ready(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回 TPU 初始化标志是否非零。

**位置**：`driver/tpu/cvi_tpu.c:223`

<a id="fn-driver-tpu-cvi_tpu.c-236"></a>

### `cvi_tpu_impl_run_dmabuf`

```c
static int cvi_tpu_impl_run_dmabuf(void *dmabuf_v, uint64_t dmabuf_p)
```

cvi_tpu_run_dmabuf — 执行一个 DMA 缓冲(同步轮询, 无中断/线程)

@dmabuf_v: DMA 缓冲虚拟地址(含 dma_hdr + cpu_sync_desc 数组)
@dmabuf_p: DMA 缓冲物理地址(供 TPU DMA 引擎使用)

改写自 platform_run_dmabuf(), 直接同步等待替代 wait_for_completion。

<details>
<summary>函数内备注（13 条）</summary>

**第 255 行**

> dmabuf 必须 4KB 页对齐(TPU DMA 引擎要求)

**第 263 行**

> 写入数组基地址

**第 266 行**

> PMU 缓冲(可选, 需 16 字节对齐)

**第 276 行**

> event=TdmaBandwidth(0x2)«带宽统计», enable+tpu+tdma, burst=16, ring-buf

**第 277 行（行尾）**

> enable

**第 278 行（行尾）**

> enable_tpu

**第 279 行（行尾）**

> enable_tdma

**第 280 行（行尾）**

> event = TdmaBandwidth

**第 281 行（行尾）**

> burst length = 16

**第 282 行（行尾）**

> ring buffer mode

**第 283 行（行尾）**

> enable dcm

**第 300 行**

> 同步轮询等待 TDMA(替换 wait_for_completion_interruptible_timeout)

**第 316 行**

> 停止 PMU, 等待 EOPMU

</details>

**位置**：`driver/tpu/cvi_tpu.c:236`

<a id="fn-driver-tpu-cvi_tpu.c-333"></a>

### `cvi_tpu_probe`

```c
static int cvi_tpu_probe(const device_t *dev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

绑定 TPU 操作表并调用驱动初始化函数，返回 0。

<details>
<summary>函数内备注（1 条）</summary>

**第 335 行（行尾）**

> 阶段 1：cvi_tpu_impl_init() 仍直接读平台配置

</details>

**位置**：`driver/tpu/cvi_tpu.c:333`

<a id="mod-driver-tpu-tpu_core.c"></a>

## driver/tpu/tpu_core.c — TPU 服务注册点

`driver/tpu/tpu_core.c` · 1 个函数 · 1 个有说明 · 21 行

永远参与构建。TPU=cvitpu 时 driver/tpu/cvi_tpu.c 的 probe 调 tpu_bind()；
否则 g_tpu_ops 保持 NULL，include/driver/tpu.h 的内联包装统一返回失败。

<a id="fn-driver-tpu-tpu_core.c-18"></a>

### `tpu_bind`

```c
void tpu_bind(const tpu_ops_t *ops)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

保存传入的 TPU 操作表指针供核心层调用。

**位置**：`driver/tpu/tpu_core.c:18`

<a id="mod-driver-uart-uart_dw.c"></a>

## driver/uart/uart_dw.c

`driver/uart/uart_dw.c` · 31 个函数 · 31 个有说明 · 545 行

<a id="fn-driver-uart-uart_dw.c-31"></a>

### `sg2002_dbg_putc`

```c
static inline void sg2002_dbg_putc(char c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

轮询 UART 状态寄存器的发送就绪位，就绪后把字符写入 UART 数据寄存器。

**位置**：`driver/uart/uart_dw.c:31`

<a id="fn-driver-uart-uart_dw.c-39"></a>

### `sg2002_dbg_putc`

```c
static inline void sg2002_dbg_putc(char c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

轮询 UART 状态寄存器的发送就绪位，就绪后把字符写入 UART 数据寄存器。

**位置**：`driver/uart/uart_dw.c:39`

<a id="fn-driver-uart-uart_dw.c-77"></a>

### `buffer_is_empty`

```c
static bool buffer_is_empty(dw_uart_buffer_t *buf)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断环形缓冲的 count 是否为 0。

**位置**：`driver/uart/uart_dw.c:77`

<a id="fn-driver-uart-uart_dw.c-81"></a>

### `buffer_is_full`

```c
static bool buffer_is_full(dw_uart_buffer_t *buf)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断环形缓冲的 count 是否达到发送缓冲容量。

**位置**：`driver/uart/uart_dw.c:81`

<a id="fn-driver-uart-uart_dw.c-85"></a>

### `buffer_put`

```c
static bool buffer_put(dw_uart_buffer_t *buf, char c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

缓冲已满时返回 false，否则写入 head 位置、前移 head 并递增 count。

**位置**：`driver/uart/uart_dw.c:85`

<a id="fn-driver-uart-uart_dw.c-94"></a>

### `buffer_get`

```c
static bool buffer_get(dw_uart_buffer_t *buf, char *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

缓冲为空时返回 false，否则从 tail 取出字符、前移 tail 并递减 count。

**位置**：`driver/uart/uart_dw.c:94`

<a id="fn-driver-uart-uart_dw.c-105"></a>

### `dw_uart_tx_ready`

```c
static bool __attribute__((unused)) dw_uart_tx_ready(void)
```

DW_UART_THR 空了, 可以写入新数据时, 硬件会置位

**位置**：`driver/uart/uart_dw.c:105`

<a id="fn-driver-uart-uart_dw.c-111"></a>

### `dw_uart_rx_ready`

```c
static bool dw_uart_rx_ready(void)
```

DW_UART_RBR 中的数据可读了, 硬件给他置位

**位置**：`driver/uart/uart_dw.c:111`

<a id="fn-driver-uart-uart_dw.c-116"></a>

### `dw_uart_lsr_temt_ready`

```c
static bool dw_uart_lsr_temt_ready(void *ctx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取 LSR 判断发送器完全空闲位是否置位并返回结果。

**位置**：`driver/uart/uart_dw.c:116`

<a id="fn-driver-uart-uart_dw.c-122"></a>

### `dw_uart_lsr_thre_ready`

```c
static bool dw_uart_lsr_thre_ready(void *ctx)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取 LSR 判断发送保持寄存器空位是否置位并返回结果。

**位置**：`driver/uart/uart_dw.c:122`

<a id="fn-driver-uart-uart_dw.c-129"></a>

### `dw_uart_enable_tx_interrupt`

```c
static void dw_uart_enable_tx_interrupt(void)
```

启用发送中断, 我有数据可以发送了

<details>
<summary>函数内备注（1 条）</summary>

**第 133 行**

> 只在真正需要启用时才打印(避免重复启用的噪音)

</details>

**位置**：`driver/uart/uart_dw.c:129`

<a id="fn-driver-uart-uart_dw.c-140"></a>

### `dw_uart_disable_tx_interrupt`

```c
static void dw_uart_disable_tx_interrupt(void)
```

禁用发送中断, 我已经没有数据可以发送了

**位置**：`driver/uart/uart_dw.c:140`

<a id="fn-driver-uart-uart_dw.c-148"></a>

### `dw_uart_enable_rx_interrupt`

```c
static void dw_uart_enable_rx_interrupt(void)
```

启用接收中断, 我准备好接收数据了

**位置**：`driver/uart/uart_dw.c:148`

<a id="fn-driver-uart-uart_dw.c-155"></a>

### `dw_uart_interrupt_handler`

```c
void dw_uart_interrupt_handler(uint64_t *stack_pointer)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读 IIR 区分收发：RX 加锁把 FIFO 数据存入接收缓冲，TX 加锁批量写入并在缓冲发空时关闭 TX 中断。

<details>
<summary>函数内备注（10 条）</summary>

**第 160 行（行尾）**

> 记录IIR值用于调试

**第 162 行（行尾）**

> RX 中断

**第 163 行（行尾）**

> 调试计数

**第 168 行**

> 不要在中断中打印！会导致死锁和重复输出

**第 174 行**

> 0x2 = TX Holding Register Empty

**第 175 行（行尾）**

> TX 中断

**第 176 行（行尾）**

> 调试计数

**第 187 行（行尾）**

> 记录总共发送的字节数

**第 193 行**

> 在释放锁之后再禁用TX中断, 避免竞态条件

**第 195 行**

> 它的意义是：
> 当我们已经把所有待发送的数据都写给硬件时, 就没必要再关心 TX 中断了。
> 因为：
> 硬件下次&quot;THR 空了&quot;时, 我们也没数据可以写；
> 如果不关中断, 硬件会不停地产生 TX 中断, 每次都发现&quot;没数据&quot;, 浪费 CPU。

</details>

**位置**：`driver/uart/uart_dw.c:155`

<a id="fn-driver-uart-uart_dw.c-213"></a>

### `dw_uart_wait_idle`

```c
static void dw_uart_wait_idle(void)
```

dw_uart_wait_idle - 等待 UART 发送器空闲

检查 LSR.TEMT (bit 6): 发送器完全空(TX Holding + TX Shift Register 均空)
适用于标准 16550 / QEMU ns16550(byte-addressed, reg-shift=0)

<details>
<summary>函数内备注（1 条）</summary>

**第 217 行**

> 超时也继续, 避免卡死

</details>

**位置**：`driver/uart/uart_dw.c:213`

<a id="fn-driver-uart-uart_dw.c-220"></a>

### `dw_uart_early_init`

```c
void dw_uart_early_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

等 UART 空闲后关闭所有中断，并使能 FIFO 且清空收发缓冲，不改动波特率。

<details>
<summary>函数内备注（5 条）</summary>

**第 222 行**

> 基址/reg_shift 由 probe 从设备表设置（或 vm_early 的早期覆盖）

**第 224 行**

> OpenSBI (或 QEMU) 已经完成波特率和帧格式的配置,
> 这里只需：
> 1. 关闭中断(轮询模式)
> 2. 使能 FIFO 并清空收发缓冲
> 不要碰 DLAB/DLL/DLM, 否则会改变波特率导致乱码。

**第 232 行**

> 等待 UART 空闲后再配置, 避免丢失正在发送的数据

**第 235 行**

> 关闭所有中断

**第 238 行**

> 使能 FIFO 并清空收发缓冲

</details>

**位置**：`driver/uart/uart_dw.c:220`

<a id="fn-driver-uart-uart_dw.c-242"></a>

### `dw_uart_init`

```c
void dw_uart_init(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

已初始化则返回，否则初始化收发缓冲与锁、关中断、清 FIFO 并使能接收中断。

<details>
<summary>函数内备注（8 条）</summary>

**第 253 行**

> 等待 UART 空闲后再配置, 避免丢失正在发送的数据

**第 256 行**

> 关闭所有中断

**第 259 行**

> 使能 FIFO 并清空收发队列(不改波特率, OpenSBI 已配好)

**第 262 行**

> TODO: 安装中断处理
> irq_install(DW_UART_IRQ, dw_uart_interrupt_handler);

**第 267 行**

> 使能 RX 中断

**第 270 行**

> dw_uart_enable_tx_interrupt();

**第 272 行**

> TODO: 配置 GICv3 中断触发模式和目标 CPU
> gicv3_set_int_trigger(DW_UART_IRQ, 0);  // 设置为电平触发
> gicv3_set_int_target(DW_UART_IRQ, 0x1);  // 目标 CPU 0
> gicv3_enable_int(DW_UART_IRQ, true);
> if (gicv3_is_int_enabled(DW_UART_IRQ)) {
> }

**第 281 行**

> dw_uart_initialized = true;

</details>

**位置**：`driver/uart/uart_dw.c:242`

<a id="fn-driver-uart-uart_dw.c-294"></a>

### `dw_uart_probe`

```c
static int dw_uart_probe(const device_t *dev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从设备配置取 MMIO 基址与寄存器移位，绑定串口操作集并做早期初始化。

**位置**：`driver/uart/uart_dw.c:294`

<a id="fn-driver-uart-uart_dw.c-305"></a>

### `dw_uart_start`

```c
static int dw_uart_start(const device_t *dev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

仅当配置为中断模式时调用 dw_uart_init() 启动中断驱动，并返回 0。

**位置**：`driver/uart/uart_dw.c:305`

<a id="fn-driver-uart-uart_dw.c-324"></a>

### `dw_uart_putchar_nb`

```c
bool dw_uart_putchar_nb(char c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

未初始化返回 false；加锁把换行前补的回车与字符存入发送缓冲，有入队则使能 TX 中断。

<details>
<summary>函数内备注（4 条）</summary>

**第 334 行**

> 处理换行：如果是 &#x27;\n&#x27;, 先尝试发送 &#x27;\r&#x27;

**第 337 行**

> 缓冲区满, 连 &#x27;\r&#x27; 都放不下

**第 344 行**

> 尝试放入字符到缓冲区

**第 350 行**

> 如果有数据入队, 启用 TX 中断

</details>

**位置**：`driver/uart/uart_dw.c:324`

<a id="fn-driver-uart-uart_dw.c-359"></a>

### `dw_uart_getchar_nb`

```c
bool dw_uart_getchar_nb(char *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

未初始化返回 false，否则加锁从接收缓冲取出一个字符并返回是否成功。

**位置**：`driver/uart/uart_dw.c:359`

<a id="fn-driver-uart-uart_dw.c-374"></a>

### `dw_uart_putchar`

```c
void dw_uart_putchar(char c)
```

early init 的时候会阻塞到发送完成 (等待时间未知)
int init 后会使用缓冲区和中断, 一次失败仍会多次尝试 (可以设置重试次数)

<details>
<summary>函数内备注（7 条）</summary>

**第 383 行**

> 如果 UART 尚未初始化, 直接写寄存器(阻塞模式)

**第 385 行**

> 如果是 &#x27;\n&#x27;, 先发送 &#x27;\r&#x27;

**第 392 行**

> 等待并发送字符

**第 399 行**

> 已初始化：先尝试非阻塞发送

**第 403 行**

> 缓冲区满, 重试若干次

**第 407 行**

> 短暂延迟, 让中断有机会发送数据

**第 411 行**

> 超时仍然失败, 丢弃字符

</details>

**位置**：`driver/uart/uart_dw.c:374`

<a id="fn-driver-uart-uart_dw.c-419"></a>

### `dw_uart_getchar`

```c
char dw_uart_getchar(void)
```

early init 的时候会阻塞到接收数据 (等待时间未知)
int init 后会使用缓冲区和中断, 如果无数据则睡眠 WFI

<details>
<summary>函数内备注（3 条）</summary>

**第 423 行**

> 如果已经完整初始化(中断模式), 使用缓冲区

**第 426 行**

> 等待数据到来

**第 432 行**

> 否则直接轮询硬件(early init 模式或未初始化)
> 等待数据可用

</details>

**位置**：`driver/uart/uart_dw.c:419`

<a id="fn-driver-uart-uart_dw.c-443"></a>

### `dw_uart_putstr`

```c
void dw_uart_putstr(const char *str)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

逐字符调用 dw_uart_putchar 输出整个字符串。

**位置**：`driver/uart/uart_dw.c:443`

<a id="fn-driver-uart-uart_dw.c-456"></a>

### `dw_uart_flush`

```c
void dw_uart_flush(void)
```

软件缓冲区 → [中断] → THR → TSR → 串口线
↑                    ↑      ↑
第一阶段检查          THRE   TEMT (第二阶段检查)

<details>
<summary>函数内备注（2 条）</summary>

**第 461 行**

> 第一阶段：等待软件缓冲区为空

**第 474 行**

> 第二阶段：等待硬件发送完成(THR 和 TSR 都为空)
> TEMT (Transmitter Empty) 位表示发送器完全空闲

</details>

**位置**：`driver/uart/uart_dw.c:456`

<a id="fn-driver-uart-uart_dw.c-487"></a>

### `dw_uart_rx_available`

```c
bool dw_uart_rx_available(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

未初始化时直接查硬件接收状态，否则加锁判断接收缓冲是否非空。

<details>
<summary>函数内备注（1 条）</summary>

**第 490 行**

> early 模式：直接查 LSR 硬件寄存器(中断未启用, 数据不经缓冲区)

</details>

**位置**：`driver/uart/uart_dw.c:487`

<a id="fn-driver-uart-uart_dw.c-501"></a>

### `dw_uart_tx_buffer_usage`

```c
uint32_t dw_uart_tx_buffer_usage(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

未初始化返回 0，否则加锁读取发送缓冲中的待发字节数。

**位置**：`driver/uart/uart_dw.c:501`

<a id="fn-driver-uart-uart_dw.c-512"></a>

### `dw_uart_get_stats`

```c
void dw_uart_get_stats(uint32_t *tx_irqs, uint32_t *rx_irqs, uint32_t *tx_usage, uint32_t *rx_usage)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按非空指针输出收发中断计数，并分别加锁读取发送与接收缓冲的占用数量。

**位置**：`driver/uart/uart_dw.c:512`

<a id="fn-driver-uart-uart_dw.c-529"></a>

### `dw_uart_is_tx_interrupt_enabled`

```c
bool dw_uart_is_tx_interrupt_enabled(void)
```

调试函数：检查TX中断是否启用

**位置**：`driver/uart/uart_dw.c:529`

<a id="fn-driver-uart-uart_dw.c-536"></a>

### `dw_uart_get_last_iir`

```c
uint32_t dw_uart_get_last_iir(void)
```

调试函数：获取最后一次IIR值

**位置**：`driver/uart/uart_dw.c:536`

<a id="fn-driver-uart-uart_dw.c-542"></a>

### `dw_uart_get_tx_sent_total`

```c
uint32_t dw_uart_get_tx_sent_total(void)
```

调试函数：获取总共发送的字节数

**位置**：`driver/uart/uart_dw.c:542`

<a id="mod-driver-uart-uart_dw.h"></a>

## driver/uart/uart_dw.h — DesignWare UART 基地址(由驱动 probe 从设备表设置)

`driver/uart/uart_dw.h` · 2 个函数 · 2 个有说明 · 120 行

QEMU virt 机器的 ns16550 UART 使用字节寻址(reg-shift=0),
寄存器偏移与标准 16550 PIO 布局相同：0,1,2,3,4,5,6,7。
必须使用 read8/write8(8-bit 访问), 32-bit 访问会访问到错误寄存器。

<a id="fn-driver-uart-uart_dw.h-54"></a>

### `dw_reg_r8`

```c
static inline uint8_t dw_reg_r8(uintptr_t abs_addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按平台与寄存器移位选择以 8 位或 32 位读方式访问 UART 寄存器。

**位置**：`driver/uart/uart_dw.h:54`

<a id="fn-driver-uart-uart_dw.h-64"></a>

### `dw_reg_w8`

```c
static inline void dw_reg_w8(uint8_t val, uintptr_t abs_addr)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

按平台与寄存器移位选择以 8 位或 32 位写方式访问 UART 寄存器。

**位置**：`driver/uart/uart_dw.h:64`

<a id="mod-driver-uart-uart_pl011.c"></a>

## driver/uart/uart_pl011.c — PL011 UART driver

`driver/uart/uart_pl011.c` · 18 个函数 · 18 个有说明 · 384 行

TX 始终是同步/轮询的：klog 在关中断、持 g_klog_lock 的状态下逐字符输出，
panic 路径还会绕过锁，所以发送不能依赖 TX 中断或任何可能sleep/自旋等待
的缓冲排空机制。

RX 可选中断模式：EARLY probe 只配置硬件并把所有中断源屏蔽；到了
DRV_PHASE_IRQ 的 start 阶段（GIC 已 probe）才注册 IRQ、打开 RX/RT 中断。
中断只把字节搬进 rx_buffer，不碰 tty/任务/信号，也不打日志。

宿主中断关闭的调用者（tick、VMM 直启模式的 console pump）仍然通过
serial_rx_ready()/serial_getc() 直接 drain 硬件 FIFO，所以没有 IRQ 也
不会丢输入。

<a id="fn-driver-uart-uart_pl011.c-64"></a>

### `buffer_is_empty`

```c
static bool buffer_is_empty(uart_buffer_t *buf)
```

Buffer operations（只用于 RX 软件环；调用者负责加锁）

**位置**：`driver/uart/uart_pl011.c:64`

<a id="fn-driver-uart-uart_pl011.c-69"></a>

### `buffer_is_full`

```c
static bool buffer_is_full(uart_buffer_t *buf)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断环形缓冲的 count 是否达到接收缓冲容量。

**位置**：`driver/uart/uart_pl011.c:69`

<a id="fn-driver-uart-uart_pl011.c-74"></a>

### `buffer_put`

```c
static bool buffer_put(uart_buffer_t *buf, char c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

缓冲已满时返回 false，否则写入 head 位置、前移 head 并递增 count。

**位置**：`driver/uart/uart_pl011.c:74`

<a id="fn-driver-uart-uart_pl011.c-85"></a>

### `buffer_get`

```c
static bool buffer_get(uart_buffer_t *buf, char *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

缓冲为空时返回 false，否则从 tail 取出字符、前移 tail 并递减 count。

**位置**：`driver/uart/uart_pl011.c:85`

<a id="fn-driver-uart-uart_pl011.c-97"></a>

### `uart_tx_fifo_full`

```c
static bool uart_tx_fifo_full(void)
```

UART hardware operations

**位置**：`driver/uart/uart_pl011.c:97`

<a id="fn-driver-uart-uart_pl011.c-102"></a>

### `uart_rx_fifo_empty`

```c
static bool uart_rx_fifo_empty(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

读取 UART 标志寄存器并判断接收 FIFO 空位是否置位。

**位置**：`driver/uart/uart_pl011.c:102`

<a id="fn-driver-uart-uart_pl011.c-113"></a>

### `pl011_rx_drain_locked`

```c
static uint32_t pl011_rx_drain_locked(void)
```

把硬件 RX FIFO 读空并放进软件环。调用者必须持有 rx_buffer.lock。

即使软件环已满也必须继续读硬件：PL011 的 RX 是电平中断，FIFO 里留一个
字节就会让中断线一直有效，ISR 会被反复触发。

<details>
<summary>函数内备注（1 条）</summary>

**第 117 行**

> 读 RSR 清掉 FE/PE/BE/OE 状态；溢出字节已经丢了，没有恢复手段

</details>

**位置**：`driver/uart/uart_pl011.c:113`

<a id="fn-driver-uart-uart_pl011.c-136"></a>

### `pl011_irq_handler`

```c
static void pl011_irq_handler(void *frame, void *ctx)
```

RX 中断处理：只搬字节，不打日志、不碰 tty/任务、不碰 TX。

TX 中断源始终不打开，所以这里不需要处理 UART_INT_TX。

**位置**：`driver/uart/uart_pl011.c:136`

<a id="fn-driver-uart-uart_pl011.c-156"></a>

### `pl011_init`

```c
void pl011_init(void)
```

Initialize UART hardware. All interrupt sources stay masked until start.

<details>
<summary>函数内备注（6 条）</summary>

**第 161 行**

> 基址由 probe 从设备表设置（device_mmio 已按平台规则换算）

**第 165 行**

> Disable UART first

**第 168 行**

> Clear all interrupts and mask every source

**第 172 行**

> Configure UART (115200 baud, 8N1)
> For 24MHz clock: IBRD = 24000000 / (16 * 115200) = 13
> FBRD = (0.0208 * 64) + 0.5 = 1

**第 180 行**

> 8 bits, no parity, 1 stop bit, enable FIFOs

**第 183 行**

> Enable UART, TX, RX

</details>

**位置**：`driver/uart/uart_pl011.c:156`

<a id="fn-driver-uart-uart_pl011.c-199"></a>

### `pl011_probe`

```c
static int pl011_probe(const device_t *dev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

从设备配置取 MMIO 基址，绑定 PL011 串口操作集并执行初始化。

**位置**：`driver/uart/uart_pl011.c:199`

<a id="fn-driver-uart-uart_pl011.c-216"></a>

### `pl011_start`

```c
static int pl011_start(const device_t *dev)
```

DRV_PHASE_IRQ 的 start：GIC 已在同阶段的 probe pass 里初始化完成，
此处可以安全注册并打开 UART 中断线。

失败时降级为轮询并返回 0：SERIAL 是必需 class，start 返回非 0 会触发
driver_required_panic，但串口本身仍然可用，没必要因此不启动。

<details>
<summary>函数内备注（2 条）</summary>

**第 236 行**

> 启动前已经输入、还留在硬件 FIFO 里的字节先进软件环

**第 242 行**

> 只打开 RX 和 RX timeout；TX 中断永远保持关闭

</details>

**位置**：`driver/uart/uart_pl011.c:216`

<a id="fn-driver-uart-uart_pl011.c-268"></a>

### `pl011_putchar_nb`

```c
bool pl011_putchar_nb(char c)
```

Non-blocking character output: direct FIFO write, never queue/IRQ.

**位置**：`driver/uart/uart_pl011.c:268`

<a id="fn-driver-uart-uart_pl011.c-278"></a>

### `pl011_putchar`

```c
void pl011_putchar(char c)
```

Blocking character output (bounded wait; TX stays synchronous).

<details>
<summary>函数内备注（2 条）</summary>

**第 281 行**

> Fallback to direct write if not initialized

**第 292 行**

> 仍然失败：丢弃这个字符, 只计数, 不打印。

> 这里不能打日志：pl011_putchar 是 uart_putchar 的实现, 而 klog 是
> 持着 g_klog_lock 逐字符调用它的。若在此 logger_warn → klog → 同一把
> 锁, 同一颗 CPU, 中断已关 → 自旋死锁。丢弃计数由 pl011_tx_dropped()
> 读出去。

</details>

**位置**：`driver/uart/uart_pl011.c:278`

<a id="fn-driver-uart-uart_pl011.c-304"></a>

### `pl011_putstr`

```c
void pl011_putstr(const char *str)
```

String output

**位置**：`driver/uart/uart_pl011.c:304`

<a id="fn-driver-uart-uart_pl011.c-311"></a>

### `pl011_getchar_nb`

```c
bool pl011_getchar_nb(char *c)
```

Non-blocking character input

<details>
<summary>函数内备注（1 条）</summary>

**第 323 行**

> 轮询模式：直接读硬件，不经过软件环

</details>

**位置**：`driver/uart/uart_pl011.c:311`

<a id="fn-driver-uart-uart_pl011.c-333"></a>

### `pl011_rx_available`

```c
bool pl011_rx_available(void)
```

Check if RX data is available

<details>
<summary>函数内备注（2 条）</summary>

**第 336 行**

> early 模式：直接查硬件 FR.RXFE 位(中断未启用)

**第 344 行**

> IRQ 模式下也要先 drain 硬件：VMM 直启模式运行 guest 时宿主中断
> 是关的，ISR 进不来；tick/其他消费者同样可能在这种情况下调用。

</details>

**位置**：`driver/uart/uart_pl011.c:333`

<a id="fn-driver-uart-uart_pl011.c-358"></a>

### `pl011_tx_dropped`

```c
uint64_t pl011_tx_dropped(void)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

返回累计的发送丢弃字节计数。

**位置**：`driver/uart/uart_pl011.c:358`

<a id="fn-driver-uart-uart_pl011.c-369"></a>

### `pl011_getchar`

```c
char pl011_getchar(void)
```

pl011_getchar - 阻塞式读取一个字符

IRQ 模式优先从软件环取；为空时调用 rx_available() 触发一次硬件 drain，
等待时不持锁。未初始化时直接轮询硬件 FIFO。

**位置**：`driver/uart/uart_pl011.c:369`

<a id="mod-driver-uart-uart_x86.c"></a>

## driver/uart/uart_x86.c — x86 COM1 (16550 Port I/O) UART 驱动实现

`driver/uart/uart_x86.c` · 15 个函数 · 15 个有说明 · 334 行

TX 始终同步轮询：klog 在关中断持锁的状态下逐字符输出，panic 路径还会
绕过锁，不能依赖 TX 中断。

RX 可选中断模式：EARLY probe 只配置硬件并关闭所有中断；DRV_PHASE_IRQ
的 start 阶段（LAPIC/IOAPIC 已就绪）再注册 IRQ、打开 RDI，并把 COM1
IRQ4 通过 IOAPIC GSI4 投递到 IDT 向量 0x24。

宿主中断关闭的调用者（VMM 直启模式的 console pump、tick）仍通过
serial_rx_ready()/serial_getc() 直接 drain 硬件 FIFO。

<a id="fn-driver-uart-uart_x86.c-52"></a>

### `buffer_is_empty`

```c
static bool buffer_is_empty(uart_x86_buffer_t *buf)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断环形缓冲的 count 是否为 0。

**位置**：`driver/uart/uart_x86.c:52`

<a id="fn-driver-uart-uart_x86.c-57"></a>

### `buffer_is_full`

```c
static bool buffer_is_full(uart_x86_buffer_t *buf)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

判断环形缓冲的 count 是否达到接收缓冲容量。

**位置**：`driver/uart/uart_x86.c:57`

<a id="fn-driver-uart-uart_x86.c-62"></a>

### `buffer_put`

```c
static bool buffer_put(uart_x86_buffer_t *buf, char c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

缓冲已满时返回 false，否则写入 head 位置、前移 head 并递增 count。

**位置**：`driver/uart/uart_x86.c:62`

<a id="fn-driver-uart-uart_x86.c-73"></a>

### `buffer_get`

```c
static bool buffer_get(uart_x86_buffer_t *buf, char *c)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

缓冲为空时返回 false，否则从 tail 取出字符、前移 tail 并递减 count。

**位置**：`driver/uart/uart_x86.c:73`

<a id="fn-driver-uart-uart_x86.c-85"></a>

### `uart_x86_tx_ready`

```c
static inline int uart_x86_tx_ready(void)
```

等待 TX 寄存器空

**位置**：`driver/uart/uart_x86.c:85`

<a id="fn-driver-uart-uart_x86.c-95"></a>

### `uart_x86_rx_drain_locked`

```c
static uint32_t uart_x86_rx_drain_locked(void)
```

把硬件 RX FIFO 读空并放进软件环。调用者必须持有 rx_buffer.lock。
读 LSR 同时清掉 OE/PE/FE/BI 错误位；软件环满时继续读硬件，避免
FIFO 一直保持非空。

**位置**：`driver/uart/uart_x86.c:95`

<a id="fn-driver-uart-uart_x86.c-116"></a>

### `uart_x86_irq_handler`

```c
static void uart_x86_irq_handler(void *frame, void *ctx)
```

RX 中断处理：只搬字节，不打日志、不碰 tty/任务、不碰 TX。

IIR 低 4 位：0x04 = RDI，0x0C = FIFO 接收超时；二者都表示 RBR 有数据。
THRI 不会出现（IER 只开 RDI）。

<details>
<summary>函数内备注（1 条）</summary>

**第 136 行**

> 其他中断源未启用；读 LSR 清掉可能的状态

</details>

**位置**：`driver/uart/uart_x86.c:116`

<a id="fn-driver-uart-uart_x86.c-148"></a>

### `uart_x86_early_init`

```c
void uart_x86_early_init(void)
```

uart_x86_early_init — 配置 COM1 为 115200-8N1, 关闭所有 UART 中断
QEMU 通常已初始化, 但显式配置以保证行为确定。

<details>
<summary>函数内备注（8 条）</summary>

**第 153 行（行尾）**

> 禁用所有中断

**第 154 行（行尾）**

> 置位 DLAB, 允许写除数

**第 155 行（行尾）**

> 除数低字节 = 1 → 115200

**第 156 行（行尾）**

> 除数高字节 = 0

**第 157 行（行尾）**

> 8N1, 清除 DLAB

**第 158 行（行尾）**

> 使能并清空 FIFO

**第 161 行**

> PC 兼容 16550 需要 OUT2=1 才把中断送到 IOAPIC/PIC

**第 164 行**

> 清掉可能的残留状态

</details>

**位置**：`driver/uart/uart_x86.c:148`

<a id="fn-driver-uart-uart_x86.c-174"></a>

### `uart_x86_putchar`

```c
void uart_x86_putchar(char c)
```

uart_x86_putchar — 发送单个字符(带 \n → \r\n 转换)

**位置**：`driver/uart/uart_x86.c:174`

<a id="fn-driver-uart-uart_x86.c-187"></a>

### `uart_x86_putstr`

```c
void uart_x86_putstr(const char *s)
```

uart_x86_putstr — 发送字符串

**位置**：`driver/uart/uart_x86.c:187`

<a id="fn-driver-uart-uart_x86.c-196"></a>

### `uart_x86_getchar`

```c
char uart_x86_getchar(void)
```

uart_x86_getchar — 阻塞读取单个字符

**位置**：`driver/uart/uart_x86.c:196`

<a id="fn-driver-uart-uart_x86.c-218"></a>

### `uart_x86_getchar_nb`

```c
bool uart_x86_getchar_nb(char *c)
```

uart_x86_getchar_nb — 非阻塞读取, 无数据时返回 false

**位置**：`driver/uart/uart_x86.c:218`

<a id="fn-driver-uart-uart_x86.c-241"></a>

### `uart_x86_rx_available`

```c
bool uart_x86_rx_available(void)
```

uart_x86_rx_available — 检查是否有可读数据

<details>
<summary>函数内备注（1 条）</summary>

**第 251 行**

> VMM 直启/tick 等宿主中断关闭的场景靠这里 drain 硬件

</details>

**位置**：`driver/uart/uart_x86.c:241`

<a id="fn-driver-uart-uart_x86.c-270"></a>

### `uart_x86_probe`

```c
static int uart_x86_probe(const device_t *dev)
```

> 自动摘要（来自 `docs/api/auto_summaries.json`，不是源码中的人工注释）

忽略设备参数（COM1 端口固定），绑定 x86 串口操作集并做早期初始化。

<details>
<summary>函数内备注（1 条）</summary>

**第 272 行（行尾）**

> COM1 是固定端口，不需要平台 MMIO 配置

</details>

**位置**：`driver/uart/uart_x86.c:270`

<a id="fn-driver-uart-uart_x86.c-285"></a>

### `uart_x86_start`

```c
static int uart_x86_start(const device_t *dev)
```

DRV_PHASE_IRQ 的 start：LAPIC/IOAPIC 已在同阶段的 probe pass 里初始化，
此处可以注册并打开 COM1 的中断线。

失败时降级为轮询并返回 0：SERIAL 是必需 class，start 返回非 0 会触发
driver_required_panic，但串口本身仍然可用，没必要因此不启动。

<details>
<summary>函数内备注（2 条）</summary>

**第 305 行**

> 启动前已经输入、还留在 FIFO 里的字节先进软件环

**第 311 行**

> 只打开 RDI；TX 中断永远保持关闭

</details>

**位置**：`driver/uart/uart_x86.c:285`
