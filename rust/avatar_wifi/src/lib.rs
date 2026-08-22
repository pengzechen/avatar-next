#![no_std]

extern crate alloc;
extern crate kernel_support;

use alloc::boxed::Box;
use core::{
    ffi::c_void,
    sync::atomic::{AtomicUsize, Ordering},
    task::{Context, Poll, RawWaker, RawWakerVTable, Waker},
};

use aic8800::{PollFn, SendPollFn, TimedOut, WifiRuntime};
use rd_net::{Interface, WifiControl};
use sdhci_cv1800::hw_init::{sdio1_hw_init, Sdio1HwConfig};
use sdhci_cv1800::SdhciDelay;
use sdio_host::SdioHost;

const AP_SSID: &[u8] = b"PicoClaw-Car";
const AP_CHANNEL: u8 = 6;

unsafe extern "C" {
    fn timer_get_uptime_ms() -> u64;
    fn timer_spin(ticks: u32);
    fn task_yield();
    fn wifi_net_rx_wake();
    fn wifi_task_spawn(
        name: *const u8,
        entry: extern "C" fn(*mut c_void),
        arg: *mut c_void,
        priority: u8,
    ) -> i32;
}

fn wake_c_net_poll() {
    unsafe { wifi_net_rx_wake() };
}

fn ensure_logger() {
    static INITED: core::sync::atomic::AtomicBool = core::sync::atomic::AtomicBool::new(false);
    if !INITED.swap(true, core::sync::atomic::Ordering::Relaxed) {
        kernel_support::init_logger();
    }
}

struct AvatarWifiRuntime;

struct WifiPollTask {
    poll: Box<SendPollFn>,
}

impl AvatarWifiRuntime {
    fn dummy_waker() -> Waker {
        unsafe fn clone(_: *const ()) -> RawWaker {
            RawWaker::new(core::ptr::null(), &VTABLE)
        }
        unsafe fn wake(_: *const ()) {}
        unsafe fn wake_by_ref(_: *const ()) {}
        unsafe fn drop(_: *const ()) {}
        static VTABLE: RawWakerVTable = RawWakerVTable::new(clone, wake, wake_by_ref, drop);
        unsafe { Waker::from_raw(RawWaker::new(core::ptr::null(), &VTABLE)) }
    }
}

extern "C" fn wifi_poll_task_entry(arg: *mut c_void) {
    let mut task = unsafe { Box::from_raw(arg as *mut WifiPollTask) };
    let waker = AvatarWifiRuntime::dummy_waker();
    let mut cx = Context::from_waker(&waker);

    loop {
        if let Poll::Ready(()) = (task.poll)(&mut cx) {
            break;
        }
        unsafe { task_yield() };
    }
}

impl WifiRuntime for AvatarWifiRuntime {
    fn now_nanos(&self) -> u64 {
        unsafe { timer_get_uptime_ms() * 1_000_000 }
    }

    fn sleep_ms(&self, ms: u64) {
        for _ in 0..ms {
            unsafe { timer_spin(4000) };
        }
    }

    fn yield_now(&self) {
        unsafe { task_yield() };
    }

    fn spawn_poll_task(&self, name: &str, poll: Box<SendPollFn>) {
        let mut task_name = [0u8; 16];
        let bytes = name.as_bytes();
        let len = core::cmp::min(bytes.len(), task_name.len() - 1);
        task_name[..len].copy_from_slice(&bytes[..len]);

        let task = Box::new(WifiPollTask { poll });
        let arg = Box::into_raw(task) as *mut c_void;
        let rc = unsafe { wifi_task_spawn(task_name.as_ptr(), wifi_poll_task_entry, arg, 20) };
        if rc != 0 {
            unsafe { drop(Box::from_raw(arg as *mut WifiPollTask)) };
            log::error!("[wifi] failed to spawn poll task '{}'", name);
        }
    }

    fn block_until(&self, timeout_ms: Option<u64>, poll: &mut PollFn<'_>) -> Result<(), TimedOut> {
        let start = self.now_nanos();
        let timeout_ns = timeout_ms.map(|ms| ms * 1_000_000);
        let waker = Self::dummy_waker();
        let mut cx = Context::from_waker(&waker);
        loop {
            if let Poll::Ready(()) = poll(&mut cx) {
                return Ok(());
            }
            if let Some(ns) = timeout_ns {
                if self.now_nanos().wrapping_sub(start) >= ns {
                    return Err(TimedOut);
                }
            }
            <Self as WifiRuntime>::yield_now(self);
        }
    }
}

impl SdhciDelay for AvatarWifiRuntime {
    fn delay_ms(&self, ms: u64) {
        <Self as WifiRuntime>::sleep_ms(self, ms);
    }

    fn yield_now(&self) {
        <Self as WifiRuntime>::yield_now(self);
    }
}

static AVATAR_WIFI_RUNTIME: AvatarWifiRuntime = AvatarWifiRuntime;
static WIFI_DEV_PTR: AtomicUsize = AtomicUsize::new(0);
static WIFI_NET_RX_COUNT: AtomicUsize = AtomicUsize::new(0);
static WIFI_NET_TX_COUNT: AtomicUsize = AtomicUsize::new(0);
static WIFI_NET_RX_EMPTY_COUNT: AtomicUsize = AtomicUsize::new(0);

#[unsafe(no_mangle)]
pub unsafe extern "C" fn wifi_runtime_init() -> i32 {
    ensure_logger();
    sdhci_cv1800::set_delay(&AVATAR_WIFI_RUNTIME);
    aic8800::set_runtime(&AVATAR_WIFI_RUNTIME);
    log::info!("[wifi] runtime installed");
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn wifi_sdio1_irq_handler() {
    sdhci_cv1800::irq::sdhci_irq_handler(0);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn wifi_sdio1_probe(
    crg_base: usize,
    sysctrl_base: usize,
    rtcsys_ctrl_base: usize,
    rtcsys_io_base: usize,
    sdio1_base: usize,
) -> i32 {
    ensure_logger();

    if crg_base == 0
        || sysctrl_base == 0
        || rtcsys_ctrl_base == 0
        || rtcsys_io_base == 0
        || sdio1_base == 0
    {
        log::error!(
            "[wifi] missing SDIO1 base(s): crg={:#x} sysctrl={:#x} rtcsys_ctrl={:#x} rtcsys_io={:#x} sdio1={:#x}",
            crg_base,
            sysctrl_base,
            rtcsys_ctrl_base,
            rtcsys_io_base,
            sdio1_base
        );
        return -1;
    }

    log::info!(
        "[wifi] SDIO1 probe: crg={:#x} sysctrl={:#x} rtcsys_ctrl={:#x} rtcsys_io={:#x} sdio1={:#x}",
        crg_base,
        sysctrl_base,
        rtcsys_ctrl_base,
        rtcsys_io_base,
        sdio1_base
    );

    let cfg = Sdio1HwConfig {
        crg_base_va: crg_base,
        sysctrl_base_va: sysctrl_base,
        rtcsys_ctrl_base_va: rtcsys_ctrl_base,
        rtcsys_io_base_va: rtcsys_io_base,
        sdio1_base_va: sdio1_base,
    };
    sdio1_hw_init(&cfg);
    log::info!("[wifi] SDIO1 SoC hw init done");

    let mut host = sdhci_cv1800::CviSdhci::new(sdio1_base);
    match host.init() {
        Ok(()) => {
            let (vid, did) = host.vendor_device_id();
            log::info!(
                "[wifi] SDIO card initialized: vendor=0x{:04x} device=0x{:04x}",
                vid,
                did
            );
            host.prepare_first_data_xfer();

            let mut wifi = match aic8800::probe(host) {
                Ok(wifi) => wifi,
                Err(e) => {
                    log::error!("[wifi] chip probe failed: {}", e);
                    return -3;
                }
            };
            log::info!("[wifi] chip probe complete");

            if let Err(e) = wifi.start_ap_open(AP_SSID, AP_CHANNEL) {
                log::error!("[wifi] AP start failed: {:?}", e);
                return -4;
            }
            log::info!("[wifi] SoftAP started, channel {}", AP_CHANNEL);

            wifi.set_rx_wake(wake_c_net_poll);

            let wifi = Box::leak(Box::new(wifi));
            WIFI_DEV_PTR.store(wifi as *mut _ as usize, Ordering::Release);
            0
        }
        Err(e) => {
            log::error!("[wifi] SDIO host init failed: {:?}", e);
            -2
        }
    }
}

fn with_wifi<R>(f: impl FnOnce(&mut aic8800::fdrv::AicWifiNetDev) -> R) -> Option<R> {
    let ptr = WIFI_DEV_PTR.load(Ordering::Acquire);
    if ptr == 0 {
        return None;
    }
    let wifi = unsafe { &mut *(ptr as *mut aic8800::fdrv::AicWifiNetDev) };
    Some(f(wifi))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn wifi_net_send(frame: *const u8, len: usize) -> i32 {
    if frame.is_null() || len == 0 {
        return -1;
    }
    let data = unsafe { core::slice::from_raw_parts(frame, len) };
    let rc = with_wifi(|wifi| match wifi.send_ethernet_frame(data) {
        Ok(()) => {
            let n = WIFI_NET_TX_COUNT.fetch_add(1, Ordering::Relaxed) + 1;
            if n <= 8 || n % 32 == 0 {
                let etype = if len >= 14 {
                    u16::from_be_bytes([data[12], data[13]])
                } else {
                    0
                };
                log::info!("[wifi-net] tx #{} len={} etype=0x{:04x}", n, len, etype);
            }
            0
        }
        Err(_) => -1,
    });
    rc.unwrap_or(-1)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn wifi_net_recv(frame: *mut u8, maxlen: usize) -> i32 {
    if frame.is_null() || maxlen == 0 {
        return -1;
    }
    let rc = with_wifi(|wifi| {
        let out = unsafe { core::slice::from_raw_parts_mut(frame, maxlen) };
        match wifi.recv_ethernet_frame(out) {
            Some(len) => {
                let n = WIFI_NET_RX_COUNT.fetch_add(1, Ordering::Relaxed) + 1;
                if n <= 16 || n % 32 == 0 {
                    let etype = if len >= 14 {
                        u16::from_be_bytes([out[12], out[13]])
                    } else {
                        0
                    };
                    log::info!("[wifi-net] rx #{} len={} etype=0x{:04x}", n, len, etype);
                }
                len as i32
            }
            None => {
                let n = WIFI_NET_RX_EMPTY_COUNT.fetch_add(1, Ordering::Relaxed) + 1;
                if n <= 8 || n.is_power_of_two() {
                    log::info!("[wifi-net] rx empty #{}", n);
                }
                0
            }
        }
    });
    rc.unwrap_or(-1)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn wifi_net_mac(mac: *mut u8) {
    if mac.is_null() {
        return;
    }
    let _ = with_wifi(|wifi| {
        let m = wifi.mac_address();
        unsafe { core::ptr::copy_nonoverlapping(m.as_ptr(), mac, 6) };
    });
}
