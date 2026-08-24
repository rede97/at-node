//! AT-Node rust-s3 — entry point.
//!
//! Boot: clocks -> heap -> esp-rtos/embassy -> cfg (flash) -> LED -> AT
//! serial console. Later stages add wifi/mqtt/http/kbd as sibling tasks.
//! Module map per esp32/rust/MIGRATION.md section 4.

#![no_std]
#![feature(type_alias_impl_trait)]
#![feature(asm_experimental_arch)]
#![recursion_limit = "512"]
#![no_main]
#![deny(
    clippy::mem_forget,
    reason = "mem::forget is generally not safe to do with esp_hal types, especially those \
    holding buffers for the duration of a data transfer."
)]
#![allow(
    clippy::large_stack_frames,
    reason = "Embassy task futures live in the executor's static task pool, never on a call stack; \
    the lint cannot see through the task macro. Sync/ISR frame discipline is enforced in review."
)]

mod api;
mod at;
mod at_serial;
mod cfg;
mod kb;
#[cfg(feature = "kbd-ble")]
mod kbd_ble;
#[cfg(feature = "kbd-usb")]
mod kbd_usb;
mod led;
mod rathole;
mod ssdp;
#[cfg(feature = "wifi")]
mod wifi;
#[cfg(not(feature = "wifi"))]
mod wifi {
    //! Stub when WiFi is compiled out (kbd-ble-only R6 bring-up): the
    //! WiFi blob's ~46 KB internal-heap allocation would starve the BLE
    //! controller. Net services all imply the wifi feature.
    pub fn link_up() -> bool {
        false
    }
    pub fn ipv4() -> Option<embassy_net::Ipv4Address> {
        None
    }
    pub fn rssi() -> i32 {
        0
    }
}

#[cfg(feature = "http")]
mod httpd;
#[cfg(not(feature = "http"))]
mod httpd {
    //! Stub when the http feature is compiled out (base variant).
    pub fn enabled() -> bool {
        false
    }
    pub fn running() -> bool {
        false
    }
}

#[cfg(feature = "hws")]
mod hws;
#[cfg(not(feature = "hws"))]
mod hws {
    //! Stub when hws (GPIO/ADC/I2C) is compiled out (rathole variant).
    pub const I2C_IO_MAX: usize = 32;

    pub fn enabled() -> bool {
        false
    }
    pub fn gpio_write(_pin: u8, _level: bool) -> Result<(), ()> {
        Err(())
    }
    pub fn gpio_read(_pin: u8) -> Result<u8, ()> {
        Err(())
    }
    pub fn adc_read_mv(_ch: u8) -> Result<u32, ()> {
        Err(())
    }
    pub async fn i2c_scan(out: &mut heapless::String<600>) {
        out.clear();
        let _ = out.push_str("+I2C: none");
    }
    pub async fn i2c_read(_addr: u8, _reg: u32, _data: &mut [u8]) -> Result<(), ()> {
        Err(())
    }
    pub async fn i2c_write(_addr: u8, _reg: u32, _data: &[u8]) -> Result<(), ()> {
        Err(())
    }
}

#[cfg(feature = "mqtt")]
mod mqttc;
#[cfg(not(feature = "mqtt"))]
mod mqttc {
    //! Stub when the mqtt feature is compiled out (rathole variant).
        #![allow(dead_code)]

    pub fn enabled() -> bool {
        false
    }
    pub fn status() -> (bool, bool) {
        (false, false)
    }
    pub fn status_str() -> &'static str {
        "off"
    }
    pub async fn start() -> Result<(), ()> {
        Err(())
    }
    pub fn stop() {}
    pub async fn publish(_topic: &str, _msg: &str) -> bool {
        false
    }
    pub async fn subscribe(_topic: &str) -> bool {
        false
    }
}

use embassy_executor::Spawner;
use embassy_time::{Duration, Timer};
use esp_backtrace as _;
use esp_hal::clock::CpuClock;
use esp_hal::timer::timg::TimerGroup;

extern crate alloc;

/// Dedicated PSRAM heap (initialized in main before use).
pub static PSRAM_HEAP: esp_alloc::EspHeap = esp_alloc::EspHeap::empty();


// This creates a default app-descriptor required by the esp-idf bootloader.
esp_bootloader_esp_idf::esp_app_desc!();

#[allow(
    clippy::large_stack_frames,
    reason = "it's not unusual to allocate larger buffers etc. in main"
)]
#[esp_rtos::main]
async fn main(spawner: Spawner) -> ! {
    esp_println::logger::init_logger_from_env();

    let config = esp_hal::Config::default().with_cpu_clock(CpuClock::max());
    let peripherals = esp_hal::init(config);

    // PSRAM (8 MiB octal on N8R8) as a DEDICATED heap — deliberately NOT
    // in the global heap: the precompiled WiFi blob allocates from the
    // global heap via plain malloc and silently lands in PSRAM when
    // internal regions fill, which crashes it on the TX path (blob DMA
    // cannot touch external RAM; observed as LoadProhibited in ppTask).
    // Big CPU-side buffers (TLS/HTTP/MQTT socket bufs) allocate from
    // PSRAM_HEAP explicitly, freeing ~50 KiB of internal DRAM for the
    // executor stack; DMA buffers (USB EP, WiFi internals) stay internal.
    // PSRAM (8 MiB octal on N8R8) as a DEDICATED heap — deliberately NOT
    // in the global heap: the precompiled WiFi blob allocates from the
    // global heap via plain malloc and silently lands in PSRAM when
    // internal regions fill, which crashes it on the TX path (blob DMA
    // cannot touch external RAM; observed as LoadProhibited in ppTask).
    // Big CPU-side buffers (TLS/MQTT) allocate from PSRAM_HEAP explicitly.
    let psram = esp_hal::psram::Psram::new(peripherals.PSRAM, Default::default());
    let (psram_start, psram_size) = psram.raw_parts();
    unsafe {
        PSRAM_HEAP.add_region(esp_alloc::HeapRegion::new(
            psram_start,
            psram_size,
            esp_alloc::MemoryCapability::External.into(),
        ));
    }

    // Internal heap: 48 KiB static region + the 73 KiB reclaimed region.
    // Executor stack = dram_seg leftovers after statics; JTAG showed
    // picoserve serve() polling needs ~20 KiB of it and the WiFi blob
    // needs its internal-heap headroom, so big CPU-side buffers live in
    // PSRAM instead (see psram_allocator above).
    // 56K static + 73.7K reclaimed ≈ 130K, matching esp-hal's embassy_coex
    // example (128K): the WiFi blob (~46K) and the BLE controller (~33K)
    // do not fit the old 77.7K heap (BLE assert emi.c 164). Executor stack
    // keeps ~120K of dram_seg — MQTT/TLS handshake verified.
    esp_alloc::heap_allocator!(size: 56 * 1024);
    esp_alloc::heap_allocator!(#[esp_hal::ram(reclaimed)] size: 73744);

    let timg0 = TimerGroup::new(peripherals.TIMG0);
    let sw_interrupt =
        esp_hal::interrupt::software::SoftwareInterruptControl::new(peripherals.SW_INTERRUPT);
    esp_rtos::start(timg0.timer0, sw_interrupt.software_interrupt0);

    cfg::init(peripherals.FLASH).await;

    led::init(spawner, peripherals.RMT, peripherals.GPIO48);
    led::status(led::Status::Boot);

    let rng = esp_hal::rng::Rng::new();
    let _ = (peripherals.RNG, peripherals.GPIO8, peripherals.GPIO9);

    // Heap watermark BEFORE the wifi driver allocates (MIGRATION 5.3).
    esp_println::println!("boot: internal heap free {} bytes", esp_alloc::HEAP.free());
    // BLE BEFORE WiFi (esp-hal embassy_coex example order): the BT
    // controller's memory pools are carved at ble_init; when WiFi runs
    // first the later controller comes up but advertising dies with HCI
    // Memory Capacity Exceeded.
    #[cfg(feature = "kbd-ble")]
    kbd_ble::init(spawner, peripherals.BT);
    #[cfg(feature = "wifi")]
    let stack = {
        let seed = (rng.random() as u64) << 32 | rng.random() as u64;
        wifi::init(spawner, peripherals.WIFI, seed)
    };
    #[cfg(not(feature = "wifi"))]
    let _ = (rng, peripherals.WIFI);
    #[cfg(feature = "mqtt")]
    spawner.spawn(mqtt_task(stack).expect("spawn mqtt task"));
    #[cfg(feature = "rathole")]
    {
        rathole::init().await;
        spawner.spawn(rathole_task(stack).expect("spawn rathole task"));
        spawner.spawn(rathole_watch().expect("spawn rathole watcher"));
        spawner.spawn(rathole_fwd(stack).expect("spawn rathole forwarders"));
    }
    #[cfg(feature = "http")]
    {
        httpd::init().await;
        for _ in 0..httpd::ACCEPTORS {
            spawner.spawn(http_acceptor(stack).expect("spawn http acceptor"));
        }
        for _ in 0..httpd::HANDLERS {
            spawner.spawn(http_handler().expect("spawn http handler"));
        }
        spawner.spawn(httpd::restart_task().expect("spawn restart task"));
    }
    #[cfg(feature = "ssdp")]
    spawner.spawn(ssdp_task(stack).expect("spawn ssdp task"));
    #[cfg(feature = "hws")]
    hws::init(
        peripherals.I2C0,
        peripherals.ADC1,
        peripherals.GPIO1,
        peripherals.GPIO2,
        peripherals.GPIO3,
        peripherals.GPIO4,
        peripherals.GPIO5,
        peripherals.GPIO6,
        peripherals.GPIO7,
        peripherals.GPIO10,
    );
    #[cfg(not(feature = "hws"))]
    let _ = (
        peripherals.I2C0,
        peripherals.ADC1,
        peripherals.GPIO1,
        peripherals.GPIO2,
        peripherals.GPIO3,
        peripherals.GPIO4,
        peripherals.GPIO5,
        peripherals.GPIO6,
        peripherals.GPIO7,
        peripherals.GPIO10,
    );
    #[cfg(feature = "kbd-usb")]
    kbd_usb::init(
        spawner,
        peripherals.USB0,
        peripherals.GPIO20,
        peripherals.GPIO19,
    );
    #[cfg(any(feature = "kbd-usb", feature = "kbd-ble"))]
    spawner.spawn(kb_engine().expect("spawn kb engine"));

    at_serial::init(
        spawner,
        peripherals.UART0,
        peripherals.GPIO44,
        peripherals.GPIO43,
    );

    // Boot heap watermark (MIGRATION section 5.3: record internal RAM
    // headroom per stage). println so it shows regardless of log level.
    esp_println::println!("boot: internal heap free {} bytes", esp_alloc::HEAP.free());

    loop {
        Timer::after(Duration::from_secs(60)).await;
    }
}

#[cfg(feature = "mqtt")]
#[allow(
    clippy::large_stack_frames,
    reason = "the mqtt session state lives in the embassy static task pool, not on a call stack"
)]
#[embassy_executor::task]
async fn mqtt_task(stack: embassy_net::Stack<'static>) -> ! {
    mqttc::task(stack, mqttc::take_buffers()).await
}

#[cfg(feature = "http")]
#[embassy_executor::task(pool_size = 3)]
async fn http_acceptor(stack: embassy_net::Stack<'static>) -> ! {
    httpd::acceptor_task(stack).await
}

#[cfg(feature = "http")]
#[embassy_executor::task(pool_size = 1)]
async fn http_handler() -> ! {
    httpd::handler_task().await
}

#[cfg(feature = "rathole")]
#[embassy_executor::task]
async fn rathole_task(stack: embassy_net::Stack<'static>) -> ! {
    rathole::task(stack).await
}

#[cfg(feature = "rathole")]
#[embassy_executor::task]
async fn rathole_watch() -> ! {
    rathole::watch_task().await
}

#[cfg(feature = "rathole")]
#[embassy_executor::task(pool_size = 2)]
async fn rathole_fwd(stack: embassy_net::Stack<'static>) -> ! {
    rathole::forward_loop(stack).await
}

#[cfg(feature = "ssdp")]
#[embassy_executor::task]
async fn ssdp_task(stack: embassy_net::Stack<'static>) -> ! {
    ssdp::task(stack).await
}

#[cfg(any(feature = "kbd-usb", feature = "kbd-ble"))]
#[embassy_executor::task]
async fn kb_engine() -> ! {
    kb::engine_task().await
}
