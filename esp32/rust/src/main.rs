//! AT-Node rust-s3 — entry point.
//!
//! Boot: clocks -> heap -> esp-rtos/embassy -> cfg (flash) -> LED -> AT
//! serial console. Later stages add wifi/mqtt/http/kbd as sibling tasks.
//! Module map per esp32/rust/MIGRATION.md section 4.

#![no_std]
#![feature(type_alias_impl_trait)]
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
mod httpd;
mod hws;
mod led;
mod mqttc;
mod wifi;

use embassy_executor::Spawner;
use embassy_time::{Duration, Timer};
use esp_backtrace as _;
use esp_hal::clock::CpuClock;
use esp_hal::timer::timg::TimerGroup;

extern crate alloc;


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

    // Main heap: 48 KiB static region in internal RAM + the 73 KiB region
    // reclaimed from the 2nd-stage bootloader. Heap and the executor stack
    // share dram_seg (stack gets what statics leave); 200 KiB here starved
    // the stack to 5.5 KiB and the TLS handshake (RSA bignum) blew the
    // stack guard. 128 KiB leaves ~78 KiB of stack. PSRAM heap lands later
    // if pressure shows (MIGRATION 5.3).
    esp_alloc::heap_allocator!(size: 48 * 1024);
    esp_alloc::heap_allocator!(#[esp_hal::ram(reclaimed)] size: 73744);

    let timg0 = TimerGroup::new(peripherals.TIMG0);
    let sw_interrupt =
        esp_hal::interrupt::software::SoftwareInterruptControl::new(peripherals.SW_INTERRUPT);
    esp_rtos::start(timg0.timer0, sw_interrupt.software_interrupt0);

    cfg::init(peripherals.FLASH).await;

    led::init(spawner, peripherals.RMT, peripherals.GPIO48);
    led::status(led::Status::Boot);

    let rng = esp_hal::rng::Rng::new();
    let seed = (rng.random() as u64) << 32 | rng.random() as u64;
    let _ = (peripherals.RNG, peripherals.GPIO8, peripherals.GPIO9);

    // Heap watermark BEFORE the wifi driver allocates (MIGRATION 5.3).
    esp_println::println!("boot: internal heap free {} bytes", esp_alloc::HEAP.free());
    let stack = wifi::init(spawner, peripherals.WIFI, seed);
    spawner.spawn(mqtt_task(stack).expect("spawn mqtt task"));
    httpd::init().await;
    for _ in 0..httpd::ACCEPTORS {
        spawner.spawn(http_acceptor(stack).expect("spawn http acceptor"));
    }
    for _ in 0..httpd::HANDLERS {
        spawner.spawn(http_handler().expect("spawn http handler"));
    }
    spawner.spawn(httpd::restart_task().expect("spawn restart task"));
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

#[allow(
    clippy::large_stack_frames,
    reason = "the mqtt session state lives in the embassy static task pool, not on a call stack"
)]
#[embassy_executor::task]
async fn mqtt_task(stack: embassy_net::Stack<'static>) -> ! {
    mqttc::task(stack, mqttc::take_buffers()).await
}

#[embassy_executor::task(pool_size = 3)]
async fn http_acceptor(stack: embassy_net::Stack<'static>) -> ! {
    httpd::acceptor_task(stack).await
}

#[embassy_executor::task(pool_size = 3)]
async fn http_handler() -> ! {
    httpd::handler_task().await
}
