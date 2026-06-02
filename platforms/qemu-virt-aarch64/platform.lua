-- platform.lua — qemu-virt-aarch64
--
-- 全局配置表。
--   编译期: gen_platform.py 解析 platform 表生成 platform.mk
--   早期引导: C 通过扫描嵌入字节数组获取内存布局
--   运行期:   Lua VM 进入各阶段回调，驱动初始化使用 platform.xxx 字段

platform = {
    arch = "aarch64",
    name = "QEMU",

    memory = {
        ram    = { base = 0x40000000, size = 0x80000000 },
        rootfs = { base = 0x60000000, size = 0x08000000, mb = 128 },
        reserves = {},
    },

    kernel = {
        -- QEMU -kernel 将 AArch64 image 加载到 0x40080000。
        link = 0xffff000040080000,
    },

    mmio_vma = true,
    lapic    = false,

    uart = {
        driver    = "pl011",
        base      = 0x09000000,
        reg_shift = 0,
    },

    irq = {
        driver = "gicv2",
        gicd   = 0x08000000,
        gicc   = 0x08010000,
        gich   = 0x08030000,
        gicr   = 0x080A0000,
        plic   = 0,
        clint  = 0,
    },

    timer = {
        driver     = "aarch64",
        tick_ms    = 10,
        freq_hz    = 100,
        cntp       = 26,
        counter_hz = 0,
    },
}

-- 阶段顺序: earlycon → irqcore → drivers → fs → late

-- GIC：虚拟化模式下使用 gic_virtual_init
register_device("gic", {
    irqcore = function(self)
        gicv2.virtual_init()
    end,
})

-- UART（pl011 已在 C platform_init 中初始化，Lua 阶段无需重复）
register_device("uart0", {
    drivers = function(self)
    end,
})

-- 定时器
register_device("timer0", {
    drivers = function(self)
        timer.init()
        timer.enable()
    end,
})
