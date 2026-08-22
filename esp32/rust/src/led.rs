//! AT-Node rust-s3 — WS2812 status LED (GPIO48, RMT channel 0).
//!
//! Semantics aligned with esp32/zephyr/src/led.c: tracked status presets
//! (boot yellow / wifi-connecting slow-blink blue / online green / error
//! red) vs a free custom color that holds until the next status call.
//! All updates run inside this async task; other modules only post
//! commands to the channel (no RMT traffic outside task context).

use embassy_executor::Spawner;
use embassy_futures::select::{Either, select};
use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::channel::Channel;
use embassy_time::{Duration, Timer};
use esp_hal::Async;
use esp_hal::gpio::Level;
use esp_hal::peripherals::{GPIO48, RMT};
use esp_hal::rmt::{PulseCode, Rmt, Tx, TxChannelConfig, TxChannelCreator};
use esp_hal::time::Rate;

/// Preset brightness for status colors (Zephyr BRIGHTNESS 0x20).
const BRIGHT: u8 = 0x20;

/// Blink half-period for LED_WIFI_CONNECTING (Zephyr 500 ms timer).
const BLINK_MS: u64 = 500;

// WS2812B pulse widths in 80 MHz RMT ticks (12.5 ns).
const PULSE_0: PulseCode = PulseCode::new(Level::High, 32, Level::Low, 68);
const PULSE_1: PulseCode = PulseCode::new(Level::High, 64, Level::Low, 56);

/// Tracked status presets (led_status enum in the Zephyr variant).
#[derive(Clone, Copy, PartialEq, Eq)]
#[allow(dead_code)] // Online/Error are driven by wifi/mqtt from R2/R3 on
pub enum Status {
    Boot,
    WifiConnecting,
    Online,
    Error,
}

enum Cmd {
    Status(Status),
    Rgb(u8, u8, u8),
    Off,
    /// Re-apply the current tracked status (AT+LED=auto).
    Auto,
}

static CMD: Channel<CriticalSectionRawMutex, Cmd, 4> = Channel::new();

/// Switch to a tracked preset (clears any custom color).
pub fn status(s: Status) {
    let _ = CMD.try_send(Cmd::Status(s));
}

/// Free custom color; holds until the next status()/auto() call.
pub fn set_rgb(r: u8, g: u8, b: u8) {
    let _ = CMD.try_send(Cmd::Rgb(r, g, b));
}

pub fn off() {
    let _ = CMD.try_send(Cmd::Off);
}

/// Restore the current tracked status (AT+LED=auto).
pub fn auto() {
    let _ = CMD.try_send(Cmd::Auto);
}

/// Bring up the RMT channel and spawn the LED task. Call once from main.
pub fn init(spawner: Spawner, rmt: RMT<'static>, pin: GPIO48<'static>) {
    let rmt = Rmt::new(rmt, Rate::from_mhz(80))
        .expect("rmt init")
        .into_async();
    let tx = rmt
        .channel0
        .configure_tx(
            &TxChannelConfig::default()
                .with_clk_divider(1)
                .with_idle_output(true)
                .with_idle_output_level(Level::Low),
        )
        .expect("rmt channel")
        .with_pin(pin);
    spawner.spawn(led_task(tx).expect("spawn led task"));
}

fn preset(s: Status, blink_on: bool) -> (u8, u8, u8) {
    match s {
        Status::Boot => (BRIGHT, BRIGHT, 0),
        Status::WifiConnecting => (0, 0, if blink_on { BRIGHT } else { 0 }),
        Status::Online => (0, BRIGHT, 0),
        Status::Error => (BRIGHT, 0, 0),
    }
}

async fn apply(tx: &mut esp_hal::rmt::Channel<'_, Async, Tx>, r: u8, g: u8, b: u8) {
    let mut data = [PulseCode::end_marker(); 25];
    // WS2812B wire order is GRB.
    for (i, byte) in [g, r, b].iter().enumerate() {
        for bit in 0..8 {
            data[i * 8 + bit] = if byte & (0x80 >> bit) != 0 {
                PULSE_1
            } else {
                PULSE_0
            };
        }
    }
    let _ = tx.transmit(&data).await;
}

#[embassy_executor::task]
async fn led_task(mut tx: esp_hal::rmt::Channel<'static, Async, Tx>) {
    let mut cur = Status::Boot;
    let mut custom = false;
    let mut blink_on = false;
    apply(&mut tx, BRIGHT, BRIGHT, 0).await;

    loop {
        let cmd = if !custom && cur == Status::WifiConnecting {
            match select(CMD.receive(), Timer::after(Duration::from_millis(BLINK_MS))).await {
                Either::First(c) => c,
                Either::Second(()) => {
                    blink_on = !blink_on;
                    let (r, g, b) = preset(cur, blink_on);
                    apply(&mut tx, r, g, b).await;
                    continue;
                }
            }
        } else {
            CMD.receive().await
        };

        match cmd {
            Cmd::Status(s) => {
                cur = s;
                custom = false;
                blink_on = false;
                let (r, g, b) = preset(cur, true);
                apply(&mut tx, r, g, b).await;
            }
            Cmd::Auto => {
                custom = false;
                let (r, g, b) = preset(cur, true);
                apply(&mut tx, r, g, b).await;
            }
            Cmd::Rgb(r, g, b) => {
                custom = true;
                apply(&mut tx, r, g, b).await;
            }
            Cmd::Off => {
                custom = true;
                apply(&mut tx, 0, 0, 0).await;
            }
        }
    }
}
