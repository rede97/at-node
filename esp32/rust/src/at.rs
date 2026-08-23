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
    "  AT+LED=<r>,<g>,<b>|#RRGGBB|off|auto / AT+LED?",
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
    match led::parse(args) {
        Some(a) => {
            led::apply_action(a);
            out.emit("OK").await;
        }
        None => out.emit("ERROR bad args").await,
    }
}
