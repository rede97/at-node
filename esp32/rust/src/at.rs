//! AT-Node rust-s3 — channel-independent AT parser/dispatcher.
//!
//! Semantics aligned with esp32/zephyr/src/at_core.c; response formats
//! follow esp32/arduino/arduino.ino: data lines first, then exactly one
//! `OK` or `ERROR <reason>` line. Every transport (serial now, HTTP/MQTT
//! later) must call this single handle_line() — no per-channel commands.

use core::fmt::Write as _;

use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::mutex::Mutex;
use embassy_time::{Duration, Timer};
use heapless::String;

use crate::{cfg, led};

/// Response sink: one call per completed output line (no trailing CR/LF).
pub trait AtSink {
    async fn emit(&mut self, line: &str);
}

/// Scratch for oversized responses (KEYS json) so nothing big lands on a
/// task stack (clippy large_stack_frames budget).
struct Scratch {
    buf: String<1600>,
}

static AT: Mutex<CriticalSectionRawMutex, Scratch> = Mutex::new(Scratch {
    buf: String::new(),
});

/// Base-0 integer parse (Zephyr to_u32: accepts 0x.. like Arduino).
fn to_u32(s: &str) -> Option<u32> {
    match s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        Some(h) => u32::from_str_radix(h, 16).ok(),
        None => s.parse().ok(),
    }
}

const HELP: &[&str] = &[
    "AT-Node rust-s3 commands:",
    "  AT / AT+STATUS / AT+VER / AT+HELP",
    "  AT+SET=<key>=<val> / AT+GET=<key> / AT+KEYS  config",
    "  AT+WIFI=ssid|pass,<val> / AT+WIFI=status",
    "  AT+MQTT=connect|disconnect|status|enable,0|1|auto,0|1",
    "  AT+HTTP=status|enable,0|1|auto,0|1",
    "  AT+GPIO_W=<pin>,<level> / AT+GPIO_R=<pin>",
    "  AT+ADC=<ch> / AT+I2C_SCAN / AT+I2C_R=<addr>,<reg>,<len>",
    "  AT+I2C_W=<addr>,<reg>,<d0>,<d1>,...",
    "  AT+TAP=<key>[,<mods>][,<ms>] / AT+KEY=<mods>,<k0>[,<k1..k5>]",
    "  AT+KEY_STR=<text> / AT+KEY_SEQ=<ms>,<mods>,<k0..k5>,... / AT+DEV=USB|BLE",
    "  AT+PAIR / AT+BLE=status|bonds|clear",
    "  AT+LED=<r>,<g>,<b>|#RRGGBB|off|auto / AT+LED?",
    "  AT+TUNNEL=status|connect|disconnect|clear|enable[,<0|1>]",
    "  AT+TUNNEL=server|token|service|local|auto|retry,<val>",
    "  AT+NVS=clear / AT+RST",
];

/// Handle one command line; emits data lines then OK / ERROR <reason>.
pub async fn handle_line<S: AtSink>(line: &str, out: &mut S) {
    let mut scratch = AT.lock().await;

    let line = line.trim_end_matches([' ', '\t', '\r', '\n']);
    if line.is_empty() {
        return;
    }

    if line == "AT" {
        out.emit("OK").await;
    } else if line == "AT+VER" {
        out.emit("AT-Node v1.0 [rust-s3]").await;
        out.emit("OK").await;
    } else if line == "AT+ABILITY" {
        scratch.buf.clear();
        let _ = write!(scratch.buf, "+ABILITY:{}", crate::api::ability_json());
        out.emit(scratch.buf.as_str()).await;
        out.emit("OK").await;
    } else if line == "AT+HELP" {
        for l in HELP {
            out.emit(l).await;
        }
        out.emit("OK").await;
    } else if let Some(args) = line.strip_prefix("AT+SET=") {
        match args.split_once('=') {
            Some((key, val)) if !key.is_empty() => match cfg::set(key, val).await {
                Ok(()) => out.emit("OK").await,
                Err(cfg::Error::UnknownKey) => out.emit("ERROR unknown key").await,
                Err(cfg::Error::BadValue) => out.emit("ERROR bad value").await,
                Err(_) => out.emit("ERROR flash").await,
            },
            _ => out.emit("ERROR bad args").await,
        }
    } else if let Some(key) = line.strip_prefix("AT+GET=") {
        match cfg::get(key).await {
            Ok(val) => {
                scratch.buf.clear();
                if write!(scratch.buf, "+GET:{key}={val}").is_ok() {
                    out.emit(scratch.buf.as_str()).await;
                    out.emit("OK").await;
                } else {
                    out.emit("ERROR bad value").await;
                }
            }
            Err(cfg::Error::UnknownKey) => out.emit("ERROR unknown key").await,
            Err(cfg::Error::WriteOnly) => out.emit("ERROR write-only").await,
            Err(_) => out.emit("ERROR bad value").await,
        }
    } else if line == "AT+KEYS" {
        cfg::list_json(&mut scratch.buf).await;
        scratch.buf.insert_str(0, "+KEYS:").ok();
        out.emit(scratch.buf.as_str()).await;
        out.emit("OK").await;
    } else if line == "AT+LED?" {
        let (r, g, b, mode) = led::current();
        scratch.buf.clear();
        let _ = write!(scratch.buf, "+LED:{r},{g},{b},{mode}");
        out.emit(scratch.buf.as_str()).await;
        out.emit("OK").await;
    } else if let Some(args) = line.strip_prefix("AT+LED=") {
        cmd_led(args, out).await;
    } else if line == "AT+DEV?" {
        if !crate::kb::enabled() {
            out.emit("ERROR kbd disabled").await;
            return;
        }
        scratch.buf.clear();
        let _ = write!(scratch.buf, "+DEV:{}", crate::kb::target_str());
        out.emit(scratch.buf.as_str()).await;
        out.emit("OK").await;
    } else if let Some(args) = line.strip_prefix("AT+DEV=") {
        if !crate::kb::enabled() {
            out.emit("ERROR kbd disabled").await;
            return;
        }
        let t = match args {
            "USB" => crate::kb::Target::Usb,
            "BLE" => crate::kb::Target::Ble,
            _ => {
                out.emit("ERROR bad args").await;
                return;
            }
        };
        match crate::kb::set_target(t) {
            Ok(()) => out.emit("OK").await,
            Err(()) => out.emit("ERROR bad target").await,
        }
    } else if line == "AT+PAIR" {
        #[cfg(feature = "kbd-ble")]
        {
            crate::kbd_ble::pair_open();
            scratch.buf.clear();
            let _ = write!(scratch.buf, "+PAIR:window {}s", crate::kbd_ble::PAIR_WINDOW_SECS);
            out.emit(scratch.buf.as_str()).await;
            out.emit("OK").await;
        }
        #[cfg(not(feature = "kbd-ble"))]
        out.emit("ERROR ble disabled").await;
    } else if let Some(sub) = line.strip_prefix("AT+BLE=") {
        cmd_ble(sub, &mut scratch, out).await;
    } else if let Some(args) = line.strip_prefix("AT+TAP=") {
        cmd_tap(args, out).await;
    } else if let Some(args) = line.strip_prefix("AT+KEY_STR=") {
        cmd_key_str(args, out).await;
    } else if let Some(args) = line.strip_prefix("AT+KEY_SEQ=") {
        cmd_key_seq(args, out).await;
    } else if let Some(args) = line.strip_prefix("AT+KEY=") {
        cmd_key(args, out).await;
    } else if let Some(args) = line.strip_prefix("AT+TUNNEL=") {
        cmd_tunnel(args, &mut scratch, out).await;
    } else if let Some(sub) = line.strip_prefix("AT+NVS=") {
        if sub == "clear" {
            match cfg::erase_all().await {
                Ok(()) => {
                    out.emit("NVS erased, reboot with AT+RST").await;
                    out.emit("OK").await;
                }
                Err(_) => out.emit("ERROR flash").await,
            }
        } else {
            out.emit("ERROR bad args").await;
        }
    } else if let Some(args) = line.strip_prefix("AT+GPIO_W=") {
        cmd_gpio_w(args, out).await;
    } else if let Some(args) = line.strip_prefix("AT+GPIO_R=") {
        cmd_gpio_r(args, &mut scratch, out).await;
    } else if let Some(args) = line.strip_prefix("AT+ADC=") {
        cmd_adc(args, &mut scratch, out).await;
    } else if line == "AT+I2C_SCAN" {
        if !crate::hws::enabled() {
            out.emit("ERROR hws disabled").await;
            return;
        }
        let mut buf: String<600> = String::new();
        crate::hws::i2c_scan(&mut buf).await;
        out.emit(buf.as_str()).await;
        out.emit("OK").await;
    } else if let Some(args) = line.strip_prefix("AT+I2C_R=") {
        cmd_i2c_r(args, &mut scratch, out).await;
    } else if let Some(args) = line.strip_prefix("AT+I2C_W=") {
        cmd_i2c_w(args, out).await;
    } else if line == "AT+STATUS" {
        cmd_status(&mut scratch, out).await;
    } else if let Some(args) = line.strip_prefix("AT+WIFI=") {
        cmd_wifi(args, out).await;
    } else if let Some(args) = line.strip_prefix("AT+MQTT=") {
        cmd_mqtt(args, &mut scratch, out).await;
    } else if let Some(args) = line.strip_prefix("AT+HTTP=") {
        cmd_http(args, &mut scratch, out).await;
    } else if line == "AT+HTTP" {
        cmd_http("status", &mut scratch, out).await;
    } else if line == "AT+RST" {
        out.emit("OK").await;
        Timer::after(Duration::from_millis(100)).await; // let transport flush
        esp_hal::system::software_reset();
    } else {
        out.emit("ERROR unknown cmd").await;
    }
}

/// Split args on ',' into up to N fields (Zephyr split()).
fn split<'a, const N: usize>(args: &'a str, fields: &mut [&'a str; N]) -> usize {
    let mut n = 0;
    for f in args.split(',') {
        if n >= N {
            break;
        }
        fields[n] = f;
        n += 1;
    }
    n
}

/// AT+GPIO_W=<pin>,<level> (Zephyr cmd_gpio_w).
async fn cmd_gpio_w<S: AtSink>(args: &str, out: &mut S) {
    if !crate::hws::enabled() {
        out.emit("ERROR hws disabled").await;
        return;
    }
    let mut f = [""; 2];
    if split(args, &mut f) != 2 {
        out.emit("ERROR bad args").await;
        return;
    }
    match (to_u32(f[0]), to_u32(f[1])) {
        (Some(pin), Some(level)) if pin <= 255 => {
            if crate::hws::gpio_write(pin as u8, level != 0).is_ok() {
                out.emit("OK").await;
            } else {
                out.emit("ERROR unsafe pin").await;
            }
        }
        _ => out.emit("ERROR unsafe pin").await,
    }
}

/// AT+GPIO_R=<pin> (Zephyr cmd_gpio_r; arduino.ino:2772 format).
async fn cmd_gpio_r<S: AtSink>(args: &str, scratch: &mut Scratch, out: &mut S) {
    if !crate::hws::enabled() {
        out.emit("ERROR hws disabled").await;
        return;
    }
    match to_u32(args) {
        Some(pin) if pin <= 255 => match crate::hws::gpio_read(pin as u8) {
            Ok(level) => {
                scratch.buf.clear();
                let _ = write!(scratch.buf, "+GPIO_R:{level}");
                out.emit(scratch.buf.as_str()).await;
                out.emit("OK").await;
            }
            Err(()) => out.emit("ERROR bad pin").await,
        },
        _ => out.emit("ERROR bad pin").await,
    }
}

/// AT+ADC=<ch> (Zephyr cmd_adc; arduino.ino:2779 format, millivolts).
async fn cmd_adc<S: AtSink>(args: &str, scratch: &mut Scratch, out: &mut S) {
    if !crate::hws::enabled() {
        out.emit("ERROR hws disabled").await;
        return;
    }
    match to_u32(args) {
        Some(ch) if ch <= 255 => match crate::hws::adc_read_mv(ch as u8) {
            Ok(mv) => {
                scratch.buf.clear();
                let _ = write!(scratch.buf, "+ADC:{mv}");
                out.emit(scratch.buf.as_str()).await;
                out.emit("OK").await;
            }
            Err(()) => out.emit("ERROR bad channel").await,
        },
        _ => out.emit("ERROR bad channel").await,
    }
}

/// AT+I2C_R=<addr>,<reg>,<len> (Zephyr cmd_i2c_r).
async fn cmd_i2c_r<S: AtSink>(args: &str, scratch: &mut Scratch, out: &mut S) {
    if !crate::hws::enabled() {
        out.emit("ERROR hws disabled").await;
        return;
    }
    let mut f = [""; 3];
    if split(args, &mut f) != 3 {
        out.emit("ERROR bad args").await;
        return;
    }
    let (Some(addr), Some(reg), Some(len)) = (to_u32(f[0]), to_u32(f[1]), to_u32(f[2])) else {
        out.emit("ERROR bad args").await;
        return;
    };
    if len == 0 || len > crate::hws::I2C_IO_MAX as u32 {
        out.emit("ERROR bad args").await;
        return;
    }
    let mut data = [0u8; crate::hws::I2C_IO_MAX];
    if crate::hws::i2c_read(addr as u8, reg, &mut data[..len as usize])
        .await
        .is_err()
    {
        out.emit("ERROR i2c").await;
        return;
    }
    // "+I2C_R:0A 1F" — hex bytes after colon, space separated.
    scratch.buf.clear();
    let _ = scratch.buf.push_str("+I2C_R:");
    for (i, b) in data[..len as usize].iter().enumerate() {
        if i > 0 {
            let _ = scratch.buf.push(' ');
        }
        let _ = write!(scratch.buf, "{b:02X}");
    }
    out.emit(scratch.buf.as_str()).await;
    out.emit("OK").await;
}

/// AT+I2C_W=<addr>,<reg>,<d0>,<d1>,... (Zephyr cmd_i2c_w).
async fn cmd_i2c_w<S: AtSink>(args: &str, out: &mut S) {
    let mut f = [""; crate::hws::I2C_IO_MAX + 2];
    let n = split(args, &mut f);
    if n < 3 {
        out.emit("ERROR bad args").await;
        return;
    }
    let (Some(addr), Some(reg)) = (to_u32(f[0]), to_u32(f[1])) else {
        out.emit("ERROR bad args").await;
        return;
    };
    let mut data = [0u8; crate::hws::I2C_IO_MAX];
    for (i, field) in f[2..n].iter().enumerate() {
        // Zephyr casts without range check ((uint8_t)to_u32).
        data[i] = to_u32(field).unwrap_or(0) as u8;
    }
    if crate::hws::i2c_write(addr as u8, reg, &data[..n - 2])
        .await
        .is_ok()
    {
        out.emit("OK").await;
    } else {
        out.emit("ERROR i2c").await;
    }
}

/// AT+WIFI=ssid|pass,<val> / AT+WIFI=status (Zephyr cmd_wifi; legacy alias
/// over the cfg registry, so cfg::set publishes the change for the wifi
/// watchdog).
async fn cmd_wifi<S: AtSink>(args: &str, out: &mut S) {
    match args.split_once(',') {
        Some(("ssid", v)) | Some(("pass", v)) => {
            let key = if args.starts_with("ssid") {
                "wifi.ssid"
            } else {
                "wifi.pass"
            };
            match cfg::set(key, v).await {
                Ok(()) => out.emit("OK").await,
                Err(_) => out.emit("ERROR bad value").await,
            }
        }
        _ if args == "status" => {
            let ssid = cfg::get_str("wifi.ssid").await;
            let mut line: String<80> = String::new();
            let _ = write!(line, "+WIFI:{ssid}"); // arduino.ino:2938 format
            out.emit(line.as_str()).await;
            out.emit("OK").await;
        }
        _ => out.emit("ERROR bad args").await,
    }
}

/// AT+MQTT=connect|disconnect|status|enable,<0|1>|auto,<0|1>
/// (Zephyr cmd_mqtt; enable/auto are cfg aliases so the change pubsub
/// reaches the mqtt task).
async fn cmd_mqtt<S: AtSink>(args: &str, scratch: &mut Scratch, out: &mut S) {
    if !crate::mqttc::enabled() {
        out.emit("ERROR mqtt disabled").await;
        return;
    }
    let mut f = [""; 2];
    let n = split(args, &mut f);
    let (sub, val) = (f[0], if n == 2 { f[1] } else { "" });

    if sub == "connect" {
        match crate::mqttc::start().await {
            Ok(()) => out.emit("OK").await,
            Err(()) => out.emit("ERROR broker unset").await,
        }
    } else if sub == "disconnect" {
        crate::mqttc::stop();
        out.emit("OK").await;
    } else if sub == "status" {
        let (connected, _running) = crate::mqttc::status();
        let auto = cfg::get_str("mqtt.auto").await;
        let enable = cfg::get_str("mqtt.enable").await;
        scratch.buf.clear();
        let _ = write!(
            scratch.buf,
            "+MQTT:{},auto={},enabled={}",
            if connected {
                "connected"
            } else {
                "disconnected"
            },
            auto,
            enable
        );
        out.emit(scratch.buf.as_str()).await;
        out.emit("OK").await;
    } else if n == 2 && (sub == "enable" || sub == "auto") {
        let key = if sub == "enable" {
            "mqtt.enable"
        } else {
            "mqtt.auto"
        };
        match cfg::set(key, val).await {
            Ok(()) => out.emit("OK").await,
            Err(_) => out.emit("ERROR bad value").await,
        }
    } else {
        out.emit("ERROR bad args").await;
    }
}

/// AT+HTTP[=status|enable,<0|1>|auto,<0|1>] (Zephyr cmd_http).
async fn cmd_http<S: AtSink>(args: &str, scratch: &mut Scratch, out: &mut S) {
    let mut f = [""; 2];
    let n = split(args, &mut f);
    let (sub, val) = (f[0], if n == 2 { f[1] } else { "" });

    if sub == "status" {
        let enabled = crate::httpd::running();
        let auto = cfg::get_str("http.auto").await;
        scratch.buf.clear();
        let _ = write!(
            scratch.buf,
            "+HTTP:{},auto={}",
            if enabled { "enabled" } else { "disabled" },
            auto
        );
        out.emit(scratch.buf.as_str()).await;
        out.emit("OK").await;
    } else if n == 2 && (sub == "enable" || sub == "auto") {
        if !crate::httpd::enabled() {
            out.emit("ERROR http disabled").await;
            return;
        }
        let key = if sub == "enable" {
            "http.enable"
        } else {
            "http.auto"
        };
        match cfg::set(key, val).await {
            Ok(()) => out.emit("OK").await,
            Err(_) => out.emit("ERROR bad value").await,
        }
    } else {
        out.emit("ERROR bad args").await;
    }
}

/// AT+STATUS (Zephyr cmd_status field order). ble/usb/mqtt/http report
/// "off" truthfully until their stages land (R3-R6).
async fn cmd_status<S: AtSink>(scratch: &mut Scratch, out: &mut S) {
    let name = cfg::get_str("device.name").await;
    let mac = esp_hal::efuse::base_mac_address();
    let rssi = crate::wifi::rssi();

    scratch.buf.clear();
    let _ = write!(scratch.buf, "device={name}");
    out.emit(scratch.buf.as_str()).await;
    out.emit("ble=off").await;
    out.emit("usb=off").await;
    out.emit(if crate::wifi::link_up() {
        "wifi=up"
    } else {
        "wifi=down"
    })
    .await;

    scratch.buf.clear();
    match crate::wifi::ipv4() {
        Some(ip) => {
            let _ = write!(scratch.buf, "ip={ip}");
            out.emit(scratch.buf.as_str()).await;
        }
        None => out.emit("ip=-").await,
    }

    scratch.buf.clear();
    let m = mac.as_bytes();
    let _ = write!(
        scratch.buf,
        "mac={:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
        m[0], m[1], m[2], m[3], m[4], m[5]
    );
    out.emit(scratch.buf.as_str()).await;

    scratch.buf.clear();
    let _ = write!(scratch.buf, "rssi={rssi}");
    out.emit(scratch.buf.as_str()).await;

    scratch.buf.clear();
    let _ = write!(scratch.buf, "mqtt={}", crate::mqttc::status_str());
    out.emit(scratch.buf.as_str()).await;
    out.emit(if crate::httpd::running() {
        "http=on"
    } else {
        "http=off"
    })
    .await;
    out.emit("OK").await;
}

/// AT+LED=<r>,<g>,<b>|#RRGGBB|off|auto — parse shared with HTTP/MQTT.
async fn cmd_led<S: AtSink>(args: &str, out: &mut S) {
    if !led::enabled() {
        out.emit("ERROR led disabled").await;
        return;
    }
    match led::parse(args) {
        Some(a) => {
            led::apply_action(a);
            out.emit("OK").await;
        }
        None => out.emit("ERROR bad args").await,
    }
}

/// AT+TAP=<key>[,<mods>][,<ms>] — atomic press+release (F18 discipline).
async fn cmd_tap<S: AtSink>(args: &str, out: &mut S) {
    if !crate::kb::enabled() {
        out.emit("ERROR kbd disabled").await;
        return;
    }
    let mut f = [""; 3];
    let n = split(args, &mut f);
    if n == 0 {
        out.emit("ERROR bad args").await;
        return;
    }
    let key = to_u32(f[0]);
    let mods = if n > 1 { to_u32(f[1]) } else { Some(0) };
    let ms = if n > 2 { to_u32(f[2]) } else { Some(50) };
    match (key, mods, ms) {
        (Some(k), Some(m), Some(ms)) if k <= 255 && m <= 255 && ms <= 60_000 => {
            if crate::kb::tap(m as u8, k as u8, ms) {
                out.emit("OK").await;
            } else {
                out.emit("ERROR busy").await;
            }
        }
        _ => out.emit("ERROR bad args").await,
    }
}

/// AT+KEY=<mods>,<k0>[,<k1>..<k5>] — bare report; holds until AT+KEY=0,0.
async fn cmd_key<S: AtSink>(args: &str, out: &mut S) {
    if !crate::kb::enabled() {
        out.emit("ERROR kbd disabled").await;
        return;
    }
    let mut f = [""; 7];
    let n = split(args, &mut f);
    if n < 2 {
        out.emit("ERROR bad args").await;
        return;
    }
    let Some(mods) = to_u32(f[0]) else {
        out.emit("ERROR bad args").await;
        return;
    };
    let mut r = crate::kb::Report {
        mods: mods as u8,
        ..Default::default()
    };
    for (i, k) in f[1..n].iter().enumerate() {
        match to_u32(k) {
            Some(v) if v <= 255 => r.keys[i] = v as u8,
            _ => {
                out.emit("ERROR bad args").await;
                return;
            }
        }
    }
    if crate::kb::hold(r) {
        out.emit("OK").await;
    } else {
        out.emit("ERROR busy").await;
    }
}

/// AT+KEY_STR=<text> — ASCII playback, auto-paired press/release.
async fn cmd_key_str<S: AtSink>(args: &str, out: &mut S) {
    if !crate::kb::enabled() {
        out.emit("ERROR kbd disabled").await;
        return;
    }
    if args.is_empty() {
        out.emit("ERROR bad args").await;
        return;
    }
    let mut s: heapless::String<192> = heapless::String::new();
    if s.push_str(args).is_err() {
        out.emit("ERROR too long").await;
        return;
    }
    if crate::kb::text(s, 40, 40) {
        out.emit("OK").await;
    } else {
        out.emit("ERROR busy").await;
    }
}

/// AT+KEY_SEQ=<ms>,<mods>,<k0..k5>,... — batch playback, <=8 reports.
async fn cmd_key_seq<S: AtSink>(args: &str, out: &mut S) {
    if !crate::kb::enabled() {
        out.emit("ERROR kbd disabled").await;
        return;
    }
    // >8 reports must error, not silently truncate (CH582 SEQ_MAX_REPORTS).
    if args.matches(',').count() + 1 > 57 {
        out.emit("ERROR too many reports").await;
        return;
    }
    let mut f = [""; 57];
    let n = split(args, &mut f);
    if n < 8 || (n - 1) % 7 != 0 {
        out.emit("ERROR bad args").await;
        return;
    }
    let Some(delay) = to_u32(f[0]) else {
        out.emit("ERROR bad args").await;
        return;
    };
    let mut reports: heapless::Vec<crate::kb::Report, { crate::kb::SEQ_MAX_REPORTS }> =
        heapless::Vec::new();
    for chunk in f[1..n].chunks_exact(7) {
        let Some(mods) = to_u32(chunk[0]).filter(|m| *m <= 255) else {
            out.emit("ERROR bad args").await;
            return;
        };
        let mut r = crate::kb::Report {
            mods: mods as u8,
            ..Default::default()
        };
        for (i, k) in chunk[1..].iter().enumerate() {
            match to_u32(k) {
                Some(v) if v <= 255 => r.keys[i] = v as u8,
                _ => {
                    out.emit("ERROR bad args").await;
                    return;
                }
            }
        }
        if reports.push(r).is_err() {
            out.emit("ERROR bad args").await;
            return;
        }
    }
    if crate::kb::seq(reports, delay) {
        out.emit("OK").await;
    } else {
        out.emit("ERROR busy").await;
    }
}

/// AT+TUNNEL=status|enable[,<0|1>]|<field>,<val>|connect|disconnect|clear
/// (Arduino AT+TUNNEL semantics; fields alias the tunnel.1.* registry keys,
/// enable,<v> is the rathole master switch).
async fn cmd_tunnel<S: AtSink>(args: &str, scratch: &mut Scratch, out: &mut S) {
    if !crate::rathole::enabled() {
        out.emit("ERROR tunnel disabled").await;
        return;
    }
    if args == "status" || args == "enable" {
        let master = crate::cfg::get_str("rathole.enable").await;
        scratch.buf.clear();
        let _ = write!(scratch.buf, "+TUNNEL_EN:{master}");
        out.emit(scratch.buf.as_str()).await;
        if args == "status" {
            let j = crate::rathole::status_json().await;
            scratch.buf.clear();
            let _ = write!(scratch.buf, "+TUNNEL:{j}");
            out.emit(scratch.buf.as_str()).await;
        }
        out.emit("OK").await;
        return;
    }
    if let Some(v) = args.strip_prefix("enable,") {
        match crate::cfg::set("rathole.enable", v).await {
            Ok(()) => out.emit("OK").await,
            Err(_) => out.emit("ERROR bad value").await,
        }
        return;
    }
    let (sub, val) = args.split_once(',').unwrap_or((args, ""));
    let key = match sub {
        "server" => "tunnel.1.server",
        "token" => "tunnel.1.token",
        "service" => "tunnel.1.service",
        "local" => "tunnel.1.local",
        "auto" => "tunnel.1.auto",
        "retry" => "tunnel.1.retry",
        _ => "",
    };
    if !key.is_empty() {
        if val.is_empty() {
            out.emit("ERROR bad args").await;
            return;
        }
        match crate::cfg::set(key, val).await {
            Ok(()) => out.emit("OK").await,
            Err(_) => out.emit("ERROR bad value").await,
        }
        return;
    }
    match sub {
        "connect" => {
            if crate::rathole::connect().await {
                out.emit("OK").await;
            } else {
                out.emit("ERROR start failed").await;
            }
        }
        "disconnect" => {
            crate::rathole::disconnect();
            out.emit("OK").await;
        }
        "clear" => {
            crate::rathole::clear().await;
            out.emit("OK").await;
        }
        _ => out.emit("ERROR bad args").await,
    }
}
/// AT+BLE=status|bonds|clear (R6.3; mirrors the CH582/Arduino BLE surface).
async fn cmd_ble<S: AtSink>(sub: &str, scratch: &mut Scratch, out: &mut S) {
    #[cfg(not(feature = "kbd-ble"))]
    {
        let _ = (sub, scratch);
        out.emit("ERROR ble disabled").await;
    }
    #[cfg(feature = "kbd-ble")]
    match sub {
        "status" => {
            scratch.buf.clear();
            let _ = write!(
                scratch.buf,
                "+BLE:conn={},window={}",
                crate::kbd_ble::connected() as u8,
                if crate::kbd_ble::pair_window_open() {
                    "open"
                } else {
                    "closed"
                }
            );
            out.emit(scratch.buf.as_str()).await;
            if let Some(addr) = crate::kbd_ble::bond_addr() {
                scratch.buf.clear();
                let _ = write!(
                    scratch.buf,
                    "+BLE:bond={:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
                    addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]
                );
                out.emit(scratch.buf.as_str()).await;
            } else {
                out.emit("+BLE:bond=none").await;
            }
            out.emit("OK").await;
        }
        "bonds" => {
            if let Some(addr) = crate::kbd_ble::bond_addr() {
                scratch.buf.clear();
                let _ = write!(
                    scratch.buf,
                    "+BONDS:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
                    addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]
                );
                out.emit(scratch.buf.as_str()).await;
            } else {
                out.emit("+BONDS:none").await;
            }
            out.emit("OK").await;
        }
        "clear" => {
            crate::kbd_ble::clear_bond().await;
            out.emit("OK").await;
        }
        _ => out.emit("ERROR bad args").await,
    }
}
