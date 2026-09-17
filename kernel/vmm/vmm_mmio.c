/*
 * kernel/vmm/vmm_mmio.c — VMM MMIO 设备总线实现
 *
 * 移植自 x-kernel: virt/vdev/core/src/lib.rs (MmioBus)。
 * 线性查找：设备数很少（<= MMIO_MAX_DEVICES），无需哈希。
 */

#include "vmm/vmm_mmio.h"
#include "klog.h"
#include "string.h"

void mmio_bus_init(mmio_bus_t *bus)
{
    memset(bus, 0, sizeof(*bus));
}

int mmio_bus_register(mmio_bus_t *bus, mmio_device_t *dev)
{
    if (bus->nr >= MMIO_MAX_DEVICES) {
        KLOG_ERROR("[mmio] bus full, cannot register '%s'\n",
                   dev->ops ? dev->ops->name : "?");
        return -1;
    }
    bus->devs[bus->nr++] = dev;
    KLOG_INFO("[mmio] registered '%s' base=0x%llx size=0x%llx\n",
              dev->ops->name,
              (unsigned long long)dev->ops->base,
              (unsigned long long)dev->ops->size);
    return 0;
}

int mmio_bus_handle(mmio_bus_t *bus, uint64_t gpa, int is_write,
                    uint8_t size, uint64_t value, uint32_t vcpu_id,
                    uint64_t *out)
{
    int i;

    for (i = 0; i < bus->nr; i++) {
        mmio_device_t *dev = bus->devs[i];
        const mmio_dev_ops_t *ops = dev->ops;
        uint64_t off;

        if (gpa < ops->base || gpa >= ops->base + ops->size)
            continue;

        off = gpa - ops->base;

        if (is_write) {
            if (ops->write_for_vcpu)
                ops->write_for_vcpu(dev, off, size, value, vcpu_id);
            else if (ops->write)
                ops->write(dev, off, size, value);
            if (out)
                *out = 0;
        } else {
            uint64_t v;
            if (ops->read_for_vcpu)
                v = ops->read_for_vcpu(dev, off, size, vcpu_id);
            else if (ops->read)
                v = ops->read(dev, off, size);
            else
                v = 0;
            if (out)
                *out = v;
        }
        return 1;   /* 命中 */
    }
    return 0;       /* 无设备命中 */
}
