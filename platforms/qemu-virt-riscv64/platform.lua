-- platform.lua — qemu-virt-riscv64
-- [[ BUILD_CONFIG: 由 gen_platform.py 在编译期解析，生成 platform.mk / platform.h ]]
local BUILD_CONFIG = {
    ARCH                   = "riscv64",
    MEM_RAM_BASE           = "0x80000000",
    MEM_RAM_SIZE           = "0x80000000",
    MEM_ROOTFS_BASE        = "0x88000000",
    MEM_ROOTFS_SIZE        = "0x08000000",
    ROOTFS_SIZE_MB         = "128",
    MEM_PLATFORM           = "QEMU",

    DEV_UART_TYPE          = "dw",
    DEV_IRQ_TYPE           = "none",
    DEV_TIMER_TYPE         = "rv",

    DEV_UART_BASE          = "0x10000000",
    DEV_GICD_BASE          = "0",
    DEV_GICC_BASE          = "0",
    DEV_GICH_BASE          = "0",
    DEV_GICR_BASE          = "0",
    DEV_PLIC_BASE          = "0x0C000000",
    DEV_CLINT_BASE         = "0x02000000",

    DEV_MMIO_NEEDS_VMA     = "1",
    DEV_NEED_LAPIC         = "0",
    DEV_CNTP_TIMER         = "0",
    DEV_UART_REG_SHIFT     = "0",
    DEV_TIMER_TICK_MS      = "10",
    DEV_TIMER_FREQUENCY_HZ = "100",
    DEV_TIMER_COUNTER_HZ   = "10000000",

    PMM_RESV_0_NAME        = "opensbi",
    PMM_RESV_0_START       = "0x80000000",
    PMM_RESV_0_END         = "0x801FFFFF",
    PMM_RESV_1_NAME        = "boot_low",
    PMM_RESV_1_START       = "0x80200000",
    PMM_RESV_1_END         = "0x80205FFF",
}

-- 阶段顺序: earlycon → irqcore → drivers → fs → late

-- DW UART
register_device("uart0", {
    drivers = function(self)
        -- dw_uart 已在 C platform_init 中初始化
    end,
})

-- RISC-V 平台定时器（通过 CLINT）
register_device("timer0", {
    drivers = function(self)
        timer.init()
        timer.enable()
    end,
})
