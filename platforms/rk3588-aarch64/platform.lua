-- platform.lua — rk3588-aarch64
--
-- Rockchip RK3588 SoC, AArch64
-- UART: DesignWare 16550 (UART2 @ 0xFEB50000)
-- IRQ:  GICv3 (GICD @ 0xFE600000, GICR @ 0xFE680000)
-- NPU:  RKNPU @ 0xFDAB0000/0xFDAC0000/0xFDAD0000
-- VHE:  boot.S 在 EL2 自动启用 HCR_EL2.E2H | TGE

platform = {
    arch = "aarch64",
    name = "RK3588",

    memory = {
        -- RK3588 4GB LPDDR5 布局
        --   Bank0: 0x0000_0000 - 0x3FFF_FFFF  (1 GB)
        --   Bank1: 0x4000_0000 - 0x7FFF_FFFF  (1 GB)
        --   Bank2: 0x8000_0000 - 0xBFFF_FFFF  (1 GB)
        --   MMIO:  0xC000_0000 - 0xFFFF_FFFF  (1 GB, UART/GIC/PCIe etc.)
        --   低 2 MB 保留给固件 / U-Boot
        ram    = { base = 0x00200000, size = 0xBFE00000 },   -- 3 GB minus early 2 MB
        rootfs = { base = 0x10000000, size = 0x08000000, mb = 128 },
        reserves = {
            { name = "kernel", start = 0x00200000, stop = 0x00800000 },
        },
    },

    kernel = {
        -- U-Boot: tftp 0x400000 kernel_aarch64.bin; go 0x400000
        link = 0xffff000000400000,
    },

    -- RK3588 裸机运行，但任务切换后 ttbr0_el1=0，低半 MMIO 地址失效。
    -- 必须加 KERNEL_VMA 偏移，使所有驱动通过 TTBR1 高地址访问设备寄存器。
    mmio_vma = true,
    lapic    = false,

    uart = {
        driver    = "dw",
        base      = 0xFEB50000,  -- UART2（调试串口）
        reg_shift = 2,            -- DW APB UART：寄存器步长 4 字节（索引 << 2）
    },

    irq = {
        driver = "gicv3",
        gicd   = 0xFE600000,
        gicc   = 0,              -- GICv3 无 GICC（CPU 接口走系统寄存器）
        gich   = 0,
        gicr   = 0xFE680000,    -- 每核 128 KB，4 核共 512 KB
        plic   = 0,
        clint  = 0,
    },

    timer = {
        driver     = "aarch64",
        tick_ms    = 10,
        freq_hz    = 100,
        cntp       = 26,         -- AArch64 通用定时器 PPI
        counter_hz = 0,          -- 运行期从 CNTFRQ_EL0 读取
    },

    -- NPU 扩展字段（由 rknpu 驱动通过 platform_get_uintptr 读取）
    npu = {
        driver = "rknpu",
        base0 = 0xFDAB0000,
        base1 = 0xFDAC0000,
        base2 = 0xFDAD0000,
        pmu   = 0xFD8D8000,
        irq0  = 142,   -- NPU0 SPI = 110 + 32
        irq1  = 143,
        irq2  = 144,
    },
}

-- ── 设备注册 ──────────────────────────────────────────────────────────────────
-- 阶段顺序: earlycon → irqcore → drivers → fs → late

-- 早期串口（DesignWare UART，无中断初始化）
register_device("uart0", {
    earlycon = function(self)
        dw_uart.init()
    end,
})

-- GICv3 中断控制器
register_device("gic", {
    irqcore = function(self)
        gicv3.init()
    end,
})

-- 定时器
register_device("timer0", {
    drivers = function(self)
        timer.init()
        timer.enable()
    end,
})

-- RK3588 NPU（三核心，仅初始化 NPU0）
register_device("npu0", {
    drivers = function(self)
        rknpu.init()
    end,
})
