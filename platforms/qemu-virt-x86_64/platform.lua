-- platform.lua — qemu-virt-x86_64
--
-- 全局配置表。
--   编译期: gen_platform.py 解析 platform 表生成 platform.mk
--   早期引导: C 通过扫描嵌入字节数组获取内存布局
--   运行期:   Lua VM 进入各阶段回调，驱动初始化使用 platform.xxx 字段

platform = {
    arch = "x86_64",
    name = "QEMU",

    memory = {
        ram    = { base = 0x00100000, size = 0x7FF00000 },
        rootfs = { base = 0x04000000, size = 0x10000000, mb = 256 },
        reserves = {
            { name = "lowmem_1m_2m", start = 0x00100000, stop = 0x001FFFFF },
        },
    },

    mmio_vma = false,
    lapic    = true,

    uart = {
        driver    = "x86",
        base      = 0,
        reg_shift = 0,
    },

    irq = {
        driver = "none",
        gicd   = 0,
        gicc   = 0,
        gich   = 0,
        gicr   = 0,
        plic   = 0,
        clint  = 0,
    },

    timer = {
        driver     = "x86",
        tick_ms    = 10,
        freq_hz    = 100,
        cntp       = 0,
        counter_hz = 0,
    },
}

-- 阶段顺序: earlycon → irqcore → drivers → fs → late

-- x86 定时器（PIT/LAPIC）
register_device("timer0", {
    drivers = function(self)
        timer.init()
        timer.enable()
    end,
})
