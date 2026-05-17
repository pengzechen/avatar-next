-- platform.lua — sg2002-riscv64 (SOPHGO SG2002 / Milk-V Duo S)
--
-- 参考: https://github.com/pengzechen/axplat-riscv64-sg2002
--   CPU:       XuanTie C906 RISC-V 64-bit
--   DRAM:      0x80000000, 256 MB
--   UART0:     DW-APB 16550，0x04140000，reg-shift=2，irq=44 (0x2c)
--   PLIC:      0x70000000  (riscv,plic0, ndev=101)
--   CLINT:     0x74000000  (SBI timer, 不直接访问)
--   TPU:       TDMA=0x0C100000, TIU=0x0C101000 (BD_CTRL@0x0C101100)

platform = {
    arch = "riscv64",
    name = "SG2002",

    memory = {
        ram    = { base = 0x80000000, size = 0x10000000 },  -- 256 MB
        rootfs = { base = 0x88000000, size = 0x04000000, mb = 64 },
        reserves = {
            { name = "opensbi",  start = 0x80000000, stop = 0x801FFFFF },
            { name = "boot_low", start = 0x80200000, stop = 0x80205FFF },
        },
    },

    mmio_vma = true,
    lapic    = false,

    uart = {
        driver    = "dw",
        base      = 0x04140000,
        reg_shift = 2,
    },

    irq = {
        driver = "none",
        gicd   = 0,
        gicc   = 0,
        gich   = 0,
        gicr   = 0,
        plic   = 0x70000000,
        clint  = 0x74000000,
    },

    timer = {
        driver     = "rv",
        tick_ms    = 10,
        freq_hz    = 100,
        cntp       = 0,
        counter_hz = 4000000,   -- C906 timebase 4 MHz (来自 axconfig.toml)
    },

    tpu = {
        driver    = "cvitpu",
        tdma_base = 0x0C100000,
        tiu_base  = 0x0C101000,  -- TIU sub-block, offset 0x1000 within TDMA MMIO
        tdma_irq  = 73,
    },

    eth = {
        driver = "cvitek",       -- DWMAC 3.70a + internal EPHY
        base   = 0x04070000,
    },

    sdmmc = {
        driver             = "sg2002",
        sd_base            = 0x04310000,   -- SDIO0 SDHCI 控制器基址
        top_base           = 0x03000000,   -- TOP 系统控制模块基址
        top_off_pwrsw_ctrl = 0x1F4,        -- sd_pwrsw_ctrl 寄存器偏移
    },
}

-- 阶段顺序: earlycon → irqcore → drivers → fs → late
register_device("uart0", {
    drivers = function(self)
        dw_uart.init()
    end,
})

register_device("timer0", {
    drivers = function(self)
        timer.init()
        timer.enable()
    end,
})

register_device("tpu0", {
    drivers = function(self)
        cvi_tpu.init()
    end,
})

register_device("sdmmc0", {
    -- SOPHGO SG2002 SDIO0 控制器（SDHCI，PIO 模式）
    -- 硬件地址见上方 platform.sdmmc 表
    -- 编译启用：make PLATFORM=sg2002-riscv64 SDMMC=sg2002 kernel
    drivers = function(self)
        sdblk.init()
    end,
})
