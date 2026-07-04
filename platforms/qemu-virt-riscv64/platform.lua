-- platform.lua — qemu-virt-riscv64
--
-- 全局配置表。
--   编译期: gen_platform.py 解析 platform 表生成 platform.mk
--   早期引导: C 通过扫描嵌入字节数组获取内存布局
--   运行期:   Lua VM 进入各阶段回调，驱动初始化使用 platform.xxx 字段

platform = {
    arch = "riscv64",
    name = "QEMU",

    memory = {
        ram    = { base = 0x80000000, size = 0x80000000 },
        rootfs = { base = 0x88000000, size = 0x10000000, mb = 256 },
        reserves = {
            { name = "opensbi",  start = 0x80000000, stop = 0x801FFFFF },
            { name = "boot_low", start = 0x80200000, stop = 0x80205FFF },
        },
    },

    mmio_vma = true,
    lapic    = false,

    uart = {
        driver    = "dw",
        base      = 0x10000000,
        reg_shift = 0,
    },

    irq = {
        driver = "none",
        gicd   = 0,
        gicc   = 0,
        gich   = 0,
        gicr   = 0,
        plic   = 0x0C000000,
        clint  = 0x02000000,
    },

    timer = {
        driver     = "rv",
        tick_ms    = 10,
        freq_hz    = 100,
        cntp       = 0,
        counter_hz = 10000000,
    },

    eth = {
        driver = "virtio",
        base   = 0x10008000,
    },
}

-- 阶段顺序: earlycon → irqcore → drivers → fs → late

-- DW UART（已在 C platform_init 中初始化）
register_device("uart0", {
    drivers = function(self)
    end,
})

-- RISC-V 定时器（通过 CLINT）
register_device("timer0", {
    drivers = function(self)
        timer.init()
        timer.enable()
    end,
})
