#![no_std]

extern crate kernel_support;

use cvitek_usb::usb;
use cvitek_usb::usb::class::uvc;
use cvitek_usb::usb::host::dwc2;
use cvitek_usb::usb::host::dwc2::ep0 as dwc2_ep0;

fn ensure_logger() {
    static INITED: core::sync::atomic::AtomicBool = core::sync::atomic::AtomicBool::new(false);
    if !INITED.swap(true, core::sync::atomic::Ordering::Relaxed) {
        kernel_support::init_logger();
    }
}

#[repr(C)]
pub struct UsbEnumResult {
    pub num_devices: u32,
    pub first_uvc_addr: u8,
    pub first_uvc_ep0_mps: u32,
    pub first_uvc_vid: u16,
    pub first_uvc_pid: u16,
    pub has_uvc: u8,
    pub has_msc: u8,
}

#[repr(C)]
pub struct UsbUvcFrame {
    pub data: *const u8,
    pub length: u32,
    pub transfers: u32,
    pub data_packets: u32,
    pub fid: u8,
}

struct SyncEnumResult(core::cell::UnsafeCell<UsbEnumResult>);
unsafe impl Sync for SyncEnumResult {}
static G_ENUM_RESULT: SyncEnumResult = SyncEnumResult(core::cell::UnsafeCell::new(UsbEnumResult {
    num_devices: 0,
    first_uvc_addr: 0,
    first_uvc_ep0_mps: 0,
    first_uvc_vid: 0,
    first_uvc_pid: 0,
    has_uvc: 0,
    has_msc: 0,
}));

use core::sync::atomic::{AtomicBool, Ordering};

static G_UVC_INITIALIZED: AtomicBool = AtomicBool::new(false);

// UvcStreamSelection 不是 Sync，需要用 UnsafeCell 包装
struct SyncStreamSel(core::cell::UnsafeCell<Option<uvc::UvcStreamSelection>>);
unsafe impl Sync for SyncStreamSel {}
static G_UVC_STREAM_SEL: SyncStreamSel = SyncStreamSel(core::cell::UnsafeCell::new(None));

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dwc2_usb_set_base_virt(addr: usize) {
    ensure_logger();
    usb::set_dwc2_base_virt(addr);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dwc2_usb_set_phy_base_virt(addr: usize) {
    usb::set_cv182x_phy_base_virt(addr);
}

unsafe fn delay_us(us: u32) {
    for _ in 0..(us as u64 * 400) {
        core::hint::spin_loop();
    }
}

/// USB 上电序列：时钟使能 → PHY 复位/模式切换 → VBUS GPIO
///
/// 参考 tgoskits cvi_usb_camera.rs 的初始化流程。
/// 所有地址参数均为虚拟地址（已通过 platform_get_mmio 转换）。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dwc2_usb_power_up(
    clkgen_base: usize,
    top_base: usize,
    fmux_base: usize,
    ioblk_base: usize,
    gpio1_base: usize,
) -> i32 {
    ensure_logger();

    if clkgen_base == 0 || top_base == 0 || fmux_base == 0 || ioblk_base == 0 || gpio1_base == 0 {
        log::error!("[USB] power_up: missing base address(es)");
        return -1;
    }

    log::info!("[USB] power_up: clkgen={:#x} top={:#x} fmux={:#x} ioblk={:#x} gpio1={:#x}",
              clkgen_base, top_base, fmux_base, ioblk_base, gpio1_base);

    unsafe {
        // 1. enable_usb_clocks_cv181x
        //    CLKGEN+0x004 |= (0xF << 28)
        //    CLKGEN+0x008 |= 1
        //    CLKGEN+0x030 &= ~((1<<17)|(1<<18))
        let en1 = (clkgen_base + 0x004) as *mut u32;
        let en2 = (clkgen_base + 0x008) as *mut u32;
        let byp = (clkgen_base + 0x030) as *mut u32;
        core::ptr::write_volatile(en1, core::ptr::read_volatile(en1) | (0xF << 28));
        core::ptr::write_volatile(en2, core::ptr::read_volatile(en2) | 1);
        core::ptr::write_volatile(byp, core::ptr::read_volatile(byp) & !((1u32 << 17) | (1u32 << 18)));
        log::info!("[USB] clocks enabled");

        // 2. cvitek_usb_top_host_bringup
        //    TOP+0x3000: clear bit11, delay 50us, set bit11, delay 50us
        //    TOP+0x48: device mode (|0xC0|0x01), delay 1000us, host mode (clear bit7, set bit6|0x01), delay 1000us
        //    TOP+0xB4: set bit7
        let rst = (top_base + 0x3000) as *mut u32;
        let usb_pin = (top_base + 0x48) as *mut u32;
        let eco = (top_base + 0xB4) as *mut u32;

        let v = core::ptr::read_volatile(rst);
        core::ptr::write_volatile(rst, v & !(1u32 << 11));
        delay_us(50);
        let v = core::ptr::read_volatile(rst);
        core::ptr::write_volatile(rst, v | (1u32 << 11));
        delay_us(50);

        let x = core::ptr::read_volatile(usb_pin);
        core::ptr::write_volatile(usb_pin, (x & !0xC0) | 0xC0 | 0x01);
        delay_us(1000);
        let x = core::ptr::read_volatile(usb_pin);
        core::ptr::write_volatile(usb_pin, (x & !0xC0) | 0x40 | 0x01);
        delay_us(1000);

        let v = core::ptr::read_volatile(eco);
        core::ptr::write_volatile(eco, v | 0x80);
        log::info!("[USB] PHY host bringup done");

        // 3. pinmux_usb_vbus_det_gpio_output_prep
        //    FMUX+0xFC: write XGPIOB_6 = 3 (bits [2:0])
        //    IOBLK+0x020: set bits [7:5] = 0b111 (drive strength max)
        let fmux_vbus = (fmux_base + 0xFC) as *mut u32;
        let ioblk_vbus = (ioblk_base + 0x020) as *mut u32;

        core::ptr::write_volatile(fmux_vbus, 3u32);
        let v = core::ptr::read_volatile(ioblk_vbus);
        core::ptr::write_volatile(ioblk_vbus, v | (7u32 << 5));
        log::info!("[USB] VBUS pinmux configured");

        // 4. enable_usb_vbus_gpio
        //    GPIO1 pin 6: set direction=output, set level=high
        let gpio1_ddr = (gpio1_base + 0x004) as *mut u32;
        let gpio1_dr  = (gpio1_base + 0x000) as *mut u32;
        let pin_mask: u32 = 1 << 6;

        let v = core::ptr::read_volatile(gpio1_ddr);
        core::ptr::write_volatile(gpio1_ddr, v | pin_mask);
        let v = core::ptr::read_volatile(gpio1_dr);
        core::ptr::write_volatile(gpio1_dr, v | pin_mask);
        log::info!("[USB] VBUS GPIO enabled (GPIOB pin 6 high)");

        // 5. 等待 2 秒让设备稳定
        log::info!("[USB] waiting 2s for VBUS stabilization...");
        delay_us(2_000_000);
        log::info!("[USB] power_up complete");
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dwc2_usb_init() -> i32 {
    ensure_logger();
    match unsafe { dwc2::dwc2_probe() } {
        Ok((h1, h2, h3)) => {
            log::info!("[USB] DWC2 probed: GHWCFG1={:#010x} GHWCFG2={:#010x} GHWCFG3={:#010x}", h1, h2, h3);
            0
        }
        Err(e) => {
            log::error!("[USB] dwc2_probe failed: {:?}", e);
            -1
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dwc2_usb_device_connected() -> i32 {
    let hprt = unsafe { usb::host::dwc2::dwc2_hprt0_read() };
    if usb::host::dwc2::hprt_connsts(hprt) { 1 } else { 0 }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dwc2_usb_enumerate_device(out: *mut UsbEnumResult) -> i32 {
    ensure_logger();

    let mut extras = None;
    for attempt in 1..=4u32 {
        match usb::host::enumerate_topology_only() {
            Ok(ex) => {
                extras = Some(ex);
                break;
            }
            Err(e) => {
                log::warn!("[USB] enumerate attempt {}/4 failed: {:?}", attempt, e);
                for _ in 0..(1_500_000 * attempt) {
                    core::hint::spin_loop();
                }
            }
        }
    }

    let extras = match extras {
        Some(ex) => ex,
        None => {
            log::error!("[USB] enumerate failed after 4 attempts");
            return -1;
        }
    };

    let result = unsafe { &mut *out };
    let mut count = 0u32;
    result.has_uvc = 0;
    result.has_msc = 0;
    result.first_uvc_addr = 0;

    if let Some(ref uvc_dev) = extras.uvc {
        result.has_uvc = 1;
        result.first_uvc_addr = uvc_dev.addr;
        result.first_uvc_ep0_mps = uvc_dev.ep0_mps;
        result.first_uvc_vid = uvc_dev.vid;
        result.first_uvc_pid = uvc_dev.pid;
        count += 1;

        uvc_init_stream_now(uvc_dev.addr as u32, uvc_dev.ep0_mps);
    }
    if extras.msc.is_some() {
        result.has_msc = 1;
        count += 1;
    }
    result.num_devices = count;

    unsafe { core::ptr::copy_nonoverlapping(result as *const UsbEnumResult, G_ENUM_RESULT.0.get(), 1) };
    0
}

fn uvc_init_stream_now(dev: u32, ep0_mps: u32) {
    if G_UVC_INITIALIZED.load(Ordering::Relaxed) {
        return;
    }

    let cfg_buf = match uvc::read_configuration_descriptor(dev, ep0_mps, 0) {
        Ok(buf) => buf,
        Err(e) => {
            log::error!("[USB] UVC read config desc failed: {:?}", e);
            return;
        }
    };
    let cfg_total = u16::from_le_bytes([cfg_buf[2], cfg_buf[3]]) as usize;

    let mut sel = match uvc::parse_uvc_video_stream(&cfg_buf, cfg_total) {
        Ok(s) => s,
        Err(e) => {
            log::error!("[USB] UVC parse stream failed: {:?}", e);
            return;
        }
    };

    if let Some(ctl) = uvc::parse_uvc_control_entities(&cfg_buf, cfg_total) {
        let default_tune = uvc::UvcImageTuning {
            brightness: None,
            contrast: None,
            hue: None,
            saturation: None,
            sharpness: None,
            gamma: None,
            backlight: None,
            gain: None,
            white_balance_temp_k: None,
            power_line_freq: None,
        };
        let _ = uvc::uvc_init_camera_controls(dev, ep0_mps, &ctl, &default_tune);
    }

    if let Err(e) = uvc::uvc_start_video_stream(dev, ep0_mps, &mut sel) {
        log::error!("[USB] UVC start stream failed: {:?}", e);
        return;
    }

    // warm-up: 给传感器 ~2 秒出图时间，丢弃第一次捕获（可能超时）
    for _ in 0..8_000_000u32 {
        core::hint::spin_loop();
    }
    log::info!("[USB] UVC warm-up: discarding first capture attempt");
    let _ = uvc::uvc_capture_one_frame(dev, ep0_mps, &sel);
    uvc::reset_frame_continuity();

    unsafe {
        *G_UVC_STREAM_SEL.0.get() = Some(sel);
    }
    G_UVC_INITIALIZED.store(true, Ordering::Relaxed);
    log::info!("[USB] UVC stream initialized during enumeration");
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn dwc2_usb_capture_first_uvc_frame(
    enum_result: *const UsbEnumResult,
    out: *mut UsbUvcFrame,
) -> i32 {
    ensure_logger();
    let er = unsafe { &*enum_result };
    if er.has_uvc == 0 {
        log::warn!("[USB] no UVC device enumerated");
        return -1;
    }

    let dev = er.first_uvc_addr as u32;
    let ep0_mps = er.first_uvc_ep0_mps;

    if !G_UVC_INITIALIZED.load(Ordering::Relaxed) {
        uvc_init_stream_now(dev, ep0_mps);
        if !G_UVC_INITIALIZED.load(Ordering::Relaxed) {
            log::error!("[USB] UVC stream init failed");
            return -2;
        }
    }

    let sel = unsafe { (*G_UVC_STREAM_SEL.0.get()).as_ref().unwrap() };

    match uvc::uvc_capture_one_frame(dev, ep0_mps, sel) {
        Ok(jpeg_len) => {
            let frame_data = dwc2_ep0::dma_rx_slice(uvc::UVC_ASSEMBLED_JPEG_DMA_OFF, jpeg_len);
            let frame = unsafe { &mut *out };
            match frame_data {
                Some(data) => {
                    frame.data = data.as_ptr();
                    frame.length = jpeg_len as u32;
                }
                None => {
                    frame.data = core::ptr::null();
                    frame.length = 0;
                }
            }
            frame.transfers = 0;
            frame.data_packets = 0;
            frame.fid = uvc::LAST_EOF_FID.load(core::sync::atomic::Ordering::Relaxed);
            0
        }
        Err(e) => {
            log::error!("[USB] UVC capture failed: {:?}", e);
            -5
        }
    }
}
