#![no_std]

extern crate alloc;
extern crate kernel_support;

use alloc::boxed::Box;
use core::{ffi::c_void, task::{Context, Poll, RawWaker, RawWakerVTable, Waker}};

use aic8800::{PollFn, SendPollFn, TimedOut, WifiRuntime};
use rd_net::WifiControl;
use sdio_host::SdioHost;
use sdhci_cv1800::hw_init::{sdio1_hw_init, Sdio1HwConfig};
use sdhci_cv1800::SdhciDelay;

const AP_SSID: &[u8] = b"PicoClaw-Car";
const AP_CHANNEL: u8 = 6;

unsafe extern "C" {
    fn timer_get_uptime_ms() -> u64;
    fn timer_spin(ticks: u32);
    fn task_yield();
    fn wifi_task_spawn(
        name: *const u8,
        entry: extern "C" fn(*mut c_void),
        arg: *mut c_void,
        priority: u8,
    ) -> i32;
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
            log::info!("[wifi] SDIO card initialized: vendor=0x{:04x} device=0x{:04x}", vid, did);
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

            let _wifi = Box::leak(Box::new(wifi));
            0
        }
        Err(e) => {
            log::error!("[wifi] SDIO host init failed: {:?}", e);
            -2
        }
    }
}
