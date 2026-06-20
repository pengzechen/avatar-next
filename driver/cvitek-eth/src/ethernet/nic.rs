//! SG2002 / CV1812H 板载 Synopsys DesignWare MAC（DWMAC 3.70a）驱动。
//!
//! # 初始化流程
//!
//! 1. 时钟使能（`CLKGEN_BASE`）＋ 复位解除（`Rstc`）
//! 2. DMA 软复位（`DmaBusMode::SWR`）
//! 3. 建立 TX/RX 描述符环，分配 2 KB 帧缓冲
//! 4. MDIO / PHY 自协商（最长等 2.5 s）
//! 5. 配置 MAC（FES / DM 位根据协商结果），启动 DMA 收发
//!
//! # 使用方式
//!
//! ```no_run
//! let mut nic = CvitekEthNic::init(ETH_BASE).unwrap();
//! // 发送
//! nic.transmit(&frame_bytes).unwrap();
//! // 接收（非阻塞）
//! if let Ok(tok) = nic.receive() {
//!     process(tok.frame());
//!     // Drop(tok) 会自动把描述符还给 DMA
//! }
//! ```

use alloc::boxed::Box;
use alloc::vec::Vec;
use core::sync::atomic::{fence, Ordering};

use tock_registers::interfaces::{ReadWriteable, Readable, Writeable};

use super::desc::{
    BUF_SIZE, DmaDesc, RDES0_ES, RDES0_FL_MASK, RDES0_FL_SHIFT, RDES0_OWN, RDES1_RBS1_MASK,
    RDES1_RER, RX_RING_SIZE, TDES0_OWN, TDES1_FS, TDES1_IC, TDES1_LS, TDES1_TBS1_MASK,
    TDES1_TER, TX_RING_SIZE,
};
use super::mdio::{mdio_read, mdio_write};
use super::regs::{
    Addr0High, DmaBusMode, DmaOperation, DmaStatus, Dwc3DmaRegs, Dwc3GmacRegs, FrameFilter,
    MacControl, dma_regs, gmac_regs,
};
use crate::utils::cache::{dcache_clean_range, dcache_invalidate_range};

/// GMAC MMIO 默认基地址（物理地址），SG2002 以太网控制器。
pub const ETH_BASE: usize = 0x0407_0000;

const PHY_ADDR: u32 = 0;

const CLKGEN_BASE: usize = 0x0300_2000;
const REG_CLK_EN_0: usize = 0x000;
/// bit25 = eth0 ahb 时钟，bit26 = eth0 ptpclk
const CLKEN0_ETH_MASK: u32 = (1 << 25) | (1 << 26);
/// bit0 = ephy 复位，bit1 = eth0 mac 复位
const SOFT_RSTN3_EPHY_MASK: u32 = (1 << 0) | (1 << 1);

/// 以太网帧最短有效载荷（不含 FCS），发送时不足则尾部补零。
const MIN_ETH_FRAME: usize = 60;

// ─── 错误类型 ────────────────────────────────────────────────────────────────

/// 驱动操作错误
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EthError {
    /// 资源暂时不可用（TX 描述符全忙 / RX 无新帧），可在下一轮重试
    Again,
    /// 内存分配失败
    NoMemory,
    /// 参数非法（如帧长超限）
    BadParam,
}

/// 以太网操作结果类型
pub type EthResult<T> = Result<T, EthError>;

// ─── 帧缓冲 ──────────────────────────────────────────────────────────────────

/// 单个 DMA 帧缓冲，64 byte 对齐以避免跨 cache-line 边界。
#[repr(C, align(64))]
struct DmaPktBuf([u8; BUF_SIZE]);

impl DmaPktBuf {
    fn new() -> Self {
        Self([0u8; BUF_SIZE])
    }
}

// ─── 主驱动结构体 ─────────────────────────────────────────────────────────────

/// cvitek 以太网网络接口控制器驱动。
///
/// 内部维护 TX/RX 描述符环和对应帧缓冲（均 heap 分配）。
/// 通过 [`Self::init`] 初始化后，调用 [`Self::transmit`] / [`Self::receive`] 进行收发。
pub struct CvitekEthNic {
    base: usize,
    mac_addr: [u8; 6],

    tx_descs: Vec<DmaDesc>,
    rx_descs: Vec<DmaDesc>,
    tx_bufs:  Vec<Box<DmaPktBuf>>,
    rx_bufs:  Vec<Box<DmaPktBuf>>,

    tx_head: usize, // 下一个可写 TX 描述符
    tx_tail: usize, // 已发出但尚未回收的最早 TX 描述符
    rx_cur:  usize, // 下一个期望 DMA 填充完成的 RX 描述符

    rx_count: u32,
    tx_count: u32,
}

// SAFETY: 单核内核里不存在真正的并发，驱动只从一个任务/线程使用。
unsafe impl Send for CvitekEthNic {}
unsafe impl Sync for CvitekEthNic {}

impl CvitekEthNic {
    pub const TX_QUEUE_SIZE: usize = TX_RING_SIZE;
    pub const RX_QUEUE_SIZE: usize = RX_RING_SIZE;
    pub const MAX_FRAME_LEN: usize = BUF_SIZE;

    #[inline]
    fn gmac(&self) -> &Dwc3GmacRegs {
        unsafe { gmac_regs(self.base) }
    }

    #[inline]
    fn dma(&self) -> &Dwc3DmaRegs {
        unsafe { dma_regs(self.base) }
    }

    // ─── DMA 软复位 ────────────────────────────────────────────────────────

    fn dma_reset(&self) {
        let dma = self.dma();
        dma.bus_mode.modify(DmaBusMode::SWR::SET);
        let mut t = 100_000u32;
        while dma.bus_mode.is_set(DmaBusMode::SWR) {
            t = t.wrapping_sub(1);
            if t == 0 {
                log::warn!("cvitek-eth: DMA reset timeout");
                break;
            }
        }
    }

    // ─── MAC 地址 I/O ──────────────────────────────────────────────────────

    fn read_mac_from_hw(&self) -> [u8; 6] {
        let gmac = self.gmac();
        let hi = gmac.addr0_high.get();
        let lo = gmac.addr0_low.get();
        [
            (lo & 0xFF) as u8,
            ((lo >> 8) & 0xFF) as u8,
            ((lo >> 16) & 0xFF) as u8,
            ((lo >> 24) & 0xFF) as u8,
            (hi & 0xFF) as u8,
            ((hi >> 8) & 0xFF) as u8,
        ]
    }

    fn set_mac_hw(&self, m: &[u8; 6]) {
        let lo = (m[0] as u32)
            | ((m[1] as u32) << 8)
            | ((m[2] as u32) << 16)
            | ((m[3] as u32) << 24);
        let hi_field = (m[4] as u32) | ((m[5] as u32) << 8);
        self.gmac().addr0_low.set(lo);
        self.gmac()
            .addr0_high
            .write(Addr0High::ADDR_HI.val(hi_field) + Addr0High::ADDR_ENABLE::SET);
    }

    // ─── 描述符 cache 操作 ─────────────────────────────────────────────────

    fn flush_desc(desc: &DmaDesc) {
        dcache_clean_range(desc as *const DmaDesc as usize, core::mem::size_of::<DmaDesc>());
    }

    fn invalidate_desc(desc: &DmaDesc) {
        dcache_invalidate_range(
            desc as *const DmaDesc as usize,
            core::mem::size_of::<DmaDesc>(),
        );
    }

    // ─── 描述符环初始化 ────────────────────────────────────────────────────

    fn setup_tx_ring(&mut self) {
        for i in 0..TX_RING_SIZE {
            let buf_pa = self.tx_bufs[i].0.as_ptr() as u32;
            let d = &mut self.tx_descs[i];
            d.des0 = 0;
            d.des1 = if i == TX_RING_SIZE - 1 { TDES1_TER } else { 0 };
            d.des2 = buf_pa;
            d.des3 = 0;
            Self::flush_desc(d);
        }
    }

    fn setup_rx_ring(&mut self) {
        for i in 0..RX_RING_SIZE {
            let data_pa = self.rx_bufs[i].0.as_ptr() as u32;
            let d = &mut self.rx_descs[i];
            d.des2 = data_pa;
            d.des1 = {
                let mut v = (BUF_SIZE as u32).min(RDES1_RBS1_MASK);
                if i == RX_RING_SIZE - 1 {
                    v |= RDES1_RER;
                }
                v
            };
            d.des3 = 0;
            fence(Ordering::Release);
            d.des0 = RDES0_OWN;
            Self::flush_desc(d);
        }
    }

    // ─── 时钟 / 复位 ───────────────────────────────────────────────────────

    unsafe fn enable_clocks_and_release_resets() {
        unsafe {
            let clk_en0 = (CLKGEN_BASE + REG_CLK_EN_0) as *mut u32;
            let v = core::ptr::read_volatile(clk_en0);
            core::ptr::write_volatile(clk_en0, v | CLKEN0_ETH_MASK);
        }

        let rstc = unsafe { crate::rstc::Rstc::new() };
        let regs = rstc.regs();

        // 解除 ETH0 软复位（SOFT_RSTN_0 bit12）
        let v0 = regs.soft_rstn_0.get();
        regs.soft_rstn_0.set(v0 | (1 << 12));

        // 解除 EPHY 相关复位（SOFT_RSTN_3 bit0/1）
        let v3 = regs.soft_rstn_3.get();
        regs.soft_rstn_3.set(v3 | SOFT_RSTN3_EPHY_MASK);

        // 等待 EPHY 稳定（约 2 ms @1GHz）
        for _ in 0..2_000_000u32 {
            core::hint::spin_loop();
        }
    }

    // ─── 初始化入口 ────────────────────────────────────────────────────────

    /// 初始化以太网驱动。
    ///
    /// `base` 为 GMAC MMIO 物理基地址，通常为 [`ETH_BASE`]。
    /// 调用后 PHY 会完成自协商，MAC/DMA 进入就绪状态。
    pub fn init(base: usize) -> EthResult<Self> {
        // 分配描述符和帧缓冲
        let mut tx_descs = Vec::with_capacity(TX_RING_SIZE);
        let mut rx_descs = Vec::with_capacity(RX_RING_SIZE);
        for _ in 0..TX_RING_SIZE { tx_descs.push(DmaDesc::zero()); }
        for _ in 0..RX_RING_SIZE { rx_descs.push(DmaDesc::zero()); }

        let mut tx_bufs: Vec<Box<DmaPktBuf>> = Vec::with_capacity(TX_RING_SIZE);
        let mut rx_bufs: Vec<Box<DmaPktBuf>> = Vec::with_capacity(RX_RING_SIZE);
        for _ in 0..TX_RING_SIZE { tx_bufs.push(Box::new(DmaPktBuf::new())); }
        for _ in 0..RX_RING_SIZE { rx_bufs.push(Box::new(DmaPktBuf::new())); }

        let mut nic = Self {
            base,
            mac_addr: [0; 6],
            tx_descs,
            rx_descs,
            tx_bufs,
            rx_bufs,
            tx_head: 0,
            tx_tail: 0,
            rx_cur:  0,
            rx_count: 0,
            tx_count: 0,
        };

        let ver = nic.gmac().version.get();
        log::info!("cvitek-eth: DWMAC version {:#x}", ver);

        // 读硬件 MAC；若全 0 / 全 F 则使用默认值
        nic.mac_addr = nic.read_mac_from_hw();
        if nic.mac_addr == [0; 6] || nic.mac_addr == [0xFF; 6] {
            nic.mac_addr = [0x00, 0x50, 0x43, 0x02, 0x02, 0x02];
        }
        log::info!(
            "cvitek-eth: MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
            nic.mac_addr[0], nic.mac_addr[1], nic.mac_addr[2],
            nic.mac_addr[3], nic.mac_addr[4], nic.mac_addr[5],
        );

        // 使能时钟 + 解除复位
        unsafe { Self::enable_clocks_and_release_resets() };

        // DMA 软复位
        nic.dma_reset();

        // 建立描述符环
        nic.setup_tx_ring();
        nic.setup_rx_ring();

        // DMA 总线模式：PBL=8, DSL=12（64 byte stride）, FB=1, AAL=1
        nic.dma().bus_mode.write(
            DmaBusMode::PBL.val(8)
                + DmaBusMode::DSL.val(12)
                + DmaBusMode::FB::SET
                + DmaBusMode::AAL::SET,
        );
        nic.dma().tx_base.set(nic.tx_descs.as_ptr() as u32);
        nic.dma().rx_base.set(nic.rx_descs.as_ptr() as u32);

        // 屏蔽不需要的 GMAC 中断（PCS/LPI/...），关闭 DMA 中断
        nic.gmac().int_mask.set(0x60F);
        nic.dma().intr_ena.set(0);

        // 写入 MAC 地址
        nic.set_mac_hw(&nic.mac_addr);

        // 接收所有帧（Promiscuous + ReceiveAll）
        nic.gmac()
            .frame_filter
            .write(FrameFilter::PR::SET + FrameFilter::RA::SET);
        nic.gmac().hash_high.set(0xFFFF_FFFF);
        nic.gmac().hash_low.set(0xFFFF_FFFF);

        // ─── PHY 初始化（内部 EPHY）──────────────────────────────────────

        // 1. 软复位 PHY（bit15）
        mdio_write(nic.gmac(), PHY_ADDR, 0, 0x8000);
        for _ in 0..1_000_000u32 { core::hint::spin_loop(); }

        // 等待复位完成
        for _ in 0..100u32 {
            if mdio_read(nic.gmac(), PHY_ADDR, 0) & 0x8000 == 0 { break; }
            for _ in 0..100_000u32 { core::hint::spin_loop(); }
        }

        // 2. 启动自协商（AN=1, full duplex, 100M）
        mdio_write(nic.gmac(), PHY_ADDR, 0, 0x3300);

        // 3. 等待 link up（最长 ~2.5 s）
        let mut link_up = false;
        let mut bmcr    = 0u16;
        #[allow(unused_assignments)]
        let mut bmsr    = 0u16;
        let mut lpa     = 0u16;

        for i in 0..2500u32 {
            // 读两次 BMSR 以清除 latch-low 位
            let _ = mdio_read(nic.gmac(), PHY_ADDR, 1);
            bmsr = mdio_read(nic.gmac(), PHY_ADDR, 1);
            if bmsr & 4 != 0 {
                bmcr = mdio_read(nic.gmac(), PHY_ADDR, 0);
                lpa  = mdio_read(nic.gmac(), PHY_ADDR, 5);
                log::info!(
                    "cvitek-eth: link UP after {} polls \
                     bmcr=0x{:04x} bmsr=0x{:04x} lpa=0x{:04x}",
                    i, bmcr, bmsr, lpa
                );
                link_up = true;
                break;
            }
            for _ in 0..200_000u32 { core::hint::spin_loop(); }
        }
        if !link_up {
            bmcr = mdio_read(nic.gmac(), PHY_ADDR, 0);
            bmsr = mdio_read(nic.gmac(), PHY_ADDR, 1);
            lpa  = mdio_read(nic.gmac(), PHY_ADDR, 5);
            log::warn!(
                "cvitek-eth: link still DOWN after AN timeout, \
                 continuing (bmcr=0x{:04x} bmsr=0x{:04x} lpa=0x{:04x})",
                bmcr, bmsr, lpa
            );
        }

        // 4. 根据 LPA 协商结果确定速率和双工
        let anar = mdio_read(nic.gmac(), PHY_ADDR, 4);
        let common = anar & lpa;
        let (speed_100m, full_duplex) = if common & (1 << 8) != 0 {
            (true, true)
        } else if common & (1 << 7) != 0 {
            (true, false)
        } else if common & (1 << 6) != 0 {
            (false, true)
        } else if common & (1 << 5) != 0 {
            (false, false)
        } else {
            // 协商结果无法确定，从 BMCR 读取或默认 100M FD
            (bmcr & (1 << 13) != 0 || true, bmcr & (1 << 8) != 0 || true)
        };

        log::info!(
            "cvitek-eth: link mode = {} {} (anar=0x{:04x} lpa=0x{:04x})",
            if speed_100m { "100M" } else { "10M" },
            if full_duplex { "FD" } else { "HD" },
            anar, lpa
        );

        // ─── MAC 配置 ─────────────────────────────────────────────────────

        let mut mc = MacControl::PS::SET + MacControl::TE::SET + MacControl::RE::SET;
        if speed_100m { mc += MacControl::FES::SET; }
        if full_duplex { mc += MacControl::DM::SET; }
        nic.gmac().mac_control.write(mc);
        nic.gmac().mmc_cntrl.set(0x01); // 冻结 MMC 计数器

        // ─── DMA 启动 ─────────────────────────────────────────────────────

        nic.dma().operation.write(
            DmaOperation::TSF::SET
                + DmaOperation::RSF::SET
                + DmaOperation::OSF::SET
                + DmaOperation::FTF::SET, // 先 flush TX FIFO
        );
        // 等待 FIFO flush 完成
        let mut t = 100_000u32;
        while nic.dma().operation.is_set(DmaOperation::FTF) {
            t = t.wrapping_sub(1);
            if t == 0 { break; }
        }
        nic.dma().operation.modify(DmaOperation::ST::SET + DmaOperation::SR::SET);
        nic.dma().rx_poll.set(1);

        // 打印初始化后寄存器快照
        let g = nic.gmac();
        let d = nic.dma();
        log::debug!(
            "cvitek-eth: mac_ctl=0x{:08x} frame_filter=0x{:08x} \
             addr0_hi=0x{:08x} addr0_lo=0x{:08x}",
            g.mac_control.get(), g.frame_filter.get(),
            g.addr0_high.get(), g.addr0_low.get()
        );
        log::debug!(
            "cvitek-eth: dma_bus=0x{:08x} dma_op=0x{:08x} \
             dma_status=0x{:08x} tx_base=0x{:08x} rx_base=0x{:08x}",
            d.bus_mode.get(), d.operation.get(), d.status.get(),
            d.tx_base.get(), d.rx_base.get()
        );
        log::info!("cvitek-eth: initialized OK");
        Ok(nic)
    }

    // ─── 公开查询接口 ──────────────────────────────────────────────────────

    /// 返回当前 MAC 地址（6 字节数组）。
    pub fn mac_address(&self) -> [u8; 6] {
        self.mac_addr
    }

    /// 检查是否可以立即发送（TX 描述符未被 DMA 占用）。
    pub fn can_transmit(&self) -> bool {
        Self::invalidate_desc(&self.tx_descs[self.tx_head]);
        let des0 = unsafe { core::ptr::read_volatile(&self.tx_descs[self.tx_head].des0) };
        des0 & TDES0_OWN == 0
    }

    /// 检查是否有新的 RX 帧等待读取。
    pub fn can_receive(&self) -> bool {
        Self::invalidate_desc(&self.rx_descs[self.rx_cur]);
        let des0 = unsafe { core::ptr::read_volatile(&self.rx_descs[self.rx_cur].des0) };
        des0 & RDES0_OWN == 0
    }

    // ─── TX 描述符回收 ─────────────────────────────────────────────────────

    /// 回收已被 DMA 发完的 TX 描述符（从 tail 推进）。
    pub fn reclaim_tx(&mut self) {
        while self.tx_tail != self.tx_head {
            Self::invalidate_desc(&self.tx_descs[self.tx_tail]);
            let des0 =
                unsafe { core::ptr::read_volatile(&self.tx_descs[self.tx_tail].des0) };
            if des0 & TDES0_OWN != 0 {
                break; // DMA 还没发完
            }
            self.tx_tail = (self.tx_tail + 1) % TX_RING_SIZE;
        }
    }

    // ─── 发送 ──────────────────────────────────────────────────────────────

    /// 发送一帧（`packet` 不含 FCS）。
    ///
    /// 帧长不足 [`MIN_ETH_FRAME`] 字节时自动补零；超过 [`BUF_SIZE`] 返回错误。
    pub fn transmit(&mut self, packet: &[u8]) -> EthResult<()> {
        if packet.len() > Self::MAX_FRAME_LEN {
            return Err(EthError::BadParam);
        }

        self.reclaim_tx();

        let idx = self.tx_head;
        Self::invalidate_desc(&self.tx_descs[idx]);
        let des0 = unsafe { core::ptr::read_volatile(&self.tx_descs[idx].des0) };
        if des0 & TDES0_OWN != 0 {
            return Err(EthError::Again);
        }

        // 拷贝帧数据到 DMA 缓冲区
        let buf = &mut self.tx_bufs[idx].0;
        buf[..packet.len()].copy_from_slice(packet);
        let mut len = packet.len();
        if len < MIN_ETH_FRAME {
            for b in &mut buf[len..MIN_ETH_FRAME] { *b = 0; }
            len = MIN_ETH_FRAME;
        }

        // clean D-cache（CPU 写 → DMA 读）
        let data_pa = buf.as_ptr() as usize;
        dcache_clean_range(data_pa, len);

        // 填写描述符
        let d = &mut self.tx_descs[idx];
        let mut tdes1 =
            TDES1_IC | TDES1_FS | TDES1_LS | ((len as u32) & TDES1_TBS1_MASK);
        if idx == TX_RING_SIZE - 1 {
            tdes1 |= TDES1_TER;
        }
        unsafe {
            core::ptr::write_volatile(&mut d.des2, data_pa as u32);
            core::ptr::write_volatile(&mut d.des1, tdes1);
        }
        fence(Ordering::Release);
        // 最后写 OWN 位，把描述符交给 DMA
        unsafe { core::ptr::write_volatile(&mut d.des0, TDES0_OWN) };
        Self::flush_desc(d);
        dcache_clean_range(data_pa, len);

        self.tx_count = self.tx_count.wrapping_add(1);
        if log::log_enabled!(log::Level::Trace) {
            let n = packet.len().min(20);
            // log::trace!(
            //     "cvitek-eth: TX#{} idx={} len={} hdr={:02x?}",
            //     self.tx_count, idx, len, &packet[..n]
            // );
        }

        self.tx_head = (self.tx_head + 1) % TX_RING_SIZE;
        self.dma().tx_poll.set(1);
        Ok(())
    }

    // ─── 接收 ──────────────────────────────────────────────────────────────

    /// 轮询接收一帧（非阻塞）。
    ///
    /// 成功时返回 [`RxToken`]；token 的 [`Drop`] 实现会自动把描述符还给 DMA。
    /// 无新帧时返回 `Err(EthError::Again)`。
    pub fn receive(&mut self) -> EthResult<RxToken<'_>> {
        let idx = self.rx_cur;

        // 清除 RI 中断状态（如果不轮询 DMA interrupt 则是可选的）
        if self.dma().status.is_set(DmaStatus::RI) {
            self.dma()
                .status
                .write(DmaStatus::RI::SET + DmaStatus::NIS::SET);
        }

        Self::invalidate_desc(&self.rx_descs[idx]);
        let des0 = unsafe { core::ptr::read_volatile(&self.rx_descs[idx].des0) };

        // DMA 还未填完
        if des0 & RDES0_OWN != 0 {
            return Err(EthError::Again);
        }
        // 帧有错误
        if des0 & RDES0_ES != 0 {
            self.requeue_rx(idx);
            self.rx_cur = (self.rx_cur + 1) % RX_RING_SIZE;
            return Err(EthError::Again);
        }

        // 计算有效帧长（RDES0.FL 含 4 字节 FCS）
        let frame_len =
            ((des0 & RDES0_FL_MASK) >> RDES0_FL_SHIFT) as usize;
        let frame_len = if frame_len >= 4 { frame_len - 4 } else { frame_len }
            .min(BUF_SIZE);

        // invalidate D-cache（DMA 写 → CPU 读）
        let buf_va = self.rx_bufs[idx].0.as_ptr() as usize;
        dcache_invalidate_range(buf_va, frame_len);

        self.rx_cur = (self.rx_cur + 1) % RX_RING_SIZE;
        self.rx_count = self.rx_count.wrapping_add(1);

        if log::log_enabled!(log::Level::Trace) {
            let n = frame_len.min(20);
            // log::trace!(
            //     "cvitek-eth: RX#{} idx={} len={} hdr={:02x?}",
            //     self.rx_count, idx, frame_len,
            //     &self.rx_bufs[idx].0[..n]
            // );
        }

        Ok(RxToken { nic: self, slot: idx, len: frame_len })
    }

    // ─── 内部：归还 RX 描述符 ─────────────────────────────────────────────

    pub(crate) fn requeue_rx(&mut self, slot: usize) {
        let pa = self.rx_bufs[slot].0.as_ptr() as u32;
        let d = &mut self.rx_descs[slot];
        unsafe {
            core::ptr::write_volatile(&mut d.des2, pa);
            let mut rdes1 = (BUF_SIZE as u32).min(RDES1_RBS1_MASK);
            if slot == RX_RING_SIZE - 1 { rdes1 |= RDES1_RER; }
            core::ptr::write_volatile(&mut d.des1, rdes1);
            core::ptr::write_volatile(&mut d.des3, 0);
            fence(Ordering::Release);
            core::ptr::write_volatile(&mut d.des0, RDES0_OWN);
        }
        Self::flush_desc(d);
        self.dma()
            .status
            .write(DmaStatus::RI::SET + DmaStatus::NIS::SET);
        self.dma().rx_poll.set(1);
    }
}

// ─── RxToken ─────────────────────────────────────────────────────────────────

/// 接收帧 token。
///
/// 调用 [`Self::frame`] 获取帧数据切片；token 被 drop 时自动把描述符归还给 DMA。
pub struct RxToken<'a> {
    nic:  &'a mut CvitekEthNic,
    slot: usize,
    len:  usize,
}

impl<'a> RxToken<'a> {
    /// 返回接收到的以太网帧数据（不含 FCS）。
    #[inline]
    pub fn frame(&self) -> &[u8] {
        &self.nic.rx_bufs[self.slot].0[..self.len]
    }

    #[inline]
    pub fn len(&self) -> usize {
        self.len
    }

    #[inline]
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
}

impl<'a> Drop for RxToken<'a> {
    fn drop(&mut self) {
        let slot = self.slot;
        self.nic.requeue_rx(slot);
    }
}
