//! AT-Node rust-s3 — shared API surface: ability flags, the services
//! catalog (help.json / MQTT sys/info), and the sys info manifest.
//!
//! Semantics aligned with esp32/arduino/arduino.ino (build_ability_json,
//! API_CATALOG/build_services_json, build_sys_info_json). Entries appear
//! as their stage lands: keyboard/* + ble/* in R5/R6, net/* evaluated
//! separately, ir/tunnel are non-goals.

use core::fmt::Write as _;

use heapless::String;

use crate::{cfg, mqttc, wifi};

/// Arduino build_ability_json(). ble flips true in R6, http in R4.
pub fn ability_json() -> String<96> {
    let mut j: String<96> = String::new();
    let _ = j.push_str(
        "{\"ble\":false,\"mqtt\":true,\"rathole\":false,\"i2c\":true,\
\"http\":true,\"breath_led\":false}",
    );
    j
}

struct ApiParam(&'static str, &'static str);
struct ApiEntry {
    method: &'static str,
    params: &'static [ApiParam],
    desc: &'static str,
}

const P_GW: &[ApiParam] = &[
    ApiParam("pin", "GPIO number (safe set, see docs)"),
    ApiParam("level", "0=LOW 1=HIGH"),
];
const P_GR: &[ApiParam] = &[ApiParam("pin", "GPIO number (input pullup)")];
const P_ADC: &[ApiParam] = &[ApiParam("ch", "ADC1 channel 0-9 (GPIO1-10)")];
const P_I2CR: &[ApiParam] = &[
    ApiParam("addr", "I2C device address (hex ok)"),
    ApiParam("reg", "register address"),
    ApiParam("len", "bytes to read, default 1"),
];
const P_I2CW: &[ApiParam] = &[
    ApiParam("addr", "I2C device address (hex ok)"),
    ApiParam("reg", "register address"),
    ApiParam("data", "hex bytes to write (e.g. FF01)"),
];
const P_CFGSET: &[ApiParam] = &[
    ApiParam("key", "config key (see config/list)"),
    ApiParam("val", "value"),
];
const P_CFGGET: &[ApiParam] = &[ApiParam("key", "config key")];

/// Arduino API_CATALOG restricted to what this firmware implements.
const CATALOG: &[ApiEntry] = &[
    ApiEntry {
        method: "gpio/write",
        params: P_GW,
        desc: "set GPIO output level",
    },
    ApiEntry {
        method: "gpio/read",
        params: P_GR,
        desc: "read GPIO input (pullup)",
    },
    ApiEntry {
        method: "adc/read",
        params: P_ADC,
        desc: "read ADC millivolts",
    },
    ApiEntry {
        method: "i2c/scan",
        params: &[],
        desc: "scan I2C bus for devices",
    },
    ApiEntry {
        method: "i2c/read",
        params: P_I2CR,
        desc: "read I2C register",
    },
    ApiEntry {
        method: "i2c/write",
        params: P_I2CW,
        desc: "write I2C register",
    },
    ApiEntry {
        method: "ability",
        params: &[],
        desc: "compile-time feature flags",
    },
    ApiEntry {
        method: "config/set",
        params: P_CFGSET,
        desc: "unified config: set key=val (NVS)",
    },
    ApiEntry {
        method: "config/get",
        params: P_CFGGET,
        desc: "unified config: read key",
    },
    ApiEntry {
        method: "config/list",
        params: &[],
        desc: "unified config: list all keys",
    },
    ApiEntry {
        method: "sys/info",
        params: &[],
        desc: "device manifest + this API catalog",
    },
];

/// Arduino build_services_json().
pub fn services_json() -> String<1600> {
    let mut s: String<1600> = String::new();
    let _ = s.push('{');
    for (i, e) in CATALOG.iter().enumerate() {
        if i > 0 {
            let _ = s.push(',');
        }
        let _ = write!(s, "\"{}\":{{\"d\":\"{}\",\"p\":{{", e.method, e.desc);
        for (j, p) in e.params.iter().enumerate() {
            if j > 0 {
                let _ = s.push(',');
            }
            let _ = write!(s, "\"{}\":\"{}\"", p.0, p.1);
        }
        let _ = s.push_str("}}");
    }
    let _ = s.push('}');
    s
}

/// Arduino build_sys_info_json(): retained MQTT info manifest, also the
/// sys/info RPC result. ble fields stay false/empty until R6.
pub async fn sys_info_json() -> String<1600> {
    let name = cfg::get_str("device.name").await;
    let hostname = cfg::get_str("device.hostname").await;
    let mut ip_s: String<16> = String::new();
    if let Some(ip) = wifi::ipv4() {
        let _ = write!(ip_s, "{ip}");
    }
    let (_, mqtt_connected) = mqttc::status();
    let services = services_json();

    let mut j: String<1600> = String::new();
    let _ = write!(
        j,
        "{{\"device\":\"{name}\",\"hostname\":\"{hostname}\",\"ip\":\"{ip_s}\",\
\"ble_addr\":\"\",\"ble_connected\":false,\"typing\":false,\
\"mqtt\":{mqtt_connected},\"services\":{services}}}"
    );
    j
}

/// URL-decode helper (Arduino url_decode): %XX and '+' -> space.
pub fn url_decode(input: &str, out: &mut String<256>) {
    out.clear();
    let b = input.as_bytes();
    let mut i = 0;
    while i < b.len() {
        match b[i] {
            b'%' if i + 2 < b.len() => {
                match u8::from_str_radix(&input[i + 1..i + 3], 16) {
                    Ok(v) => {
                        let _ = out.push(v as char);
                        i += 3;
                    }
                    Err(_) => {
                        let _ = out.push('%');
                        i += 1;
                    }
                }
            }
            b'+' => {
                let _ = out.push(' ');
                i += 1;
            }
            c => {
                let _ = out.push(c as char);
                i += 1;
            }
        }
    }
}

/// Arduino query_get(): url-decoded value of `key` in a "k=v&k2=v2"
/// query string, written into the caller's scratch buffer.
pub fn query_get<'a>(query: &str, key: &str, out: &'a mut String<256>) -> &'a str {
    out.clear();
    for pair in query.split('&') {
        if let Some((k, v)) = pair.split_once('=')
            && k == key
        {
            url_decode(v, out);
            return out.as_str();
        }
    }
    ""
}
