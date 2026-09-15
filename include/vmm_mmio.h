/*
 * include/vmm_mmio.h — VMM MMIO 设备总线与设备接口
 *
 * 移植自 x-kernel: virt/vdev/core/src/lib.rs (MmioDevice / MmioBus)
 *
 * Rust 的 trait 对象在此以「函数指针 vtable」表达：每个虚拟设备提供
 * 一组回调，MmioBus 按 GPA 区间把 guest 的 MMIO 访问分发到对应设备。
 *
 * 使用前提：Stage-2 / G-stage / EPT 必须把设备所在的 GPA 区间映射为
 * 「无效」，这样 guest 访问设备时才会陷入 VMM（见各架构 stage2/gstage/ept）。
 */
#ifndef VMM_MMIO_H
#define VMM_MMIO_H

#include "types.h"

/* ── 最大注册设备数 ────────────────────────────────────────── */
#define MMIO_MAX_DEVICES  16

struct mmio_device;

/*
 * MMIO 设备操作表（对标 Rust MmioDevice trait）。
 *
 * name         — 设备名（诊断用）
 * base/size    — 设备 MMIO 窗口（GPA）
 * read         — 读 offset 处 size 字节，返回读到的值
 * write        — 写 size 字节到 offset
 * read_for_vcpu/write_for_vcpu — 带 vCPU id 的变体；未提供时回落到 read/write
 */
typedef struct mmio_dev_ops {
    const char *name;
    uint64_t    base;
    uint64_t    size;

    uint64_t (*read)(struct mmio_device *dev, uint64_t offset, uint8_t size);
    void     (*write)(struct mmio_device *dev, uint64_t offset, uint8_t size,
                      uint64_t value);

    uint64_t (*read_for_vcpu)(struct mmio_device *dev, uint64_t offset,
                              uint8_t size, uint32_t vcpu_id);
    void     (*write_for_vcpu)(struct mmio_device *dev, uint64_t offset,
                               uint8_t size, uint64_t value, uint32_t vcpu_id);
} mmio_dev_ops_t;

/* 设备实例：操作表 + 设备私有状态 */
typedef struct mmio_device {
    const mmio_dev_ops_t *ops;
    void                 *priv;
} mmio_device_t;

/* ── MMIO 总线 ─────────────────────────────────────────────── */
typedef struct mmio_bus {
    mmio_device_t *devs[MMIO_MAX_DEVICES];
    int            nr;
} mmio_bus_t;

void mmio_bus_init(mmio_bus_t *bus);

/* 注册设备；超过 MMIO_MAX_DEVICES 返回 -1 */
int  mmio_bus_register(mmio_bus_t *bus, mmio_device_t *dev);

/*
 * 分发一次 MMIO 访问。
 * 命中设备返回 1（读访问时把读到的值写入 *out）；无设备命中返回 0。
 */
int  mmio_bus_handle(mmio_bus_t *bus, uint64_t gpa, int is_write,
                     uint8_t size, uint64_t value, uint32_t vcpu_id,
                     uint64_t *out);

#endif /* VMM_MMIO_H */
