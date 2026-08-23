//! AT-Node rust-s3 — HTTP control plane (picoserve).
//!
//! Route table and JSON formats aligned with esp32/arduino/arduino.ino
//! handlers (NOT the Zephyr variant). The SPA ships unmodified: `/` serves
//! the shared gzip bytes extracted from esp32/arduino/web_page.h by
//! build.rs; legacy page URLs 302 to `/`.
//!
//! Runtime gate: http.enable (master) + http.auto (boot start), driven by
//! the cfg pubsub like mqtt. Disabling drops the server future, which
//! closes the listener socket; enabling re-accepts.
//!
//! Buffers: one server task, 3 KiB http buffer + 2/2 KiB tcp buffers.
//! Response bodies use alloc Strings — request rate is low and bounded;
//! the wire path itself stays allocation-free (MIGRATION 5.3 note).
//!
//! Deviation: unknown paths return picoserve's default empty 404 (Arduino
//! returns a JSON body); the SPA never calls unknown paths.

use core::cell::RefCell;
use core::fmt::Write as _;

use alloc::string::String;
use embassy_net::Stack;
use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::signal::Signal;
use embassy_time::Duration;
use log::warn;
use picoserve::response::{IntoResponse, Response, StatusCode};
use picoserve::routing::{PathRouter, get, post};
use picoserve::{Config, Timeouts};

use crate::{api, at, cfg, hws, led, mqttc, rathole, wifi};

const PORT: u16 = 80;

static SPA_GZ: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/web_page.gz"));

// ------------------------------------------------------------- runtime ---

/// Compile-time capability flag (feature matrix in Cargo.toml).
pub fn enabled() -> bool {
    cfg!(feature = "http")
}

static STATE: critical_section::Mutex<RefCell<bool>> =
    critical_section::Mutex::new(RefCell::new(false));
static KICK: Signal<CriticalSectionRawMutex, ()> = Signal::new();

fn set_running(v: bool) {
    critical_section::with(|cs| *STATE.borrow(cs).borrow_mut() = v);
    KICK.signal(());
}

/// http= field for AT+STATUS / AT+HTTP=status.
pub fn running() -> bool {
    critical_section::with(|cs| *STATE.borrow(cs).borrow())
}

// ------------------------------------------------------------- helpers ---

/// Collect AT output lines for /at-node/at ("\n" joined).
struct CollectSink {
    buf: String,
}

impl at::AtSink for CollectSink {
    async fn emit(&mut self, line: &str) {
        if !self.buf.is_empty() {
            self.buf.push('\n');
        }
        self.buf.push_str(line);
    }
}

fn json_escape(input: &str, out: &mut String) {
    for c in input.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => {}
            c => out.push(c),
        }
    }
}

/// Content with an application/json type (Content for String would emit
/// text/plain first, and strict fetch() callers then skip JSON parsing).
struct JsonStr(String);

impl picoserve::response::Content for JsonStr {
    fn content_type(&self) -> &'static str {
        "application/json"
    }

    fn content_length(&self) -> usize {
        self.0.len()
    }

    async fn write_content<W: picoserve::io::Write>(self, mut writer: W) -> Result<(), W::Error> {
        writer.write_all(self.0.as_bytes()).await
    }
}

/// JSON response with CORS (Arduino send_json). Single constructor so
/// every handler branch yields the same concrete response type.
fn json_response(code: StatusCode, body: String) -> impl IntoResponse {
    Response::new(code, JsonStr(body)).with_header("Access-Control-Allow-Origin", "*")
}

fn err_body(msg: &str) -> String {
    let mut s = String::new();
    let _ = write!(s, "{{\"ok\":false,\"error\":\"{msg}\"}}");
    s
}

/// Owned query-string extractor (FromRequestParts has no built-in for
/// plain string access; owned to keep handler lifetimes simple).
struct QueryString(String);

impl<'r, State> picoserve::extract::FromRequestParts<'r, State> for QueryString {
    type Rejection = core::convert::Infallible;

    async fn from_request_parts(
        _state: &'r State,
        request_parts: &picoserve::request::RequestParts<'r>,
    ) -> Result<Self, Self::Rejection> {
        let mut s = String::new();
        if let Some(q) = request_parts.query() {
            s.push_str(q.0);
        }
        Ok(QueryString(s))
    }
}

/// Request args: url-encoded body first, then URL query (Arduino arg()
/// checks POST fields then query).
struct Args<'a> {
    body: &'a str,
    query: &'a str,
}

impl<'a> Args<'a> {
    fn get(&self, key: &str, out: &'a mut heapless::String<256>) -> &'a str {
        if self.has_in(self.body, key) {
            return api::query_get(self.body, key, out);
        }
        api::query_get(self.query, key, out)
    }

    fn has_in(&self, haystack: &str, key: &str) -> bool {
        let mut needle = String::new();
        let _ = write!(needle, "{key}=");
        haystack.contains(needle.as_str())
    }

    fn has(&self, key: &str) -> bool {
        self.has_in(self.body, key) || self.has_in(self.query, key)
    }

    fn to_u32(&self, key: &str) -> Option<u32> {
        let mut buf = heapless::String::<256>::new();
        let v = self.get(key, &mut buf);
        if let Some(h) = v.strip_prefix("0x").or_else(|| v.strip_prefix("0X")) {
            u32::from_str_radix(h, 16).ok()
        } else {
            v.parse().ok()
        }
    }
}

fn args<'a>(query: &'a QueryString, body: &'a str) -> Args<'a> {
    Args {
        body,
        query: query.0.as_str(),
    }
}

// -------------------------------------------------------------- router ---

pub fn build_app() -> picoserve::Router<impl PathRouter<()>, ()> {
    picoserve::Router::new()
        .route(
            "/",
            // File keeps ONE Content-Type (Content for &[u8] would add
            // application/octet-stream alongside ours).
            picoserve::routing::get_service(
                picoserve::response::File::with_content_type_and_headers(
                    "text/html",
                    SPA_GZ,
                    &[
                        ("Content-Encoding", "gzip"),
                        ("Cache-Control", "no-cache"),
                        ("Access-Control-Allow-Origin", "*"),
                    ],
                ),
            ),
        )
        .route("/at-node/status", get(|| async { Response::new(StatusCode::FOUND, "")
                    .with_header("Location", "/") }))
        .route("/at-node/help", get(|| async { Response::new(StatusCode::FOUND, "")
                    .with_header("Location", "/") }))
        .route("/at-node/pair", get(|| async { Response::new(StatusCode::FOUND, "")
                    .with_header("Location", "/") }))
        .route("/at-node/tunnel", get(|| async { Response::new(StatusCode::FOUND, "")
                    .with_header("Location", "/") }))
        .route("/at-node/mqtt", get(|| async { Response::new(StatusCode::FOUND, "")
                    .with_header("Location", "/") }))
        .route("/at-node/cmd/status", get(h_cmd_status))
        .route("/at-node/cmd/ability", get(h_ability))
        .route("/at-node/help.json", get(h_help_json))
        .route("/at-node/at", post(h_at))
        .route("/at-node/cmd/gpio/write", post(h_gpio_write))
        .route("/at-node/cmd/gpio/read", post(h_gpio_read))
        .route("/at-node/cmd/adc/read", post(h_adc_read))
        .route("/at-node/cmd/i2c/scan", post(h_i2c_scan))
        .route("/at-node/cmd/i2c/read", post(h_i2c_read))
        .route("/at-node/cmd/i2c/write", post(h_i2c_write))
        .route("/at-node/cmd/led", get(h_led_status).post(h_led_set))
        .route("/at-node/cmd/mqtt/status", get(h_mqtt_status))
        .route("/at-node/cmd/mqtt/config", post(h_mqtt_config))
        .route("/at-node/cmd/mqtt/connect", post(h_mqtt_connect))
        .route("/at-node/cmd/mqtt/clear", post(h_mqtt_clear))
        .route("/at-node/cmd/mqtt/publish", post(h_mqtt_publish))
        .route("/at-node/cmd/mqtt/subscribe", post(h_mqtt_subscribe))
        .route("/at-node/cmd/config", get(h_config_get).post(h_config_set))
        .route("/at-node/cmd/config/list", get(h_config_list))
        .route("/at-node/cmd/wifi/config", post(h_wifi_config))
        .route("/at-node/cmd/http/status", get(h_http_status))
        .route("/at-node/cmd/http/config", post(h_http_config))
        .route("/at-node/cmd/http/clear", post(h_http_clear))
        .route("/at-node/cmd/nvs/clear", post(h_nvs_clear))
        // ONE route for all tunnel ops: every picoserve route adds router
        // type depth (and native-stack poll depth per request); six tunnel
        // routes overflowed the executor stack on config POST.
        .route(
            (
                "/at-node/cmd/tunnel",
                picoserve::routing::parse_path_segment::<TunnelOp>(),
            ),
            get(h_tunnel_get).post(h_tunnel_post),
        )
        // Keyboard injection (same consolidation; broker method shapes).
        .route(
            (
                "/at-node/cmd/keyboard",
                picoserve::routing::parse_path_segment::<KbOp>(),
            ),
            picoserve::routing::post(h_keyboard_post),
        )
}

// ----------------------------------------------------------- handlers ---

async fn h_cmd_status() -> impl IntoResponse {
    let name = cfg::get_str("device.name").await;
    let hostname = cfg::get_str("device.hostname").await;
    let mut ip_s: heapless::String<16> = heapless::String::new();
    if let Some(ip) = wifi::ipv4() {
        let _ = write!(ip_s, "{ip}");
    }
    let (_, mqtt_connected) = mqttc::status();
    let rssi = wifi::rssi();
    let pct = (2 * (rssi + 100)).clamp(0, 100);
    let mut j = String::new();
    let _ = write!(
        j,
        "{{\"device\":\"{name}\",\"hostname\":\"{hostname}\",\"connected\":false,\
\"ip\":\"{ip_s}\",\"ble_addr\":\"\",\"typing\":false,\"mqtt\":{mqtt_connected},\
\"ap\":false,\"http_enabled\":{http_enabled},\"heap\":{heap},\
\"wifi_rssi\":{rssi},\"wifi_pct\":{pct},\"ability\":{ability}}}",
        http_enabled = running(),
        heap = esp_alloc::HEAP.free(),
        ability = api::ability_json(),
    );
    json_response(StatusCode::OK, j)
}

async fn h_ability() -> impl IntoResponse {
    let mut j = String::new();
    let _ = write!(j, "{{\"ok\":true,\"ability\":{}}}", api::ability_json());
    json_response(StatusCode::OK, j)
}

async fn h_help_json() -> impl IntoResponse {
    let mut j = String::new();
    let _ = write!(j, "{{\"ok\":true,\"services\":{}}}", api::services_json());
    json_response(StatusCode::OK, j)
}

async fn h_at(body: String) -> impl IntoResponse {
    let line = body.trim();
    if line.is_empty() {
        return json_response(StatusCode::BAD_REQUEST, err_body("empty command"));
    }
    let mut sink = CollectSink { buf: String::new() };
    at::handle_line(line, &mut sink).await;
    let mut esc = String::new();
    json_escape(sink.buf.as_str(), &mut esc);
    let mut j = String::new();
    let _ = write!(j, "{{\"ok\":true,\"response\":\"{esc}\"}}");
    json_response(StatusCode::OK, j)
}

async fn h_gpio_write(query: QueryString, body: String) -> impl IntoResponse {
    if !hws::enabled() {
        return json_response(StatusCode::BAD_REQUEST, err_body("hws disabled"));
    }
    let a = args(&query, body.as_str());
    match (a.to_u32("pin"), a.to_u32("level")) {
        (Some(pin), Some(level)) if pin <= 255 && hws::gpio_write(pin as u8, level != 0).is_ok() => {
            let mut j = String::new();
            let _ = write!(
                j,
                "{{\"ok\":true,\"cmd\":\"gpio/write\",\"pin\":{pin},\"level\":{level}}}"
            );
            json_response(StatusCode::OK, j)
        }
        _ => json_response(StatusCode::BAD_REQUEST, err_body("invalid pin")),
    }
}

async fn h_gpio_read(query: QueryString, body: String) -> impl IntoResponse {
    if !hws::enabled() {
        return json_response(StatusCode::BAD_REQUEST, err_body("hws disabled"));
    }
    let a = args(&query, body.as_str());
    match a.to_u32("pin") {
        Some(pin) if pin <= 255 => match hws::gpio_read(pin as u8) {
            Ok(level) => {
                let mut j = String::new();
                let _ = write!(
                    j,
                    "{{\"ok\":true,\"cmd\":\"gpio/read\",\"pin\":{pin},\"level\":{level}}}"
                );
                json_response(StatusCode::OK, j)
            }
            Err(()) => json_response(StatusCode::BAD_REQUEST, err_body("invalid pin")),
        },
        _ => json_response(StatusCode::BAD_REQUEST, err_body("invalid pin")),
    }
}

async fn h_adc_read(query: QueryString, body: String) -> impl IntoResponse {
    if !hws::enabled() {
        return json_response(StatusCode::BAD_REQUEST, err_body("hws disabled"));
    }
    let a = args(&query, body.as_str());
    match a.to_u32("ch") {
        Some(ch) if ch <= 255 => match hws::adc_read_mv(ch as u8) {
            Ok(mv) => {
                let mut j = String::new();
                let _ = write!(j, "{{\"ok\":true,\"cmd\":\"adc/read\",\"ch\":{ch},\"mv\":{mv}}}");
                json_response(StatusCode::OK, j)
            }
            Err(()) => json_response(StatusCode::BAD_REQUEST, err_body("invalid adc ch")),
        },
        _ => json_response(StatusCode::BAD_REQUEST, err_body("invalid adc ch")),
    }
}

/// Current LED state for the web picker / status polls.
async fn h_led_status() -> impl IntoResponse {
    let (r, g, b, mode) = led::current();
    let mut j = String::new();
    let _ = write!(
        j,
        "{{\"ok\":true,\"r\":{r},\"g\":{g},\"b\":{b},\"mode\":\"{mode}\",\
\"hex\":\"#{r:02x}{g:02x}{b:02x}\"}}"
    );
    json_response(StatusCode::OK, j)
}

/// Set color: color=#RRGGBB | r,g,b | off | auto (AT+LED semantics).
async fn h_led_set(query: QueryString, body: String) -> impl IntoResponse {
    if !led::enabled() {
        return json_response(StatusCode::BAD_REQUEST, err_body("led disabled"));
    }
    let a = args(&query, body.as_str());
    let mut buf = heapless::String::<256>::new();
    let color = a.get("color", &mut buf);
    match led::parse(color) {
        Some(act) => {
            led::apply_action(act);
            let mut j = String::new();
            let _ = write!(j, "{{\"ok\":true,\"cmd\":\"led\",\"color\":\"{color}\"}}");
            json_response(StatusCode::OK, j)
        }
        None => json_response(StatusCode::BAD_REQUEST, err_body("bad color")),
    }
}

async fn h_i2c_scan() -> impl IntoResponse {
    if !hws::enabled() {
        return json_response(StatusCode::BAD_REQUEST, err_body("hws disabled"));
    }
    let mut scan: heapless::String<600> = heapless::String::new();
    hws::i2c_scan(&mut scan).await;
    // "+I2C: 0xXX 0xYY" / "+I2C: none" -> Arduino json device list
    let mut j = String::new();
    j.push_str("{\"ok\":true,\"cmd\":\"i2c/scan\",\"devices\":[");
    let list = scan.as_str().trim_start_matches("+I2C:").trim();
    if list != "none" {
        let mut first = true;
        for dev in list.split(' ').filter(|d| !d.is_empty()) {
            if !first {
                j.push(',');
            }
            let _ = write!(j, "\"{}\"", dev.to_lowercase());
            first = false;
        }
    }
    j.push_str("]}");
    json_response(StatusCode::OK, j)
}

async fn h_i2c_read(query: QueryString, body: String) -> impl IntoResponse {
    if !hws::enabled() {
        return json_response(StatusCode::BAD_REQUEST, err_body("hws disabled"));
    }
    let a = args(&query, body.as_str());
    let len = a.to_u32("len").unwrap_or(1);
    if len == 0 || len > hws::I2C_IO_MAX as u32 {
        return json_response(StatusCode::BAD_REQUEST, err_body("len must be 1-32"));
    }
    let (Some(addr), Some(reg)) = (a.to_u32("addr"), a.to_u32("reg")) else {
        return json_response(StatusCode::BAD_REQUEST, err_body("missing addr/reg"));
    };
    let mut data = [0u8; hws::I2C_IO_MAX];
    if hws::i2c_read(addr as u8, reg, &mut data[..len as usize])
        .await
        .is_err()
    {
        return json_response(StatusCode::INTERNAL_SERVER_ERROR, err_body("i2c no ack"));
    }
    let mut j = String::new();
    let _ = write!(
        j,
        "{{\"ok\":true,\"cmd\":\"i2c/read\",\"addr\":\"0x{addr:x}\",\"reg\":\"0x{reg:x}\",\"data\":\""
    );
    for (i, b) in data[..len as usize].iter().enumerate() {
        if i > 0 {
            j.push(' ');
        }
        let _ = write!(j, "{b:02X}");
    }
    j.push_str("\"}");
    json_response(StatusCode::OK, j)
}

async fn h_i2c_write(query: QueryString, body: String) -> impl IntoResponse {
    if !hws::enabled() {
        return json_response(StatusCode::BAD_REQUEST, err_body("hws disabled"));
    }
    let a = args(&query, body.as_str());
    let mut hexbuf = heapless::String::<256>::new();
    let hex: String = a
        .get("data", &mut hexbuf)
        .chars()
        .filter(|c| *c != ' ')
        .collect();
    if hex.is_empty() || !hex.len().is_multiple_of(2) || hex.len() / 2 > hws::I2C_IO_MAX {
        return json_response(StatusCode::BAD_REQUEST, err_body("data must be hex pairs"));
    }
    let mut data = [0u8; hws::I2C_IO_MAX];
    let mut n = 0usize;
    for i in (0..hex.len()).step_by(2) {
        match u8::from_str_radix(&hex[i..i + 2], 16) {
            Ok(b) => {
                data[n] = b;
                n += 1;
            }
            Err(_) => return json_response(StatusCode::BAD_REQUEST, err_body("data must be hex pairs")),
        }
    }
    let (Some(addr), Some(reg)) = (a.to_u32("addr"), a.to_u32("reg")) else {
        return json_response(StatusCode::BAD_REQUEST, err_body("missing addr/reg"));
    };
    if hws::i2c_write(addr as u8, reg, &data[..n])
        .await
        .is_err()
    {
        return json_response(StatusCode::INTERNAL_SERVER_ERROR, err_body("i2c no ack"));
    }
    let mut j = String::new();
    let _ = write!(
        j,
        "{{\"ok\":true,\"cmd\":\"i2c/write\",\"addr\":\"0x{addr:x}\",\"reg\":\"0x{reg:x}\"}}"
    );
    json_response(StatusCode::OK, j)
}

async fn h_mqtt_status() -> impl IntoResponse {
    if !mqttc::enabled() {
        return json_response(StatusCode::BAD_REQUEST, err_body("mqtt disabled"));
    }
    let (connected, _) = mqttc::status();
    let broker = cfg::get_str("mqtt.broker").await;
    let port = cfg::get_str("mqtt.port").await;
    let hostname = cfg::get_str("device.hostname").await;
    let auto = cfg::get_str("mqtt.auto").await;
    let mut j = String::new();
    let _ = write!(
        j,
        "{{\"connected\":{connected},\"broker\":\"{broker}\",\"port\":{port},\
\"client_id\":\"atnode-{hostname}\",\"ca_fp\":\"\",\"auto\":{}}}",
        auto == "1"
    );
    json_response(StatusCode::OK, j)
}

async fn h_mqtt_config(query: QueryString, body: String) -> impl IntoResponse {
    if !mqttc::enabled() {
        return json_response(StatusCode::BAD_REQUEST, err_body("mqtt disabled"));
    }
    let a = args(&query, body.as_str());
    for (arg, key) in [
        ("broker", "mqtt.broker"),
        ("port", "mqtt.port"),
        ("user", "mqtt.user"),
        ("pass", "mqtt.pass"),
        ("auto", "mqtt.auto"),
    ] {
        if a.has(arg) {
            let mut buf = heapless::String::<256>::new();
            let _ = cfg::set(key, a.get(arg, &mut buf)).await;
        }
    }
    json_response(StatusCode::OK, "{\"ok\":true,\"cmd\":\"mqtt/config\"}".into())
}

async fn h_mqtt_connect() -> impl IntoResponse {
    if !mqttc::enabled() {
        return json_response(StatusCode::BAD_REQUEST, err_body("mqtt disabled"));
    }
    let _ = mqttc::start().await;
    json_response(StatusCode::OK, "{\"ok\":true,\"cmd\":\"mqtt/connect\",\"queued\":true}".into())
}

async fn h_mqtt_clear() -> impl IntoResponse {
    if !mqttc::enabled() {
        return json_response(StatusCode::BAD_REQUEST, err_body("mqtt disabled"));
    }
    mqttc::stop();
    for key in ["mqtt.broker", "mqtt.user", "mqtt.pass", "mqtt.auto"] {
        let _ = cfg::set(key, "").await;
    }
    let _ = cfg::set("mqtt.port", "8883").await;
    json_response(StatusCode::OK, "{\"ok\":true,\"cmd\":\"mqtt/clear\"}".into())
}

async fn h_mqtt_publish(query: QueryString, body: String) -> impl IntoResponse {
    if !mqttc::enabled() {
        return json_response(StatusCode::BAD_REQUEST, err_body("mqtt disabled"));
    }
    if !mqttc::status().0 {
        return json_response(StatusCode::CONFLICT, err_body("mqtt not connected"));
    }
    let a = args(&query, body.as_str());
    let mut tbuf = heapless::String::<256>::new();
    let topic = a.get("topic", &mut tbuf);
    if topic.is_empty() {
        return json_response(StatusCode::BAD_REQUEST, err_body("missing topic"));
    }
    let mut mbuf = heapless::String::<256>::new();
    let msg = a.get("msg", &mut mbuf);
    let ok = mqttc::publish(topic, msg).await;
    let mut j = String::new();
    let _ = write!(j, "{{\"ok\":{ok},\"cmd\":\"mqtt/publish\"}}");
    json_response(StatusCode::OK, j)
}

async fn h_mqtt_subscribe(query: QueryString, body: String) -> impl IntoResponse {
    if !mqttc::enabled() {
        return json_response(StatusCode::BAD_REQUEST, err_body("mqtt disabled"));
    }
    if !mqttc::status().0 {
        return json_response(StatusCode::CONFLICT, err_body("mqtt not connected"));
    }
    let a = args(&query, body.as_str());
    let mut tbuf = heapless::String::<256>::new();
    let topic = a.get("topic", &mut tbuf);
    if topic.is_empty() {
        return json_response(StatusCode::BAD_REQUEST, err_body("missing topic"));
    }
    let ok = mqttc::subscribe(topic).await;
    let mut j = String::new();
    let _ = write!(j, "{{\"ok\":{ok},\"cmd\":\"mqtt/subscribe\"}}");
    json_response(StatusCode::OK, j)
}

async fn h_config_get(query: QueryString) -> impl IntoResponse {
    let mut kbuf = heapless::String::<256>::new();
    let key = api::query_get(query.0.as_str(), "key", &mut kbuf);
    match cfg::get(key).await {
        Ok(val) => {
            let mut j = String::new();
            let _ = write!(j, "{{\"ok\":true,\"key\":\"{key}\",\"value\":\"{val}\"}}");
            json_response(StatusCode::OK, j)
        }
        Err(_) => json_response(StatusCode::BAD_REQUEST, err_body("unknown key")),
    }
}

async fn h_config_set(query: QueryString, body: String) -> impl IntoResponse {
    let a = args(&query, body.as_str());
    if !a.has("key") {
        return json_response(StatusCode::BAD_REQUEST, err_body("missing key"));
    }
    let mut kbuf = heapless::String::<256>::new();
    let key: heapless::String<256> = a.get("key", &mut kbuf).try_into().unwrap_or_default();
    let mut vbuf = heapless::String::<256>::new();
    let val: heapless::String<256> = if a.has("val") {
        a.get("val", &mut vbuf).try_into().unwrap_or_default()
    } else {
        a.get("value", &mut vbuf).try_into().unwrap_or_default()
    };
    match cfg::set(key.as_str(), val.as_str()).await {
        Ok(()) => {
            let mut j = String::new();
            let _ = write!(j, "{{\"ok\":true,\"cmd\":\"config\",\"key\":\"{key}\"}}");
            json_response(StatusCode::OK, j)
        }
        Err(_) => json_response(StatusCode::BAD_REQUEST, err_body("unknown key or invalid value")),
    }
}

async fn h_config_list() -> impl IntoResponse {
    let mut keys: heapless::String<1600> = heapless::String::new();
    cfg::list_json(&mut keys).await;
    let mut j = String::new();
    let _ = write!(j, "{{\"ok\":true,\"keys\":{keys}}}");
    json_response(StatusCode::OK, j)
}

async fn h_wifi_config(query: QueryString, body: String) -> impl IntoResponse {
    let a = args(&query, body.as_str());
    if a.has("ssid") {
        let mut buf = heapless::String::<256>::new();
        let _ = cfg::set("wifi.ssid", a.get("ssid", &mut buf)).await;
    }
    if a.has("pass") {
        let mut buf = heapless::String::<256>::new();
        let _ = cfg::set("wifi.pass", a.get("pass", &mut buf)).await;
    }
    let ssid = cfg::get_str("wifi.ssid").await;
    let mut j = String::new();
    let _ = write!(j, "{{\"ok\":true,\"cmd\":\"wifi/config\",\"ssid\":\"{ssid}\"}}");
    json_response(StatusCode::OK, j)
}

async fn h_http_status() -> impl IntoResponse {
    let mut j = String::new();
    let _ = write!(
        j,
        "{{\"ok\":true,\"cmd\":\"http/status\",\"enabled\":{}}}",
        running()
    );
    json_response(StatusCode::OK, j)
}

async fn h_http_config(query: QueryString, body: String) -> impl IntoResponse {
    let a = args(&query, body.as_str());
    let mut vbuf = heapless::String::<256>::new();
    let mut val = a.get("enable", &mut vbuf);
    if val.is_empty() {
        val = body.trim();
    }
    if matches!(val, "1" | "true" | "0" | "false") {
        let _ = cfg::set("http.enable", val).await;
        let mut j = String::new();
        let _ = write!(
            j,
            "{{\"ok\":true,\"cmd\":\"http/config\",\"enabled\":{}}}",
            running()
        );
        json_response(StatusCode::OK, j)
    } else {
        json_response(StatusCode::BAD_REQUEST, err_body("expected enable=1|0"))
    }
}

async fn h_http_clear() -> impl IntoResponse {
    // Arduino: force http on, drop http_auto.
    let _ = cfg::set("http.enable", "1").await;
    let _ = cfg::set("http.auto", "0").await;
    json_response(StatusCode::OK, "{\"ok\":true,\"cmd\":\"http/clear\"}".into())
}

async fn h_nvs_clear() -> impl IntoResponse {
    let _ = cfg::erase_all().await;
    RESTART.signal(()); // detached task reboots after the response lands
    json_response(
        StatusCode::OK,
        "{\"ok\":true,\"cmd\":\"nvs/clear\",\"restarting\":true}".into(),
    )
}

// ------------------------------------------------------------- keyboard ---

/// Keyboard op segment of /at-node/cmd/keyboard/<op> (broker method names).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum KbOp {
    Tap,
    Text,
    Key,
}

impl core::str::FromStr for KbOp {
    type Err = ();
    fn from_str(s: &str) -> Result<Self, ()> {
        match s {
            "tap" => Ok(Self::Tap),
            "text" => Ok(Self::Text),
            "key" => Ok(Self::Key),
            _ => Err(()),
        }
    }
}

/// POST /at-node/cmd/keyboard/<tap|text|key> (Arduino keyboard handlers;
/// same params as the MQTT methods).
async fn h_keyboard_post(op: KbOp, query: QueryString, body: String) -> impl IntoResponse {
    if !crate::kb::enabled() {
        return json_response(StatusCode::BAD_REQUEST, err_body("kbd disabled"));
    }
    let a = args(&query, body.as_str());
    let u = |key: &str, def: u32| {
        let mut buf = heapless::String::<256>::new();
        let v = a.get(key, &mut buf);
        if v.is_empty() {
            Some(def)
        } else if let Some(h) = v.strip_prefix("0x").or_else(|| v.strip_prefix("0X")) {
            u32::from_str_radix(h, 16).ok()
        } else {
            v.parse().ok()
        }
    };
    let mut j = String::new();
    match op {
        KbOp::Tap => {
            let (Some(k), Some(mods), Some(ms)) = (u("k", 0), u("mods", 0), u("ms", 50))
            else {
                return json_response(StatusCode::BAD_REQUEST, err_body("bad args"));
            };
            if k > 255 || mods > 255 || ms > 60_000 || !crate::kb::tap(mods as u8, k as u8, ms)
            {
                return json_response(StatusCode::BAD_REQUEST, err_body("bad args or busy"));
            }
            let _ = write!(j, "{{\"ok\":true,\"cmd\":\"keyboard/tap\"}}");
        }
        KbOp::Text => {
            let mut buf = heapless::String::<256>::new();
            let s = a.get("s", &mut buf);
            if s.is_empty() {
                return json_response(StatusCode::BAD_REQUEST, err_body("missing s"));
            }
            let (Some(ms), Some(gap)) = (u("ms", 40), u("gap", 40)) else {
                return json_response(StatusCode::BAD_REQUEST, err_body("bad args"));
            };
            let mut text: heapless::String<192> = heapless::String::new();
            if text.push_str(s).is_err() {
                return json_response(StatusCode::BAD_REQUEST, err_body("too long"));
            }
            if !crate::kb::text(text, ms, gap) {
                return json_response(StatusCode::CONFLICT, err_body("busy"));
            }
            let _ = write!(j, "{{\"ok\":true,\"cmd\":\"keyboard/text\"}}");
        }
        KbOp::Key => {
            let Some(mods) = u("mods", 0) else {
                return json_response(StatusCode::BAD_REQUEST, err_body("bad args"));
            };
            let mut r = crate::kb::Report {
                mods: mods as u8,
                ..Default::default()
            };
            for i in 0..6 {
                let mut kn = heapless::String::<8>::new();
                let _ = write!(kn, "k{i}");
                let v = u(&kn, 0);
                match v {
                    Some(v) if v <= 255 => r.keys[i] = v as u8,
                    Some(_) => {
                        return json_response(StatusCode::BAD_REQUEST, err_body("bad args"));
                    }
                    None => {}
                }
            }
            if !crate::kb::hold(r) {
                return json_response(StatusCode::CONFLICT, err_body("busy"));
            }
            let _ = write!(j, "{{\"ok\":true,\"cmd\":\"keyboard/key\"}}");
        }
    }
    json_response(StatusCode::OK, j)
}

// -------------------------------------------------------- rathole tunnel -

/// Tunnel operation segment of /at-node/cmd/tunnel/<op> (single route —
/// see the route table note).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum TunnelOp {
    Status,
    Config,
    Enable,
    Connect,
    Disconnect,
    Clear,
}

impl core::str::FromStr for TunnelOp {
    type Err = ();
    fn from_str(s: &str) -> Result<Self, ()> {
        match s {
            "status" => Ok(Self::Status),
            "config" => Ok(Self::Config),
            "enable" => Ok(Self::Enable),
            "connect" => Ok(Self::Connect),
            "disconnect" => Ok(Self::Disconnect),
            "clear" => Ok(Self::Clear),
            _ => Err(()),
        }
    }
}

/// GET /at-node/cmd/tunnel/status — everything else is POST-only.
async fn h_tunnel_get(op: TunnelOp) -> impl IntoResponse {
    if !rathole::enabled() {
        return json_response(StatusCode::BAD_REQUEST, err_body("tunnel disabled"));
    }
    if op != TunnelOp::Status {
        return json_response(StatusCode::BAD_REQUEST, err_body("POST required"));
    }
    let t = rathole::status_json().await;
    let mut j = String::new();
    let _ = write!(j, "{{\"ok\":true,\"tunnels\":[{t}]}}");
    json_response(StatusCode::OK, j)
}

/// POST /at-node/cmd/tunnel/<config|enable|connect|disconnect|clear>
/// (Arduino handlers; config fields server,token,service,local,retry,auto,
/// enable; empty token = unchanged).
async fn h_tunnel_post(op: TunnelOp, query: QueryString, body: String) -> impl IntoResponse {
    if !rathole::enabled() {
        return json_response(StatusCode::BAD_REQUEST, err_body("tunnel disabled"));
    }
    let a = args(&query, body.as_str());
    match op {
        TunnelOp::Status => {
            json_response(StatusCode::BAD_REQUEST, err_body("GET required"))
        }
        TunnelOp::Config => {
            {
                let mut buf = heapless::String::<256>::new();
                let id = a.get("id", &mut buf);
                if !id.is_empty() && id != "1" {
                    return json_response(StatusCode::BAD_REQUEST, err_body("invalid id"));
                }
            }
            for (arg, key) in [
                ("server", "tunnel.1.server"),
                ("token", "tunnel.1.token"),
                ("service", "tunnel.1.service"),
                ("local", "tunnel.1.local"),
                ("retry", "tunnel.1.retry"),
                ("auto", "tunnel.1.auto"),
                ("enable", "tunnel.1.enable"),
            ] {
                let mut buf = heapless::String::<256>::new();
                let v = a.get(arg, &mut buf);
                if !v.is_empty() && cfg::set(key, v).await.is_err() {
                    return json_response(StatusCode::BAD_REQUEST, err_body("bad value"));
                }
            }
            let t = rathole::status_json().await;
            let mut j = String::new();
            let _ = write!(j, "{{\"ok\":true,\"cmd\":\"tunnel/config\",\"tunnel\":{t}}}");
            json_response(StatusCode::OK, j)
        }
        TunnelOp::Enable => {
            let mut buf = heapless::String::<256>::new();
            let v = a.get("enable", &mut buf);
            if cfg::set("rathole.enable", v).await.is_err() {
                return json_response(StatusCode::BAD_REQUEST, err_body("bad value"));
            }
            let master = cfg::get_str("rathole.enable").await;
            let mut j = String::new();
            let _ = write!(
                j,
                "{{\"ok\":true,\"cmd\":\"tunnel/enable\",\"enabled\":{}}}",
                master == "1"
            );
            json_response(StatusCode::OK, j)
        }
        TunnelOp::Connect => {
            let t = rathole::status_json().await;
            if rathole::connect().await {
                let mut j = String::new();
                let _ = write!(j, "{{\"ok\":true,\"cmd\":\"tunnel/connect\",\"tunnel\":{t}}}");
                json_response(StatusCode::OK, j)
            } else {
                let mut j = String::new();
                let _ = write!(
                    j,
                    "{{\"ok\":false,\"error\":\"start failed\",\"tunnel\":{t}}}"
                );
                json_response(StatusCode::BAD_REQUEST, j)
            }
        }
        TunnelOp::Disconnect => {
            rathole::disconnect();
            json_response(StatusCode::OK, "{\"ok\":true,\"cmd\":\"tunnel/disconnect\"}".into())
        }
        TunnelOp::Clear => {
            rathole::clear().await;
            json_response(StatusCode::OK, "{\"ok\":true,\"cmd\":\"tunnel/clear\"}".into())
        }
    }
}

static RESTART: Signal<CriticalSectionRawMutex, ()> = Signal::new();

/// Delayed reboot helper for POST /at-node/cmd/nvs/clear (Arduino
/// schedule_restart(500)).
#[embassy_executor::task]
pub async fn restart_task() {
    RESTART.wait().await;
    embassy_time::Timer::after(Duration::from_millis(500)).await;
    esp_hal::system::software_reset();
}

// --------------------------------------------------------------- task ---

/// HTTP service model: ONE acceptor task + HANDLERS connection tasks.
/// The acceptor never blocks on request handling, so bursts (the SPA
/// fires ~10 parallel fetches on load) are queued, never refused.
/// With per-port listener tasks (= workers), every busy worker means
/// refused connections (observed: SPA load -> 6x ERR_CONNECTION_REFUSED).
pub const HANDLERS: usize = 3;
pub const ACCEPTORS: usize = 3;
/// Accepted-but-unserved connection queue depth. Also the tcp buffer
/// pool size: the acceptor only stalls when more than BACKLOG
/// connections are in flight at once (a much wider window than the
/// handler count, which is what actually matters for burst latency).
const BACKLOG: usize = 10;

/// One tcp rx+tx buffer pair per handler slot. Buffers are recycled
/// through FREE_IDX: an index leaves the pool before the socket is
/// created and returns only after serve() completes (socket dropped),
/// so no two live sockets can ever share a buffer.
static mut TCP_BUFS: [[u8; 2560]; BACKLOG] = [[0; 2560]; BACKLOG];

/// embassy-net sockets are !Send (smoltcp keeps per-socket state in the
/// stack), but every task here runs on ONE cooperative executor on core 0
/// — nothing ever crosses a real thread boundary.
struct ConnSocket(embassy_net::tcp::TcpSocket<'static>);

unsafe impl Send for ConnSocket {}

static CONN: embassy_sync::channel::Channel<
    CriticalSectionRawMutex,
    (ConnSocket, usize),
    BACKLOG,
> = embassy_sync::channel::Channel::new();
static FREE_IDX: embassy_sync::channel::Channel<CriticalSectionRawMutex, usize, BACKLOG> =
    embassy_sync::channel::Channel::new();

fn http_config() -> Config {
    Config::new(Timeouts {
        start_read_request: Duration::from_secs(5),
        persistent_start_read_request: Duration::from_secs(1),
        read_request: Duration::from_secs(2),
        write: Duration::from_secs(10),
    })
}

/// Boot-time init: running flag from http.auto && http.enable, and fill
/// the buffer pool. Call once from main before spawning tasks.
pub async fn init() {
    let auto = cfg::get_str("http.auto").await == "1";
    let enable = cfg::get_str("http.enable").await == "1";
    set_running(auto && enable);
    for i in 0..BACKLOG {
        let _ = FREE_IDX.try_send(i);
    }
}

/// Acceptor: listens while enabled, hands accepted connections to the
/// handler pool. Never blocks on request handling, so SYNs are always
/// accepted (backlog) instead of refused.
pub async fn acceptor_task(stack: Stack<'static>) -> ! {
    let mut changed = cfg::changed().expect("cfg change subscriber");
    loop {
        if !running() {
            embassy_futures::select::select(KICK.wait(), changed.next_message_pure()).await;
            if cfg::get_str("http.enable").await == "1" {
                set_running(true);
            }
            continue;
        }

        // Disable handling: stop the accept loop when http.enable -> 0.
        let disable = async {
            loop {
                match embassy_futures::select::select(
                    KICK.wait(),
                    changed.next_message_pure(),
                )
                .await
                {
                    embassy_futures::select::Either::First(()) => {
                        if !running() {
                            return;
                        }
                    }
                    embassy_futures::select::Either::Second(key) => {
                        if key == "http.enable" && cfg::get_str("http.enable").await != "1" {
                            set_running(false);
                            return;
                        }
                    }
                }
            }
        };

        let accept_one = async {
            let idx = FREE_IDX.receive().await;
            // SAFETY: idx is unique in the pool (see FREE_IDX invariant
            // above); no other reference to TCP_BUFS[idx] exists while
            // this socket lives.
            let bufs = unsafe { TCP_BUFS[idx].as_mut() };
            let (rx, tx) = bufs.split_at_mut(1536);
            let mut socket = embassy_net::tcp::TcpSocket::new(stack, rx, tx);
            socket.set_keep_alive(Some(Duration::from_secs(30)));
            socket.set_timeout(Some(Duration::from_secs(45)));
            match socket.accept(PORT).await {
                Ok(()) => {
                    CONN.send((ConnSocket(socket), idx)).await;
                }
                Err(e) => {
                    warn!("http: accept error: {e:?}");
                    let _ = FREE_IDX.try_send(idx);
                }
            }
        };

        // If disabled mid-accept, the pending socket is dropped (listener
        // closed) and we go back to idle.
        let _ = embassy_futures::select::select(accept_one, disable).await;
    }
}

/// Connection handler: drains the acceptor queue. Each handler builds
/// its app locally (the router type is unnameable outside).
pub async fn handler_task() -> ! {
    let app = build_app();
    let config = http_config();
    let mut http_buffer = [0u8; 3072];
    loop {
        let (ConnSocket(socket), idx) = CONN.receive().await;
        let server = picoserve::Server::new(&app, &config, &mut http_buffer[..]);
        if let Err(e) = server.serve(socket).await {
            warn!("http: serve error: {e:?}");
        }
        let _ = FREE_IDX.try_send(idx);
    }
}
