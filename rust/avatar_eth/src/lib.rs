//! avatar_eth — Avatar OS 内核以太网驱动 C FFI 适配层
//!
//! 本 crate 是 `cvitek-eth` BSP 驱动与 C 内核的胶水层：
//!
//! - 以 `#[no_mangle] extern "C"` 函数向 C 侧导出 `eth_init / eth_send / eth_recv / eth_mac_addr`
//! - 通过依赖 `kernel_support` 为整个 Rust 树提供全局分配器和 panic 处理
//!
//! C 侧声明见 `include/eth_api.h`。

#![no_std]

extern crate alloc;
extern crate kernel_support; // 拉入 GlobalAllocator + panic_handler

use alloc::boxed::Box;
use cvitek_eth::{CvitekEthNic, EthError};

// 首次调用时初始化 Rust 日志系统
fn ensure_logger() {
    static INITED: core::sync::atomic::AtomicBool =
        core::sync::atomic::AtomicBool::new(false);
    if !INITED.swap(true, core::sync::atomic::Ordering::Relaxed) {
        kernel_support::init_logger();
    }
}

/// 初始化 cvitek 以太网驱动，返回不透明句柄。
///
/// # Safety
/// `base` 必须是有效的 GMAC MMIO 物理基地址（已经过内核恒等或固定虚拟映射）。
#[no_mangle]
pub unsafe extern "C" fn eth_init(base: u64) -> *mut CvitekEthNic {
    ensure_logger();
    match CvitekEthNic::init(base as usize) {
        Ok(nic) => Box::into_raw(Box::new(nic)),
        Err(e) => {
            log::error!("eth_init failed: {:?}", e);
            core::ptr::null_mut()
        }
    }
}

/// 发送一帧以太网数据。
///
/// 返回 `0` = 成功，`-1` = TX 描述符忙（重试），`-2` = 参数错误。
///
/// # Safety
/// `nic` 必须是 `eth_init` 返回的有效指针；`data` 指向长度 ≥ `len` 的有效内存。
#[no_mangle]
pub unsafe extern "C" fn eth_send(
    nic: *mut CvitekEthNic,
    data: *const u8,
    len: usize,
) -> i32 {
    let nic = unsafe { &mut *nic };
    let packet = unsafe { core::slice::from_raw_parts(data, len) };
    match nic.transmit(packet) {
        Ok(()) => 0,
        Err(EthError::Again) => -1,
        Err(_) => -2,
    }
}

/// 轮询接收一帧（非阻塞）。
///
/// 返回实际帧字节数（> 0），无数据返回 `0`，错误返回 `-1`。
///
/// # Safety
/// `nic` 有效；`buf` 指向长度 ≥ `maxlen` 的可写内存。
#[no_mangle]
pub unsafe extern "C" fn eth_recv(
    nic: *mut CvitekEthNic,
    buf: *mut u8,
    maxlen: usize,
) -> i32 {
    let nic = unsafe { &mut *nic };
    match nic.receive() {
        Ok(token) => {
            let frame = token.frame();
            let copy_len = frame.len().min(maxlen);
            unsafe { core::ptr::copy_nonoverlapping(frame.as_ptr(), buf, copy_len) };
            copy_len as i32
        }
        Err(EthError::Again) => 0,
        Err(_) => -1,
    }
}

/// 读取设备 MAC 地址。
///
/// # Safety
/// `nic` 有效；`mac` 指向 6 字节可写缓冲区。
#[no_mangle]
pub unsafe extern "C" fn eth_mac_addr(nic: *mut CvitekEthNic, mac: *mut u8) {
    let nic = unsafe { &*nic };
    let addr = nic.mac_address();
    unsafe { core::ptr::copy_nonoverlapping(addr.as_ptr(), mac, 6) };
}
