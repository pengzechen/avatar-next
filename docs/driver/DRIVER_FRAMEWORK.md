# 驱动框架 v1 设计（轻量版）

> 状态：**阶段 1 已实现**（2026-10-23）；阶段 2/3 待做。
> 本文描述设计；文末 §16 记录阶段 1 的实际落地情况与偏差。
> 目标版本：阶段 1（生命周期框架）；阶段 2/3 的最终形态在文中一并给出，便于 review 方向。

---

## 0. 摘要

引入一个约 300 行的驱动核心：

- 驱动用 `DRIVER_ENTRY` 自注册到 `.drivers` 只读段；
- 平台设备由 `platform.conf` 生成成静态的 `g_platform_devices[]`；
- 启动时按 6 个阶段调用 `drivers_init(phase)`，`kernel/main.c` 不再有 `#if DRIVER_*` 初始化梯子；
- `kernel/`、`fs/`、`vmm/` 只能 include `include/driver/*.h`，看不到驱动私有头；
- 平台差异变成 `platform.conf` 数据，不再散落 `#if PLATFORM_*`。

明确不做：设备树解析、总线匹配、热插拔、deferred probe、电源管理、模块加载、sysfs、引用计数。一次构建每个 class 只有一个后端（目前的真实情况）。

---

## 1. 目标与非目标

### 目标

1. 新增/替换一个驱动，不再需要改 `kernel/main.c`。
2. 驱动的初始化顺序由“阶段”表达，而不是注释 + `#if`。
3. `kernel/`、`fs/`、`vmm/` 只依赖公共接口，不依赖具体驱动。
4. 平台地址/中断号/硬件差异只出现在 `platform.conf`（以及极少数 arch 代码）。
5. 可选驱动（DMA/ION/NPU/TPU/网络）缺失时，上层仍能编译并返回明确的 `-ENOSYS`，而不是靠 `#if` 切代码。

### 非目标

- 不实现 Linux 的设备模型（`struct device` 树、bus、driver core、kobject、sysfs、uevent）。
- 不支持运行时加载/卸载、热插拔、多实例枚举（需要时再扩，接口不变）。
- 不引入 DTB 解析；`platform.conf` 仍是唯一设备描述来源。
- 不改变现有驱动的数据路径语义；阶段 1 是纯结构调整。

---

## 2. 设计决策（review 重点）

| 编号 | 决策 | 理由 | 备选 |
|------|------|------|------|
| D1 | 驱动用链接器段 `.drivers` 自注册 | 构建选择（`UART=dw` 等）自动反映到注册表，不会与 `platform.conf` 名字失配 | 由 Makefile/gen 脚本生成注册表；多一处不一致风险 |
| D2 | 绑定只按 **class**，不按驱动名 | 一次构建每个 class 只有一个后端；命令行覆盖不会失配 | 按 compatible 字符串匹配（更接近 DT，但复杂） |
| D3 | 设备数据用 typed cfg（`serial_cfg_t` 等），由 `gen_platform.py` 生成 | 编译期类型检查，驱动不再字符串查 key | 继续用 `platform_static_kv` 的 key/value（改动小但弱类型） |
| D4 | 初始化分 6 个阶段，同阶段先 probe 后 start | UART 的“早期轮询 → 中断模式”和 timer 的 init/enable 都能表达 | 单一 init + 驱动内部排序（回到隐式依赖） |
| D5 | 公共头集中在 `include/driver/`，内核编译路径移除 `-Idriver` | 从构建上强制“只暴露接口” | 约定 + code review（不可靠） |
| D6 | 可选服务用弱符号兜底 | 去掉 fs/syscall 里大量 `#if DRIVER_*`，缺服务时返回 `-ENOSYS` | 保留 feature 宏 `#if HAVE_*`（仍是条件编译） |
| D7 | v1 不改数据路径 | 第一次改动只移动初始化，便于用现有门禁验证 | 一次性重写接口（风险大） |

---

## 3. 目录与文件

### 新增

```
include/driver/
    driver.h            # drv_class_t / drv_phase_t / device_t / driver_t / DRIVER_ENTRY
    serial.h            # serial_cfg_t + serial_ops_t（阶段 2）
    irq.h               # irq_cfg_t + irq_ops_t（阶段 2）
    time.h              # time_cfg_t（阶段 1 只需 cfg；API 沿用 timer）
    block.h             # block_cfg_t + block_root_device()
    net.h               # net_cfg_t（接口沿用 net/netdev.h）
    npu.h               # npu_cfg_t + npu_present()（阶段 2）
    tpu.h               # tpu_cfg_t（阶段 2）
    dma.h / ion.h       # 服务能力（阶段 2）

driver/core/
    driver.c            # 注册表遍历、阶段、绑定状态、panic 策略
    device.c            # g_platform_devices 查找、device_mmio()
    stubs.c             # 阶段 2：DMA/ION/NPU/TPU 弱符号兜底

driver/drivers.mk       # 汇总各 class 的 drivers.mk
```

### 修改

```
kernel/main.c           # 初始化梯子 → drivers_init(phase)
kernel/task/cpu.c       # 阶段 3：PLATFORM_* → platform.conf
kernel/mm/aarch64/vm_early.c
include/guest_loader.h
driver/uart/*.c         # 加 DRIVER_ENTRY + probe/start
driver/irq/*.c
driver/timer/timer.c
driver/blk/*.c
driver/eth/*.c
driver/npu/rknpu.c
driver/tpu/cvi_tpu.c
driver/dma/*.c
driver/ion/ion.c
platforms/*/platform.conf
tools/gen_platform.py    # 生成 g_platform_devices[]
boot/*/link.ld           # .drivers 段
Makefile                 # §6 下沉到 driver/*/drivers.mk
```

### 删除（阶段 2 之后）

```
driver/irq/irq.h                 # 私有身份改为 driver/irq/irq_priv.h 或合并
driver/timer/timer.h             # 公共 API 移到 include/driver/time.h
driver/uart/uart.h               # 公共 API 移到 include/driver/serial.h
kernel/main.c 里的 #include "eth/..." / "npu/..." / "tpu/..." / "blk/..."
```

---

## 4. 核心数据结构

### 4.1 `include/driver/driver.h`（完整草案）

```c
#ifndef DRIVER_DRIVER_H
#define DRIVER_DRIVER_H

#include "types.h"

typedef enum {
    DRV_SERIAL = 0,
    DRV_IRQ,
    DRV_TIME,
    DRV_STORAGE,
    DRV_NET,
    DRV_DMA,        /* service：无设备 */
    DRV_ION,        /* service：无设备 */
    DRV_NPU,
    DRV_TPU,
    DRV_CLASS_COUNT
} drv_class_t;

typedef enum {
    DRV_PHASE_EARLY = 0,   /* 串口，先于 klog */
    DRV_PHASE_IRQ,         /* 中断控制器；串口切中断模式 */
    DRV_PHASE_TIME,        /* 时间基准 */
    DRV_PHASE_STORAGE,     /* 根块设备，先于 fs_init */
    DRV_PHASE_CORE,        /* DMA/ION/NPU/TPU */
    DRV_PHASE_NET,         /* 调度器就绪之后 */
    DRV_PHASE_COUNT
} drv_phase_t;

typedef struct device {
    const char *name;      /* "uart0" —— 仅用于日志/诊断 */
    drv_class_t cls;
    const void *cfg;       /* 指向该 class 的 cfg 结构 */
} device_t;

typedef struct driver {
    const char *name;              /* "pl011" —— 仅用于日志/诊断 */
    drv_class_t cls;
    drv_phase_t phase;             /* probe 阶段 */
    drv_phase_t start_phase;       /* start 阶段；DRV_PHASE_COUNT 表示没有 */
    int  (*probe)(const device_t *dev);
    int  (*start)(const device_t *dev);
} driver_t;

#define DRIVER_ENTRY(sym) \
    const driver_t sym __attribute__((used, section(".drivers"), aligned(8)))

/* 由 driver/core 实现 */
void drivers_init(drv_phase_t phase);
bool driver_present(drv_class_t cls);
const device_t *driver_device(drv_class_t cls);
uintptr_t device_mmio(uintptr_t raw_pa);

/* 由 gen_platform.py 生成到 platform_static.c */
extern const device_t g_platform_devices[];
extern const unsigned g_platform_device_count;

#endif /* DRIVER_DRIVER_H */
```

设计说明：

- `device_t` 不包含 `base/irq` 字段，所有硬件参数在 class 的 cfg 里。避免“有的设备用 base、有的用 props”的两种风格。
- `driver_t.start_phase` 允许一个驱动有两个初始化点，正好覆盖 UART 的 early/full 两段。
- `driver_device()` 只按 class 取；一次构建一个 class 一个后端。`device_t.name` 只用于日志。

### 4.2 各 class 的 cfg（阶段 1 需要的最小集合）

```c
/* include/driver/serial.h */
typedef struct {
    uintptr_t base;         /* 原始物理地址；驱动用 device_mmio() 换算 */
    uint32_t  reg_shift;
    uint32_t  irq;          /* 0 = 暂不接中断 */
    bool      irq_mode;     /* true：start 阶段切到中断模式 */
    const char *compatible; /* 可选 sanity check，如 "arm,pl011" */
} serial_cfg_t;

/* include/driver/irq.h */
typedef struct {
    uintptr_t gicd, gicr, gicc, gich;
    uintptr_t plic, clint;
    bool      virt;         /* 是否提供 irq_virt_ops（GICv3） */
} irq_cfg_t;

/* include/driver/time.h */
typedef struct {
    uint32_t freq_hz;
    uint32_t tick_ms;
    uint32_t cntp;
    uint64_t counter_hz;
} time_cfg_t;

/* include/driver/block.h */
typedef struct {
    uintptr_t base;         /* ramblk：rootfs 物理基址 */
    uint64_t  size;
    uintptr_t sd_base;      /* sdblk */
    uintptr_t top_base;
    uint32_t  pwrsw_off;
} block_cfg_t;

/* include/driver/net.h */
typedef struct {
    uintptr_t base;
    uint32_t  irq;
    const char *compatible;
} net_cfg_t;

/* include/driver/npu.h */
typedef struct {
    uintptr_t base[3];
    uint32_t  irq[3];
    uintptr_t pmu;
} npu_cfg_t;

/* include/driver/tpu.h */
typedef struct {
    uintptr_t tdma_base;
    uintptr_t tiu_base;
    uint32_t  irq;
} tpu_cfg_t;
```

DMA/ION 是 service，没有 cfg。

---

## 5. 核心实现草案

### 5.1 `driver/core/device.c`

```c
#include "driver/driver.h"
#include "platform_cfg.h"
#include "mm_vm.h"      /* KERNEL_VMA */

const device_t *driver_device(drv_class_t cls)
{
    for (unsigned i = 0; i < g_platform_device_count; i++)
        if (g_platform_devices[i].cls == cls)
            return &g_platform_devices[i];
    return NULL;
}

uintptr_t device_mmio(uintptr_t raw_pa)
{
    if (raw_pa == 0)
        return 0;
    return g_mmio_needs_vma ? raw_pa + KERNEL_VMA : raw_pa;
}
```

### 5.2 `driver/core/driver.c`

```c
#include "driver/driver.h"
#include "klog.h"
#include "platform_ops.h"

extern const driver_t __start_drivers[], __stop_drivers[];

static const driver_t *g_bound[DRV_CLASS_COUNT];

/* 没有设备、但需要初始化的服务 */
static bool driver_is_service(drv_class_t cls)
{
    return cls == DRV_DMA || cls == DRV_ION;
}

/* 有设备就必须有驱动的 class */
static bool driver_requires_device(drv_class_t cls)
{
    return cls == DRV_SERIAL || cls == DRV_IRQ ||
           cls == DRV_TIME   || cls == DRV_STORAGE;
}

static const driver_t *driver_of_class(drv_class_t cls)
{
    for (const driver_t *d = __start_drivers; d < __stop_drivers; d++)
        if (d->cls == cls)
            return d;
    return NULL;
}

void drivers_init(drv_phase_t phase)
{
    /* 先检查“平台声明了设备、但构建里没有对应驱动” */
    for (unsigned i = 0; i < g_platform_device_count; i++) {
        const device_t *dev = &g_platform_devices[i];
        if (!driver_requires_device(dev->cls))
            continue;
        const driver_t *d = driver_of_class(dev->cls);
        if (d && d->phase == phase && !d->probe)
            panic("driver %s has no probe", d->name);
        if (!d && phase == DRV_PHASE_EARLY)
            ; /* EARLY 阶段只检查串口；其余 class 在各自阶段检查 */
    }

    /* 第一遍：probe */
    for (const driver_t *d = __start_drivers; d < __stop_drivers; d++) {
        if (d->phase != phase)
            continue;

        const device_t *dev = driver_device(d->cls);
        if (!dev && !driver_is_service(d->cls)) {
            KLOG_WARN("[drv] %s: no matching device, skipped\n", d->name);
            continue;
        }
        if (!d->probe) {
            g_bound[d->cls] = d;
            continue;
        }

        int rc = d->probe(dev);
        if (rc != 0) {
            KLOG_ERROR("[drv] %s: probe failed (%d)\n", d->name, rc);
            if (driver_requires_device(d->cls))
                panic("required driver %s failed", d->name);
            continue;
        }
        g_bound[d->cls] = d;
        KLOG_INIT("[drv] %s: %s ready\n", d->name, dev ? dev->name : "service");
    }

    /* 第二遍：start（保证同阶段内先 probe 后 start） */
    for (const driver_t *d = __start_drivers; d < __stop_drivers; d++) {
        if (d->start_phase != phase || !d->start)
            continue;
        int rc = d->start(driver_device(d->cls));
        if (rc != 0 && driver_requires_device(d->cls))
            panic("driver %s start failed (%d)", d->name, rc);
    }
}

bool driver_present(drv_class_t cls)
{
    return cls < DRV_CLASS_COUNT && g_bound[cls] != NULL;
}
```

需要 review 的两点：

1. `panic()` 在 EARLY 阶段可能还没有串口可用。实际不会：EARLY 的唯一驱动就是串口，probe 失败时 `serial_puts` 会落到 null ops（静默），然后停机。可以接受，或者让 EARLY 失败直接 `for (;;) wfi`。
2. `driver_requires_device()` 目前把 IRQ 也列为必需；但 `irq.driver = none` 的平台不应该生成 IRQ 设备（见第 7 节的生成规则），所以不会误判。若设备存在但驱动缺失，probe 会 panic。

### 5.3 `.drivers` 段

三个 `boot/<arch>/link.ld` 的 `.rodata` 内加：

```ld
.rodata : {
    *(.rodata*)
    . = ALIGN(8);
    __start_drivers = .;
    KEEP(*(.drivers))
    __stop_drivers = .;
}
```

- `.drivers` 在两趟链接里都存在，所以不会造成 stage-1/final 的布局差异；`.text` 不动，kallsyms `--verify` 应继续通过。
- RISC-V 的 `.rodata` 有 `AT(...)`，段内新符号仍按链接地址使用，没问题。

---

## 6. 驱动描述符清单（阶段 1）

| 驱动 | class | phase | start_phase | probe 做什么 |
|------|-------|-------|-------------|--------------|
| uart_pl011 | SERIAL | EARLY | IRQ（仅 `cfg->irq_mode`） | `pl011_init(cfg)`：早期只配置硬件、屏蔽中断；start 里注册 RX IRQ |
| uart_dw | SERIAL | EARLY | IRQ（仅 `cfg->irq_mode`） | `dw_uart_early_init(cfg)` |
| uart_x86 | SERIAL | EARLY | IRQ（仅 `cfg->irq_mode`） | `uart_x86_early_init(cfg)`：早期配置 COM1、关闭中断；start 里注册 RX IRQ |
| gicv3 | IRQ | IRQ | none | `gicv3_init(cfg)` |
| gicv2 | IRQ | IRQ | none | `gic_virtual_init(cfg)` |
| plic | IRQ | IRQ | none | `plic_init(cfg)` |
| lapic | IRQ | IRQ | none | `lapic_init(cfg)`（幂等；x86 的早期调用保留） |
| timer | TIME | TIME | TIME | `timer_init(cfg)`；start = `timer_enable()` |
| ramblk | STORAGE | STORAGE | none | `ramblk_init(cfg)` + `block_register_root()` |
| sdblk | STORAGE | STORAGE | none | `sdblk_init(cfg)` + `block_register_root()`；bdev open 不再重复 init |
| virtio_net | NET | NET | none | `eth_init(cfg)` + 注册 netdev |
| cvitek_eth | NET | NET | none | `cvitek_eth_init(cfg)` + 注册 netdev |
| dma_buf | DMA | CORE | none | service probe（当前 lazy init，可为空） |
| ion | ION | CORE | none | service probe（当前 lazy init，可为空） |
| rknpu | NPU | CORE | none | `rknpu_init(cfg)` |
| cvi_tpu | TPU | CORE | none | `cvi_tpu_init(cfg)` |

说明：

- **UART 的 `irq_mode`** 用来替代 `#if DRIVER_UART_DW && (PLATFORM_RK3588 || PLATFORM_SG2002)`。qemu-riscv 的 `irq_mode=false`，rk3588/sg2002 为 true（保持当前调用 `dw_uart_init()` 的行为）。PL011 和 x86 COM1 在 `irq_mode=true` 时由 IRQ 阶段的 start 注册 RX 中断；TX 始终同步轮询，不依赖中断（klog/panic 要求）。
- **x86 外部中断号**：公共 IRQ 号空间是 IDT 向量。IOAPIC GSI n 映射到向量 `IDT_IRQ_BASE + n`（0x20+n）；0x20 已被 LAPIC timer 使用，所以外部线用 0x21-0x2F。COM1 是 ISA IRQ4 → GSI4 → 向量 0x24，`irq.ioapic` 提供 IOAPIC MMIO 基址（QEMU q35/microvm 均为 0xFEC00000）。当前只覆盖 QEMU 的“ISA IRQ 直连 GSI”模型；没有解析 ACPI/MADT，真机的中断源重映射需要另加。
- **sdblk**：阶段 1 顺手改成“只初始化一次”。当前 `fs_init()` → `sdblk_bdev_open()` → `sdblk_init()`，随后 main 又调一次；框架改为 STORAGE probe 初始化一次，`sdblk_bdev_open()` 检测到已初始化直接返回。
- **lapic**：x86 的 `exception_init()` 为了 IDT/早期中断仍需调用 `lapic_init()`；IRQ 阶段的 probe 再调用一次（写 TPR/SVR，幂等）。如果不想重复，可加 `g_lapic_ready` 守卫。

---

## 7. 设备表生成

### 7.1 platform.conf 规范化（阶段 1 必须做的部分）

当前 `irq.driver` 在 riscv/x86 上是 `none`，PLIC 靠 ARCH 推、LAPIC 靠布尔。框架不猜，改为显式：

```conf
irq = { driver = "gicv3" }   # 或 gicv2 / plic / lapic / none
```

新增/统一的字段：

```conf
uart = {
    driver    = "pl011" | "dw" | "x86",
    base      = ...,
    reg_shift = 0 | 2,
    irq       = 33,         # 0 = 不接中断；GIC INTID / PLIC source / IDT vector
    irq_mode  = true,       # true 时在 DRV_PHASE_IRQ 调 start（PL011/x86 RX IRQ）
}

irq = {
    ...
    ioapic = 0xFEC00000,    # x86 IOAPIC MMIO 基址；其它架构为 0
}

block = { driver = "ramblk" | "sdblk" }

eth = { driver = "virtio" | "cvitek" | "none", base = ..., irq = 0 }
```

五个平台的取值：

| platform | irq.driver | block.driver | uart.irq_mode |
|----------|------------|--------------|---------------|
| qemu-virt-aarch64 | gicv3 | ramblk | true |
| qemu-virt-riscv64 | plic | ramblk | false |
| qemu-virt-x86_64 | lapic | ramblk | true |
| rk3588-aarch64 | gicv3 | ramblk | true |
| sg2002-riscv64 | plic | sdblk | true |

### 7.2 生成的 `platform_static.c`

`gen_platform.py` 在现有 kv/reserves 之后追加：

```c
#include "driver/driver.h"
#include "driver/serial.h"
#include "driver/irq.h"
#include "driver/time.h"
#include "driver/block.h"
#include "driver/net.h"
#include "driver/npu.h"
#include "driver/tpu.h"

static const serial_cfg_t g_serial_cfg = {
    .base = 0x09000000, .reg_shift = 0, .irq = 33,
    .irq_mode = true, .compatible = "arm,pl011",
};
static const irq_cfg_t g_irq_cfg = {
    .gicd = 0x08000000, .gicr = 0x080A0000, .ioapic = 0, .virt = true,
};
static const time_cfg_t g_time_cfg = {
    .freq_hz = 100, .tick_ms = 10, .cntp = 26, .counter_hz = 0,
};
static const block_cfg_t g_block_cfg = {
    .base = 0x5FE00000, .size = 0x10200000,
};
static const net_cfg_t g_net_cfg = {
    .base = 0x0A003E00, .irq = 0, .compatible = "virtio,mmio",
};

const device_t g_platform_devices[] = {
    { "uart0",  DRV_SERIAL,  &g_serial_cfg },
    { "irq0",   DRV_IRQ,     &g_irq_cfg    },
    { "time0",  DRV_TIME,    &g_time_cfg   },
    { "block0", DRV_STORAGE, &g_block_cfg  },
    { "eth0",   DRV_NET,     &g_net_cfg    },
    /* npu/tpu 按 platform.conf 是否存在对应块决定是否追加 */
};
const unsigned g_platform_device_count =
    sizeof(g_platform_devices) / sizeof(g_platform_devices[0]);
```

生成规则：

- `serial/irq/time/block/net` 这五类在现有平台都必然存在；`driver = "none"`/空时跳过该设备。
- `npu` 块存在 → 追加 `npu0`；`tpu` 块存在 → 追加 `tpu0`。
- DMA/ION 是 service，不进设备表；由编译进来的驱动在 CORE 阶段 `probe(NULL)`。
- `base` 一律写 **原始物理地址**，驱动通过 `device_mmio()` 换算；这样 `mmio_vma` 规则只存在一处。

---

## 8. `kernel/main.c` 改造前后

### 改造前（节选）

```c
#if DRIVER_UART_DW && (defined(PLATFORM_RK3588) || defined(PLATFORM_SG2002))
    dw_uart_init();
#endif
#if DRIVER_GIC_V2
    gic_virtual_init();
#endif
#if DRIVER_GIC_V3
    gicv3_init();
#endif
#if ARCH_RISCV64
    plic_init();
#endif
    timer_init();
    timer_enable();
#if DRIVER_TPU_CVITPU
    cvi_tpu_init();
#endif
#if DRIVER_SDBLK_SG2002
    sdblk_init();
#endif
#if DRIVER_NPU_RKNPU
    rknpu_init();
#endif
```

### 改造后

```c
void kernel_main(void)
{
    platform_conf_init();
    platform_early_init();              /* 内部：drivers_init(DRV_PHASE_EARLY) */

#if ARCH_X86_64
    exception_init();
#elif ARCH_RISCV64
    exception_init();
#endif

    klog_init();
    init_cache();
    pmm_initialize(...);

    drivers_init(DRV_PHASE_IRQ);        /* GIC/PLIC/LAPIC；UART start */
    drivers_init(DRV_PHASE_TIME);       /* timer init + enable */
    drivers_init(DRV_PHASE_STORAGE);    /* ramblk/sdblk，先于 fs */
    fs_init();
    drivers_init(DRV_PHASE_CORE);       /* DMA/ION/NPU/TPU */

    cpu_init_bsp();
    task_init();
    cpu_bring_up_all();

    drivers_init(DRV_PHASE_NET);
    if (driver_present(DRV_NET))
        net_init();
    ...
}
```

注意：

- `platform_early_init()` 的实现从 `uart_init()` 改成 `drivers_init(DRV_PHASE_EARLY)`；`platform_ops.h` 的契约不变。
- `#if ARCH_*` 只剩异常向量这种真正的架构启动代码；`#if DRIVER_*`/`#if PLATFORM_*` 不再出现在驱动初始化里。
- 启动日志顺序会变化（IRQ/TIME 提前到 fs 之前）。唯一的语义变化是 sdblk 不再重复初始化；这一点单独在评审中确认。
- guest 变体下不跑 `DRV_PHASE_NET`（保持当前行为），用 `test_boot_skip_net()`（tests/unit-test 启动钩子）包住即可；这不是平台判断。

---

## 9. 子系统接口

### 9.1 串口（阶段 2）

```c
/* include/driver/serial.h */
typedef struct {
    void (*putc)(char c);
    void (*puts)(const char *s);
    char (*getc)(void);
    bool (*getc_nb)(char *c);
    bool (*rx_ready)(void);
} serial_ops_t;

void serial_bind(const serial_ops_t *ops);
extern const serial_ops_t *g_serial_ops;

static inline void serial_putc(char c) { g_serial_ops->putc(c); }
static inline void serial_puts(const char *s) { g_serial_ops->puts(s); }
```

- `g_serial_ops` 初值指向内部 null ops，EARLY probe 后指向真实实现。
- `platform_ops.c` 的 `uart_putchar/uart_putstr` 变成 `serial_putc/serial_puts` 的薄包装，klog 无需改动。
- 消费方（tty、vmm_console、syscall）从 `#include "uart/uart.h"` 改为 `#include "driver/serial.h"`。

### 9.2 中断（阶段 2）

```c
typedef void (*irq_fn_t)(void *frame, void *ctx);

typedef struct {
    int  (*init)(const device_t *dev);
    int  (*secondary_init)(void);
    int  (*register_handler)(unsigned irq, irq_fn_t fn, void *ctx);
    void (*enable)(unsigned irq, bool on);
    void (*set_priority)(unsigned irq, unsigned prio);
    uint32_t (*ack)(void *frame);
    uint32_t (*id)(uint32_t token);
    void (*eoi)(uint32_t token);
    int  (*send_ipi)(unsigned irq, unsigned cpu);
} irq_ops_t;

int irq_register(unsigned irq, irq_fn_t fn, void *ctx);
void irq_enable(unsigned irq, bool on);
```

- GIC：`ack/id/eoi` 走 IAR/EOIR；PLIC：claim/complete；LAPIC：`ack` 从 frame 取 vector，`eoi` 写 LAPIC。
- 三套 arch handler 表/EOI 逻辑收敛到 `driver/irq/irq.c`，同时修 spurious 1023、SGI EOI 丢 CPUNumber、`irq_install` 越界。
- `irq_virt_ops_t`（GICv3 提供）用于 VMM：`kernel/vmm/aarch64/virq.c` 改成 `if (!irq_virt_ops()) return -ENODEV;`，去掉 `#if DRIVER_GIC_V3`。
- timer 改为通过 `irq_register/irq_set_priority/irq_enable` 注册 CNTP，不再 `extern gic_set_ipriority`。

这一块风险最高，单独作为阶段 2 的最后一个提交，并跑完整中断/调度门禁。

### 9.3 时间

- 公共 API 从 `driver/timer/timer.h` 移到 `include/driver/time.h`，函数名不变（`timer_*`）。
- 驱动 probe = `timer_init(cfg)`，start = `timer_enable()`。
- `timer_read_counter()` 等热路径继续走 `include/<arch>/` 内联寄存器读，不引入每 tick 间接调用。
- 次核初始化：`timer_init_secondary()` 保留，cpu.c 改为调用公共函数（不再 include 驱动私有头）。

### 9.4 块设备

```c
/* include/driver/block.h */
struct ext4_blockdev;
void block_register_root(struct ext4_blockdev *bdev);
struct ext4_blockdev *block_root_device(void);
```

- `ramblk/sdblk` probe 调用 `block_register_root()`。
- `fs/lwext4_port/fs_init.c` 只问 `block_root_device()`；删除 `#if DRIVER_SDBLK` 和 `ramblk_get_bdev/sdblk_get_bdev`。
- `kernel/mm/pmm.c` 不再 include `driver/blk/ramblk_cfg.h`；rootfs 保留区改用 `platform_cfg.h` 的 `g_mem_rootfs_*`。

### 9.5 网络

- 接口沿用 `include/net/netdev.h`（`netdev_t`），驱动 probe 负责 `netdev_register()`。
- `main.c` 用 `driver_present(DRV_NET)` 决定是否创建 net-poll 任务/调用 `net_init()`。
- 阶段 2 再把 `net_cfg_t` 暴露给驱动，替换 `DEVICE_ETH_BASE_RAW`/`platform_get_mmio` 两种写法。

### 9.6 可选服务（阶段 2）

`driver/core/stubs.c` 提供弱定义，真实驱动提供强定义：

```c
__attribute__((weak)) bool dma_buf_available(void) { return false; }
__attribute__((weak)) bool ion_available(void) { return false; }
__attribute__((weak)) bool npu_present(void) { return false; }
__attribute__((weak)) bool tpu_present(void) { return false; }
```

目标：把 `kernel/syscall/fs/file_ops.c`、`fd_pool.c`、`mmap.c`、`pseudofs/*`、`kernel/vmm/vdev/npu/vnpu.c` 里的 `#if DRIVER_ION/DMA_BUF/NPU_RKNPU/TPU_CVITPU` 全部改成运行时能力查询。缺失时返回 `-ENOSYS`/`-ENODEV`，而不是不编译。

---

## 10. 构建系统

### 10.1 选择下沉到 `driver/*/drivers.mk`

顶层 `Makefile` §6 删除，改为：

```make
include driver/drivers.mk
```

`driver/drivers.mk`：

```make
DRIVER_SRCS := driver/core/driver.c driver/core/device.c driver/core/stubs.c
include driver/serial/drivers.mk
include driver/irq/drivers.mk
include driver/time/drivers.mk
include driver/storage/drivers.mk
include driver/net/drivers.mk
include driver/dma/drivers.mk
include driver/ion/drivers.mk
include driver/npu/drivers.mk
include driver/tpu/drivers.mk
```

`driver/serial/drivers.mk`：

```make
UART ?= $(DEV_UART_TYPE)
ifeq ($(UART),pl011)
    DRIVER_SRCS += driver/uart/uart_pl011.c
else ifeq ($(UART),dw)
    DRIVER_SRCS += driver/uart/uart_dw.c
else ifeq ($(UART),x86)
    DRIVER_SRCS += driver/uart/uart_x86.c
else ifneq ($(UART),)
    $(error Invalid UART. Use: pl011, dw or x86)
endif
```

`driver/irq/drivers.mk` 用 `IRQ ?= $(DEV_IRQ_TYPE)` 映射 `gicv3/gicv2/plic/lapic/none`，不再靠 `ARCH` 推 PLIC、不再用 `DEV_NEED_LAPIC`。

### 10.2 include 路径

- 删除全局 `CFLAGS += -Idriver`。
- 公共头：`#include "driver/serial.h"`，只依赖 `-Iinclude`。
- 私有头：各驱动对象按需加 `-Idriver/<class>`，由 `drivers.mk` 或逐文件规则提供。
- 新增检查：

```make
driver-layering-check:
	@! grep -rnE '#include *"(uart|irq|timer|eth|blk|npu|tpu|ion|dma)/' \
	       kernel/ include/ | grep -v '^include/driver/'
```

阶段 3 再加：

```make
	@! grep -rnE '#if(n?def)? .*PLATFORM_' kernel/ driver/ fs/ | grep -v platform_cfg.h
```

### 10.3 逐文件额外 flags

现在 `DRIVER_RULE` 按组给 `CFLAGS/-Idriver/tpu/-Ikernel/mm/-DLWEXT4_CFLAGS`。改为：

```make
DRIVER_EXTRA_FLAGS_driver_blk_ramblk.c   := $(LWEXT4_CFLAGS) -Idriver/blk
DRIVER_EXTRA_FLAGS_driver_blk_sdblk.c    := $(LWEXT4_CFLAGS) -Idriver/blk
DRIVER_EXTRA_FLAGS_driver_dma_dma_buf.c  := -Idriver/dma -Ikernel/mm
...
```

规则里用 `$($(DRIVER_EXTRA_FLAGS_$(notdir $<)))` 取。保持现有的产物命名 `build/<p>/driver/<basename>.o` 不变。

---

## 11. 去 `#if` 清单

### 11.1 阶段 1（本设计）

| 位置 | 现在 | 改成 |
|------|------|------|
| `kernel/main.c:94` | `DRIVER_UART_DW && PLATFORM_RK/SG` | serial `start_phase` + `cfg.irq_mode` |
| `kernel/main.c` 其余驱动初始化 | `#if DRIVER_*` | `drivers_init(phase)` |
| `kernel/main.c` 驱动 include | `"eth/virtio_net.h"` 等 | 删除；只留 `driver/driver.h` |
| `Makefile` | PLIC 按 ARCH、LAPIC 按布尔 | `irq.driver` |
| `Makefile` | `ifeq ($(PLATFORM),sg2002)` 选 sdblk | `block.driver` |

### 11.2 阶段 2（接口收口）

| 位置 | 现在 | 改成 |
|------|------|------|
| `kernel/syscall/fs/file_ops.c` | `#if DRIVER_ION` | `ion_available()` + 弱符号 |
| `kernel/syscall/fs/fd_pool.c` | `#if DRIVER_ION/DMA_BUF` | 能力查询 + 弱符号 |
| `kernel/syscall/mm/mmap.c` | `#if DRIVER_ION/DMA_BUF/NPU` | 运行时分支 + 弱符号 |
| `kernel/fs/pseudofs/dev.c` | `#if DRIVER_ION/TPU/DMA_BUF` | 运行时能力 + 弱符号 |
| `kernel/fs/pseudofs/pseudofs.c` | `#if DRIVER_DMA_BUF/NPU` | 节点表按 `driver_present()` 动态注册 |
| `kernel/vmm/aarch64/virq.c` | `#if DRIVER_GIC_V3` | `irq_virt_ops() != NULL` |
| `kernel/vmm/vdev/npu/vnpu.c` | `#if DRIVER_NPU_RKNPU` | `npu_present()` + `g_npu_ops` |
| `kernel/task/cpu.c` | `#if DRIVER_GIC_V2/V3` 调试 dump | `irq_debug_dump()`（可选 ops） |

### 11.3 阶段 3（平台数据化）

| 位置 | 现在 | 改成 |
|------|------|------|
| `kernel/task/cpu.c:363` | `#if PLATFORM_RK3588` MPIDR 移位 | `platform.conf smp.mpidr_shift` |
| `kernel/task/cpu.c:472` | `#if PLATFORM_SG2002` 关 SMP | `platform.conf smp.bringup` |
| `kernel/mm/aarch64/vm_early.c:103/154/170` | `#ifdef PLATFORM_RK3588` 1GB 块布局 | `platform.conf mmu.blocks[]` 循环填表 |
| `include/guest_loader.h:117` | `#if PLATFORM_RK3588` NPU DMA 窗口 | 生成的 `platform_guest.h`（值来自 `memory.reserves` 的 `npu-guest-dma`） |
| `driver/uart/uart_dw.c:28/337` | `ARCH_RISCV64 && PLATFORM_SG2002` 调试口 | 设备 cfg 的 earlycon hook / 平台 board hook |

允许保留 `PLATFORM_*` 的位置：`platforms/`、`include/<arch>/`（例如 `include/riscv64/pte.h` 的 SG2002 硬件怪癖）。规则：**通用 `kernel/`、`driver/`、`fs/` 里不允许出现 `PLATFORM_*`。**

---

## 12. 分阶段实施与验收

### 阶段 1：生命周期框架（本设计）

改动范围：

1. `include/driver/driver.h` + `driver/core/{driver,device}.c`
2. 三个 `link.ld` 的 `.drivers`
3. `gen_platform.py` 生成 `g_platform_devices[]`
4. `driver/drivers.mk` + 各 class 片段；Makefile §6 下沉
5. 16 个驱动加 `DRIVER_ENTRY` + probe/start
6. `kernel/main.c` 初始化梯子换阶段调用
7. `platform.conf` 增加 `irq.driver/block.driver/uart.irq_mode`
8. `block_register_root()` + `fs_init` 改用它；sdblk 单次初始化

验收：

- 5 个平台 `make PLATFORM=... kernel` 全绿，零告警；
- `make -C tests check`（三个 QEMU 平台）通过；
- `tools/boot_regress.sh` 连续启动无回归；
- 启动日志与改前逐项对比，除 IRQ/TIME 提前和 sdblk 单次 init 外无差异；
- `git diff` 中不包含任何数据路径修改（除 sdblk init 的幂等化）。

### 阶段 2：接口收口

- 公共头迁移 + `serial/irq/block` ops；
- 删除全局 `-Idriver`，开启 `driver-layering-check`；
- 服务 ops 注册（`*_bind()`），去掉 fs/syscall/pseudofs/VMM 的 `#if DRIVER_*`；
- IRQ 分发统一（单独提交）。

验收：`driver-layering-check` 干净；5 平台编译 + boot；LTP smoke；`vmm_helper_regress.sh` / `vmm_multivm_regress.sh` 通过。

### 阶段 3：平台数据化

- `smp`/`mmu.blocks`/`guest.npu_dma`/`uart.earlycon` 进 platform.conf；
- 替换第 11.3 节的 `#if PLATFORM_*`；
- 开启 `PLATFORM_*` grep 检查。

验收：`grep PLATFORM_ kernel/ driver/ fs/` 为空；5 平台编译 + boot。

---

## 13. 风险与未决问题

| 项 | 风险 | 处理 |
|----|------|------|
| `.drivers` 段 | 三个 link.ld 都要改；kallsyms 两趟链接 | 阶段 1 首个提交先在三个架构跑 `gen_kallsyms --verify` |
| IRQ 分发统一 | 改动 ack/EOI/异常返回路径，风险最高 | 独立提交，跑完整 VMM/调度门禁 |
| 间接调用 | serial/IRQ 每次多一次指针调用 | 相对 MMIO 可忽略；timer 热路径保持内联 |
| 设备表与命令行覆盖 | `UART=dw` 与 platform.conf 名字可能不一致 | D2：按 class 绑定，不按名字；`compatible` 只做 sanity check |
| 生成的 cfg 结构 | 加字段要同时改头和 gen_platform.py | C initializer 编译期兜底；文档记录字段来源 |
| sdblk 单次初始化 | 行为变化 | 阶段 1 单独列出，评审确认后随框架一起改 |

### 需要 review 拍板的问题

1. **D1/D2**：接受“链接器段 + 按 class 绑定 + 一次构建一个后端”吗？还是希望生成注册表/按名字匹配？
2. **D3**：typed cfg 还是继续 key/value？
3. **UART `irq_mode`**：v1 用配置布尔保留现状，还是直接把 UART 中断模式做成真正可用的（会扩大改动范围）？
4. **sdblk 单次初始化**：是否允许在阶段 1 顺手修掉（推荐），还是严格“零行为变化”留到阶段 2？
5. **IRQ 分发统一**：放在阶段 2 的最后，还是先不做，只保留 class 接口？
6. **命令与命名**：`UART=` 保留，`GIC=` 是否改为 `IRQ=`（会影响现有脚本/文档）？

---

## 14. 新驱动接入步骤（目标形态）

以新增一个 `foo` 网卡为例：

1. 写 `driver/eth/foo.c`：

   ```c
   static int foo_probe(const device_t *dev)
   {
       const net_cfg_t *cfg = dev->cfg;
       return foo_init(device_mmio(cfg->base));
   }

   DRIVER_ENTRY(driver_foo) = {
       .name = "foo", .cls = DRV_NET, .phase = DRV_PHASE_NET,
       .start_phase = DRV_PHASE_COUNT, .probe = foo_probe,
   };
   ```

2. 在 `driver/net/drivers.mk` 增加 `foo` 映射。
3. `platform.conf` 的 `eth.driver` 写 `foo`、填 base/irq。
4. **不需要改** `kernel/main.c`、`fs/`、`fs_init`、Makefile §6。

---

## 15. 评审清单

- [ ] 驱动注册/绑定机制（D1/D2）是否符合“不要 Linux 那么复杂”的预期？
- [ ] 6 个初始化阶段是否覆盖现有依赖，且没有隐藏顺序？
- [ ] 设备表字段是否足够描述现有 5 个平台？
- [ ] `include/driver/` 的接口边界是否合理？
- [ ] 阶段 1 的“零数据路径改动”边界是否可接受（sdblk 除外）？
- [ ] 是否同意先只实现阶段 1，再单独评审阶段 2 的 IRQ 接口？

---

## 16. 阶段 1 实现记录（2026-10-23）

已落地：

- 新增 `include/driver/{driver,serial,irq,time,block,net,npu,tpu}.h`；
- 新增 `driver/core/{driver,device}.c` 与 `driver/blk/block.c`（根块设备注册）；
- 三个 `boot/*/link.ld` 的 `.rodata` 内加入 `.drivers` 段；
- `tools/gen_platform.py` 生成 `g_platform_devices[]` 与 typed cfg；
- 16 个现有驱动加 `DRIVER_ENTRY`/probe/start（IRQ/时间/块/网络/NPU/TPU/DMA/ION/UART）；
- 驱动选择下沉到 `driver/drivers.mk` 与 9 个 class 片段；
- `kernel/main.c` 的初始化梯子替换为 6 处 `drivers_init(phase)`；
- `platform.conf` 增加 `irq.driver`（plic/lapic 不再靠 ARCH/布尔推断）、`block.driver`、`uart.irq_mode`、`eth.irq`；
- `fs_init()` 改为 `block_root_device()`，去掉 `#if DRIVER_SDBLK_SG2002`；
- `sdblk` 只在 STORAGE probe 初始化一次，`sdblk_bdev_open()` 幂等。

验证：

- 5 个平台（qemu-aarch64/riscv64/x86_64、rk3588、sg2002）`make kernel` 零告警；
- 三个架构 `gen_kallsyms --verify` 均通过；
- qemu-aarch64 / riscv64 / x86_64 实际启动到 `/ #` shell，驱动阶段顺序正确。

偏差与阶段 2 待办：

- 阶段 1 仍保留 `-Idriver` 与 `DRIVER_*` CFLAGS，fs/syscall 里的 `#if DRIVER_*` 未动（设计中的 D6 属于阶段 2）；
- 各驱动 probe 仍直接调用原有 init（内部继续用 `platform_get_mmio`/`platform_get_uint`），cfg 只用于 UART 基址等最小字段；阶段 2 改为完全从 cfg 读取；
- `KLOG_INIT("[drv] ... ready")` 属于模块 DEBUG，`LOG=info` 下不可见；需要观察驱动绑定请用 `LOG=debug`；
- `errno`/平台条件（`cpu.c`、`vm_early.c`、`guest_loader.h`、`uart_dw.c`）留待阶段 3 数据化；
- 网络 probe 失败仍返回 0（保持旧行为），阶段 2 改为返回错误并让 `driver_present(NET)` 为假。

---

## 17. 阶段 2a/2b 实现记录（2026-10-23）

### 17.1 串口 / 时间 / 块设备公共接口（2a）

- 删除 `driver/uart/uart.h`、`driver/timer/timer.h`：
  - 串口：`include/driver/serial.h` 提供 `serial_ops_t` 与
    `serial_putc/puts/getc/getc_nb/rx_ready`；三个 UART 驱动的 probe 里
    `serial_bind()`，内核不再知道具体是 PL011/DW/16550。
  - 时间：完整 timer API（tick、delay、counter、stats、tick callback）
    收敛到 `include/driver/time.h`；`timer.c` 只 include 公共头。
  - 块：`driver/blk/block.c` 维护根块设备，`fs_init` 用
    `block_root_device()`/`block_root_needs_mbr()`，不再 `#if DRIVER_SDBLK_*`。
- `kernel/mm/pmm.c` 不再 include 驱动私有 `ramblk_cfg.h`，rootfs 保留区
  改读 `platform_cfg.h` 的 `g_mem_rootfs_*`。

### 17.2 IRQ 公共接口（2b）

- 新增 `driver/irq/irq.c` + 扩展 `include/driver/irq.h`：
  - 统一处理器签名 `irq_handler_fn(void *frame, void *ctx)`；
  - `irq_register()/irq_enable()/irq_set_priority()`：设备中断线；
  - `irq_register_ipi()/irq_send_ipi(hw_id)`：resched IPI，号由后端决定；
  - `irq_register_timer()/irq_timer_enable()`：本地定时器；
  - `irq_secondary_init()/irq_cpu_hw_id()/irq_debug_dump()`；
  - `irq_dispatch_frame()`：架构异常入口唯一回调，后端完成
    ack → handler → EOI/complete；
  - `irq_virt_supported()`：给 VMM 的运行期能力查询（尚未替换 vGIC 的
    `#if DRIVER_GIC_V3`，见待办）。
- 四个后端各自 `irq_bind(&ops)`：
  - GICv3：ICC_IAR1/EOIR1；SGI 按完整 MPIDR 亲和性编码（修复 RK3588
    多 cluster IPI 只写 Aff0 的限制）；timer/IPI 注册进核心表；virt=true。
  - GICv2：IAR 原值写 EOIR/DIR；timer/IPI；virt=false。
  - PLIC：claim → 核心分发表 → complete；SBI IPI；SSIP 由后端清除；
    timer/IPI 走 scause 钩子。
  - LAPIC：按 trap frame 向量分发并在返回后 EOI；timer/IPI 注册进核心表。
- 迁移调用点：
  - `boot/aarch64/exception.c` 不再维护 `g_handler_vec`/`irq_install`，
    只调 `irq_dispatch_frame()`；
  - x86 删掉 `irq_handlers[]`/`irq_install()`，IRQ 向量交给 LAPIC 后端；
  - `kernel/task/cpu.c` 的 IPI 收发、次级核控制器初始化、SMP 自检
    GIC dump 全部改走公共接口；
  - `driver/timer/*` 通过 `irq_register_timer()` 注册；
  - `rknpu`、`cvitek_eth` 改 `irq_register()/irq_enable()/irq_set_priority()`，
    不再 include `irq/gicv3.h`/`irq/plic.h`；
  - `driver/irq/irq.h` 删除。
- `platforms/qemu-virt-aarch64/platform.conf` 补上 GICv2 的
  `gicc=0x08010000`/`gich=0x08020000`。此前 `GIC=v2` 时 GICC 基址为 0，
  PMR/CTLR 写进 RAM，表现为“所有中断都不来、SMP 自检 timer stuck”；
  GICv3 构建忽略这两个字段。

### 17.3 验证

- 5 个平台 clean build 零告警：qemu-aarch64（GICv3/GICv2）、
  qemu-riscv64（PLIC）、qemu-x86_64（LAPIC）、rk3588-aarch64 SMP=8、
  sg2002-riscv64（PLIC + cvitek-eth）。
- 三架构实际启动到 `/ #`，`uname -a`、`/proc/uptime`（约 7s 后 6.9s）
  正常；aarch64/x86 SMP=2 启动到 2/2 CPU online 并执行 busybox 命令。
- `GUEST_LINUX=1` aarch64：Linux 6.2.15 guest 启动到 `~ #` shell。
- `clang-format --dry-run --Werror` 对全部改动文件通过。

### 17.4 阶段 2c/2d 待办

- DMA/ION/TPU/NPU 的服务注册（`*_ops_t`/`*_bind()` + `*_present()`），替换 `kernel/syscall/*`、
  `kernel/fs/pseudofs/*`、`kernel/vmm/vdev/npu/vnpu.c` 的 `#if DRIVER_*`；
- VMM 的 `DRIVER_GIC_V3` 仍是编译期布局/实现选择（`include/vmm/vmm.h`
  的字段、`virq.c`/`el2_run.c`/`vm_init.c` 的 vGIC 调用），需要先给
  vGICv3 模型加弱符号桩，再用 `irq_virt_supported()` 在 VM 创建处拒绝；
- 移除全局 `-Idriver`，把 `irq/lapic.h`（x86 AP 启动、
  `g_tsc_freq_hz`）和 `irq/gicv3.h`（vGIC 寄存器定义）剩余用户收敛到
  公共/架构头，并启用分层检查。

---

## 18. 阶段 2c 实现记录（2026-10-23）

### 18.1 可选服务：ops 注册，不是弱符号

服务是否可用由“驱动有没有 probe 并绑定 ops”决定，不再用逐函数弱符号：

- 每类服务一个 `*_ops_t` + `g_*_ops` + `*_bind()`：
  - ION：`include/driver/ion.h` + `driver/ion/ion_core.c`；
  - dma-buf：`include/dma_buf.h` + `driver/dma/dma_core.c`；
  - TPU：`include/driver/tpu.h` + `driver/tpu/tpu_core.c`；
  - NPU：`include/driver/npu.h` + `driver/npu/npu_core.c`。
- 公共头里的 `ion_alloc()` 等是同名 `static inline` 包装：`g_*_ops == NULL`
  时统一返回失败/空，不再链接任何 stub 符号。
- 驱动 probe 调 `*_bind(&driver_ops)`；服务缺失时指针保持 NULL。
- NPU 的 DRM 入口（`rknpu_drm_ioctl_file/close_file/mmap_lookup`）也走
  `g_npu_ops`，pseudofs 节点表和 `sys_mmap` 直接引用内联包装。
- 删除了 `ion/dma/tpu/npu_stubs.c` 及 Makefile 里对应的兜底源文件。

### 18.2 fd 操作表（对应 Linux 的 file_operations）

`fd_obj_t` 增加 `const fd_ops_t *ops` 和 `void *priv`（`include/driver/fd_ops.h`）：

- ION / dma-buf 创建 fd 时由驱动填入自己的 ops；
- `sys_mmap` 不再有 ION/dma-buf 分支，统一调 `ops->get_phys(priv, &pa, &size)`
  再走公共逐页映射；
- `dup`/`fd_table_inherit` 调 `ops->dup`，`close` 调 `ops->close`；
- dma-buf 的 `DMA_BUF_IOCTL_SYNC` 仍在 `kernel/fs/pseudofs/dev.c` 处理
  用户指针（copy_from_user），ioctl 核心只按 `FDT_DMABUF` 分派一次。

### 18.3 去掉 kernel 里的 `#if DRIVER_*`

- `kernel/syscall/fs/{file_ops,fd_pool,ioctl}.c`、`kernel/syscall/mm/mmap.c`
  和 `kernel/fs/pseudofs/dev.c` 无条件引用服务包装。
- `/dev/ion`、`/dev/cvi-tpu0`、`/dev/dma_heap/*`、`/dev/dri/*`、
  `/proc/dri/*` 保留在固定节点表中，`pfs_node_available()` 按 `g_*_ops`
  过滤；节点 nid 语义不变。
- `kernel/vmm/vdev/npu/vnpu.c` 的 NPU 透传模型无条件编译，经
  `g_npu_ops` 调用；`host_base==0` 时仍然不注册设备。

### 18.4 验证

- 5 个平台 clean build 零告警：qemu-aarch64、qemu-riscv64、
  qemu-x86_64、rk3588 SMP=8（NPU+DMA）、sg2002（ION+TPU）。
- 默认 qemu-aarch64（无服务）：`/dev` 里没有 ion/cvi-tpu0/dma_heap/dri，
  `/proc/dri` 不存在；`/proc/uptime` 正常。
- `qemu-virt-aarch64 DMA=1` + `/bin/dmatest`：5/5 通过
  （alloc → mmap → sync → fork 共享 → 300 轮分配/关闭无泄漏）。
- `qemu-virt-riscv64 TPU=cvitpu ION=1`：`/dev/ion` 可见并正常启动。
- 没有对应硬件设备时（QEMU 的 NPU/TPU 节点），驱动 probe 被跳过，
  `g_*_ops` 为 NULL，相关节点隐藏；rk3588/sg2002 的设备表里有 npu0/tpu
  设备，probe 时绑定。
- `clang-format --dry-run --Werror` 通过。

### 18.5 剩余（阶段 2d）

- `-Idriver` 仍在全局 CFLAGS；`kernel/task/cpu.c`、`boot/x86_64/exception.c`、
  VMM 的 `irq/lapic.h`/`irq/gicv3.h` 私有 include 需要先收敛。
- VMM 的 `DRIVER_GIC_V3` 仍是编译期布局/实现选择（`include/vmm/vmm.h`、
  `virq.c`、`el2_run.c`、`vm_init.c`、`guest_loader.c`）。
- 分层检查脚本（禁止 kernel/ 引用 driver/ 私有头）尚未加入构建。
