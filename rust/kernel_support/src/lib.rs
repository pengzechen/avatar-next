//! kernel_support — Avatar OS C 内核的 Rust 支撑库
//!
//! 提供：
//! - [`GlobalAllocator`]：使用 C 内核的 `kernel_alloc` / `kernel_free`（lib/rust_glue.c）
//! - [`log::Log`] 实现：通过 `klog_putchar` 输出日志
//! - `#[panic_handler]`：调用 `platform_panic` 转交 C 内核处理
//!
//! 所有 Rust 驱动 crate 依赖本库来获得全局分配器和 panic 处理支持。

#![no_std]
#![feature(alloc_error_handler)]

extern crate alloc;

use core::alloc::{GlobalAlloc, Layout};

// ─── FFI 声明（C 内核侧实现）────────────────────────────────────────────────

extern "C" {
    /// lib/rust_glue.c — PMM 页粒度分配，返回内核虚拟地址
    fn kernel_alloc(size: usize) -> *mut u8;

    /// lib/rust_glue.c — PMM 页粒度释放
    fn kernel_free(ptr: *mut u8, size: usize);

    /// lib/klog.c — 输出单个字节（ASCII/UTF-8）
    pub fn klog_putchar(c: u8);

    /// platforms/.../platform.c — 不可恢复错误，不返回
    pub fn platform_panic();
}

// ─── 全局分配器 ──────────────────────────────────────────────────────────────

struct KernelAllocator;

unsafe impl GlobalAlloc for KernelAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        // PMM 按页对齐分配，size 向上取整到页大小在 C 侧完成
        unsafe { kernel_alloc(layout.size()) }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        unsafe { kernel_free(ptr, layout.size()) }
    }
}

#[global_allocator]
static ALLOCATOR: KernelAllocator = KernelAllocator;

#[cfg(not(test))]
#[alloc_error_handler]
fn alloc_oom(_layout: Layout) -> ! {
    unsafe { platform_panic() };
    loop {}
}

// ─── Panic 处理 ──────────────────────────────────────────────────────────────

#[cfg(not(test))]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    unsafe { platform_panic() };
    loop {}
}

// ─── 日志后端 ────────────────────────────────────────────────────────────────

struct KernelLogger;

impl log::Log for KernelLogger {
    fn enabled(&self, _metadata: &log::Metadata) -> bool {
        true
    }

    fn log(&self, record: &log::Record) {
        use core::fmt::Write;
        let _ = write!(KernelWriter, "[{}] {}\n", record.level(), record.args());
    }

    fn flush(&self) {}
}

/// 使用 `klog_putchar` 的 `fmt::Write` 实现
struct KernelWriter;

impl core::fmt::Write for KernelWriter {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        for b in s.bytes() {
            unsafe { klog_putchar(b) };
        }
        Ok(())
    }
}

static LOGGER: KernelLogger = KernelLogger;

/// 初始化 Rust 日志系统（对接 klog_putchar）
///
/// 在首次使用 Rust 日志宏之前调用（通常由 `eth_init` 内部触发）。
pub fn init_logger() {
    log::set_logger(&LOGGER).ok();
    log::set_max_level(log::LevelFilter::Trace);
}
