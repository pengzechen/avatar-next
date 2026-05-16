-- platform.lua — qemu-virt-x86_64
-- [[ BUILD_CONFIG: 由 gen_platform.py 在编译期解析，生成 platform.mk / platform.h ]]
local BUILD_CONFIG = {
    ARCH                   = "x86_64",
    MEM_RAM_BASE           = "0x00100000",
    MEM_RAM_SIZE           = "0x7FF00000",
    MEM_ROOTFS_BASE        = "0x04000000",
    MEM_ROOTFS_SIZE        = "0x08000000",
    ROOTFS_SIZE_MB         = "128",
    MEM_PLATFORM           = "QEMU",

    DEV_UART_TYPE          = "x86",
    DEV_IRQ_TYPE           = "none",
    DEV_TIMER_TYPE         = "x86",

    DEV_UART_BASE          = "0",
    DEV_GICD_BASE          = "0",
    DEV_GICC_BASE          = "0",
    DEV_GICH_BASE          = "0",
    DEV_GICR_BASE          = "0",
    DEV_PLIC_BASE          = "0",
    DEV_CLINT_BASE         = "0",

    DEV_MMIO_NEEDS_VMA     = "0",
    DEV_NEED_LAPIC         = "1",
    DEV_CNTP_TIMER         = "0",
    DEV_UART_REG_SHIFT     = "0",
    DEV_TIMER_TICK_MS      = "10",
    DEV_TIMER_FREQUENCY_HZ = "100",
    DEV_TIMER_COUNTER_HZ   = "0",

    PMM_RESV_0_NAME        = "lowmem_1m_2m",
    PMM_RESV_0_START       = "0x00100000",
    PMM_RESV_0_END         = "0x001FFFFF",
}

-- 阶段顺序: earlycon → irqcore → drivers → fs → late

-- x86 定时器（PIT/LAPIC）
register_device("timer0", {
    drivers = function(self)
        timer.init()
        timer.enable()
    end,
})
