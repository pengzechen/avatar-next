//! cvitek-eth — SG2002/CV1812H 以太网驱动
//!
//! 从 [sg200x-bsp](https://github.com/yfblock/sg200x-bsp) 独立提取，
//! 保持原始代码不变，仅调整 crate 结构使其可独立使用。
//!
//! ## 模块结构
//!
//! | 模块              | 职责                                       |
//! |-------------------|--------------------------------------------|
//! | [`ethernet`]      | 以太网驱动顶层（DWMAC 3.70a）               |
//! | [`utils::cache`]  | D-cache / DMA 一致性维护（C906 / AArch64） |
//! | [`rstc`]          | SG2002 复位控制器驱动                       |
//!
//! ## 使用方式
//!
//! 需要外部提供 `GlobalAllocator`（通过 `kernel_support` crate 实现）。
//! 调用方通过 `ethernet::CvitekEthNic::init(base)` 初始化驱动。

#![no_std]

extern crate alloc;

pub(crate) mod rstc;
pub mod utils;
pub mod ethernet;

pub use ethernet::{CvitekEthNic, ETH_BASE, EthError, EthResult, RxToken};
