-- platform.lua — qemu-virt-aarch64
-- [[ BUILD_CONFIG: 由 gen_platform.py 在编译期解析，生成 platform.mk / platform.h ]]
local BUILD_CONFIG = {
    ARCH                   = "aarch64",
    MEM_RAM_BASE           = "0x40000000",
    MEM_RAM_SIZE           = "0x80000000",
    MEM_ROOTFS_BASE        = "0x60000000",
    MEM_ROOTFS_SIZE        = "0x08000000",
    ROOTFS_SIZE_MB         = "128",
    MEM_PLATFORM           = "QEMU",

    DEV_UART_TYPE          = "pl011",
    DEV_IRQ_TYPE           = "gicv2",
    DEV_TIMER_TYPE         = "aarch64",

    DEV_UART_BASE          = "0x09000000",
    DEV_GICD_BASE          = "0x08000000",
    DEV_GICC_BASE          = "0x08010000",
    DEV_GICH_BASE          = "0x08030000",
    DEV_GICR_BASE          = "0x080A0000",
    DEV_PLIC_BASE          = "0",
    DEV_CLINT_BASE         = "0",

    DEV_MMIO_NEEDS_VMA     = "1",
    DEV_NEED_LAPIC         = "0",
    DEV_CNTP_TIMER         = "26",
    DEV_UART_REG_SHIFT     = "0",
    DEV_TIMER_TICK_MS      = "10",
    DEV_TIMER_FREQUENCY_HZ = "100",
    DEV_TIMER_COUNTER_HZ   = "0",
}

-- 阶段顺序: earlycon → irqcore → drivers → fs → late

-- GIC：虚拟化模式下使用 gic_virtual_init
register_device("gic", {
    irqcore = function(self)
        gicv2.virtual_init()
    end,
})

-- UART / 串口（pl011 驱动已在 earlycon 阶段由 C 代码完成，此处可选重新配置）
register_device("uart0", {
    drivers = function(self)
        -- pl011 已在 C platform_init 中初始化，Lua 阶段无需重复
    end,
})

-- 定时器
register_device("timer0", {
    drivers = function(self)
        timer.init()
        timer.enable()
    end,
})
