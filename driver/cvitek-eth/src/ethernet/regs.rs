//! Synopsys DesignWare MAC（DWMAC 3.70a）寄存器布局。
//!
//! 用 [`tock_registers`] 提供类型化 MMIO，整个 ethernet 子模块统一通过
//! 本文件的 [`gmac_regs`] / [`dma_regs`] 视图访问寄存器。
//!
//! 寄存器偏移与位域命名对齐 Linux `drivers/net/ethernet/stmicro/stmmac/dwmac1000.h`：
//! - GMAC 区段：base + `0x0000..0x0FFF`
//! - DMA  区段：base + `0x1000..0x1FFF`

use tock_registers::{register_bitfields, register_structs};
use tock_registers::registers::ReadWrite;

register_bitfields![u32,
    /// GMAC_CONTROL @ 0x0000：MAC 总开关。
    pub MacControl [
        RE          OFFSET(2)  NUMBITS(1) [],   // Receiver Enable
        TE          OFFSET(3)  NUMBITS(1) [],   // Transmitter Enable
        DC          OFFSET(4)  NUMBITS(1) [],   // Deferral Check
        ACS         OFFSET(7)  NUMBITS(1) [],   // Auto-pad/CRC stripping
        DCRS        OFFSET(16) NUMBITS(1) [],   // Disable CRC check
        PS          OFFSET(15) NUMBITS(1) [],   // Port select: 0=GMII, 1=MII
        FES         OFFSET(14) NUMBITS(1) [],   // Speed: 0=10M, 1=100M (MII only)
        DM          OFFSET(11) NUMBITS(1) [],   // Full duplex mode
    ],

    /// GMAC_FRAME_FILTER @ 0x0004：地址过滤策略。
    pub FrameFilter [
        PR          OFFSET(0)  NUMBITS(1) [],   // Promiscuous Mode
        RA          OFFSET(31) NUMBITS(1) [],   // Receive All
    ],

    /// GMAC_MII_ADDR @ 0x0010：MDIO 地址/控制。
    pub MiiAddr [
        MII_BUSY    OFFSET(0)  NUMBITS(1) [],   // 1 = busy
        MII_WRITE   OFFSET(1)  NUMBITS(1) [],   // 1 = write, 0 = read
        MII_CLK_CSR OFFSET(2)  NUMBITS(4) [],   // MDC 时钟分频
        MII_REG     OFFSET(6)  NUMBITS(5) [],   // PHY register address
        MII_PHY     OFFSET(11) NUMBITS(5) [],   // PHY device address
    ],

    /// GMAC_ADDR0_HIGH @ 0x0040：MAC 地址高 16 bit。bit31 必须置 1（AE）。
    pub Addr0High [
        ADDR_HI     OFFSET(0)  NUMBITS(16) [],
        ADDR_ENABLE OFFSET(31) NUMBITS(1)  [],
    ],

    /// DMA_BUS_MODE @ 0x1000。
    pub DmaBusMode [
        SWR  OFFSET(0)  NUMBITS(1) [],  // Software Reset
        DSL  OFFSET(2)  NUMBITS(5) [],  // Descriptor Skip Length（单位：字）
        PBL  OFFSET(8)  NUMBITS(6) [],  // Programmable Burst Length
        FB   OFFSET(16) NUMBITS(1) [],  // Fixed Burst Length
        AAL  OFFSET(25) NUMBITS(1) [],  // Address-Aligned Beats
    ],

    /// DMA_STATUS @ 0x1014：W1C 中断状态位。
    pub DmaStatus [
        TI  OFFSET(0)  NUMBITS(1) [],   // Transmit Interrupt
        RI  OFFSET(6)  NUMBITS(1) [],   // Receive Interrupt
        NIS OFFSET(16) NUMBITS(1) [],   // Normal Interrupt Summary
    ],

    /// DMA_OPERATION @ 0x1018：DMA 收发使能 + FIFO 模式。
    pub DmaOperation [
        SR  OFFSET(1)  NUMBITS(1) [],   // Start/Stop Receive
        OSF OFFSET(2)  NUMBITS(1) [],   // Operate on Second Frame
        ST  OFFSET(13) NUMBITS(1) [],   // Start/Stop Transmission
        FTF OFFSET(20) NUMBITS(1) [],   // Flush Transmit FIFO（自清零）
        TSF OFFSET(21) NUMBITS(1) [],   // Transmit Store-and-Forward
        RSF OFFSET(25) NUMBITS(1) [],   // Receive Store-and-Forward
    ],
];

register_structs! {
    /// GMAC（DWMAC 3.70a）顶层寄存器（base + 0x0000..0x0FFF）。
    pub Dwc3GmacRegs {
        (0x0000 => pub mac_control:   ReadWrite<u32, MacControl::Register>),
        (0x0004 => pub frame_filter:  ReadWrite<u32, FrameFilter::Register>),
        (0x0008 => pub hash_high:     ReadWrite<u32>),
        (0x000C => pub hash_low:      ReadWrite<u32>),
        (0x0010 => pub mii_addr:      ReadWrite<u32, MiiAddr::Register>),
        (0x0014 => pub mii_data:      ReadWrite<u32>),
        (0x0018 => _reserved0),
        (0x0020 => pub version:       ReadWrite<u32>),
        (0x0024 => _reserved1),
        (0x003C => pub int_mask:      ReadWrite<u32>),
        (0x0040 => pub addr0_high:    ReadWrite<u32, Addr0High::Register>),
        (0x0044 => pub addr0_low:     ReadWrite<u32>),
        (0x0048 => _reserved2),
        (0x0100 => pub mmc_cntrl:     ReadWrite<u32>),
        (0x0104 => _reserved3),
        (0x1000 => @END),
    }
}

register_structs! {
    /// GMAC DMA 子模块寄存器（base + 0x1000..0x1FFF）。
    pub Dwc3DmaRegs {
        (0x0000 => pub bus_mode:      ReadWrite<u32, DmaBusMode::Register>),
        (0x0004 => pub tx_poll:       ReadWrite<u32>),
        (0x0008 => pub rx_poll:       ReadWrite<u32>),
        (0x000C => pub rx_base:       ReadWrite<u32>),
        (0x0010 => pub tx_base:       ReadWrite<u32>),
        (0x0014 => pub status:        ReadWrite<u32, DmaStatus::Register>),
        (0x0018 => pub operation:     ReadWrite<u32, DmaOperation::Register>),
        (0x001C => pub intr_ena:      ReadWrite<u32>),
        (0x0020 => @END),
    }
}

/// MDIO `MII_CLK_CSR` 字段值：CSR 时钟落在 60–100 MHz 时使用 `/42` 分频（值 = 4）。
pub const MII_CLK_CSR_60_100M_DIV42: u32 = 0x4;

/// 用 GMAC base 地址构造 [`Dwc3GmacRegs`] 视图。
///
/// # Safety
/// `base` 必须是经过虚拟映射、长度 ≥ 0x1000 的 GMAC MMIO 区域。
#[inline]
pub unsafe fn gmac_regs(base: usize) -> &'static Dwc3GmacRegs {
    unsafe { &*(base as *const Dwc3GmacRegs) }
}

/// 用 GMAC base 地址构造 [`Dwc3DmaRegs`] 视图（自动加上 `+0x1000` 偏移）。
///
/// # Safety
/// 同 [`gmac_regs`]。
#[inline]
pub unsafe fn dma_regs(base: usize) -> &'static Dwc3DmaRegs {
    unsafe { &*((base + 0x1000) as *const Dwc3DmaRegs) }
}
