//! AT-Node rust-s3 — MQTT (TLS) remote control plane.
//!
//! Contract aligned with esp32/zephyr/src/mqttc.c:
//! - topics atnode/<device.name>/{cmd,resp,state,info}; client_id = name
//! - LWT retained "offline" on state; on connect: subscribe cmd, retained
//!   "online" + retained JSON info manifest
//! - cmd payload -> at::handle_line (same dispatcher as serial) -> resp
//! - port 1883 = plain TCP, anything else = TLS with CA-strong verify
//!   (CA DER embedded via build.rs when certs/ca.der exists; without it the
//!   build is loud and verification is disabled, Zephyr parity)
//! - 5 s reconnect backoff; every reconnect builds a FRESH socket/TLS/
//!   client triple and drops the old one first (H4: no context leak)
//! - AT+MQTT=connect|disconnect|status|enable,<0|1>|auto,<0|1>

use core::cell::RefCell;
use core::fmt::Write as _;


use embassy_net::dns::DnsQueryType;
use embassy_net::tcp::TcpSocket;
use embassy_net::{Ipv4Address, Stack};
use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
#[cfg(feature = "http")]
use embassy_sync::mutex::Mutex;
use embassy_sync::signal::Signal;
use embassy_time::{Duration, Timer};
use embedded_tls::pki::CertVerifier;
use embedded_tls::{
    Aes128GcmSha256, Certificate, CryptoProvider, CryptoRngCore, NoClock, TlsConfig,
    TlsConnection, TlsContext, TlsError, TlsVerifier,
};
use heapless::String;
use log::{info, warn};

use crate::{cfg, wifi};

const RECONNECT_BACKOFF: Duration = Duration::from_secs(5);
const WIFI_WAIT: Duration = Duration::from_secs(1);
/// Payload cap for one AT line (Zephyr PAYLOAD_BUF_SIZE).
const PAYLOAD_MAX: usize = 512;

#[cfg(have_ca)]
static CA_DER: &[u8] = include_bytes!("../certs/ca.der");

// ---------------------------------------------------------------- state ---

#[derive(Clone, Copy, Default)]
struct State {
    running: bool,
    connected: bool,
}

static STATE: critical_section::Mutex<RefCell<State>> =
    critical_section::Mutex::new(RefCell::new(State {
        running: false,
        connected: false,
    }));

/// Wakes the mqtt task (start/stop/cfg change).
static KICK: Signal<CriticalSectionRawMutex, ()> = Signal::new();

/// External publish/subscribe requests (HTTP mqtt/publish|subscribe).
/// The type stays visible so the no-http select branch can name it;
/// the channels/API are http-gated below.
#[allow(dead_code)]
pub struct PubReq {
    topic: String<128>,
    payload: String<512>,
    subscribe: bool,
}

#[cfg(feature = "http")]
static PUB_REQ: embassy_sync::channel::Channel<CriticalSectionRawMutex, PubReq, 2> =
    embassy_sync::channel::Channel::new();
#[cfg(feature = "http")]
static PUB_ACK: Signal<CriticalSectionRawMutex, bool> = Signal::new();
#[cfg(feature = "http")]
static PUB_LOCK: Mutex<CriticalSectionRawMutex, ()> = Mutex::new(());

/// Publish a message on the connected session (HTTP mqtt/publish).
/// Returns false when not connected / queue full / timed out.
#[cfg(feature = "http")]
pub async fn publish(topic: &str, payload: &str) -> bool {
    pub_req(topic, payload, false).await
}

/// Subscribe to an extra topic on the live session (HTTP mqtt/subscribe).
#[cfg(feature = "http")]
pub async fn subscribe(topic: &str) -> bool {
    pub_req(topic, "", true).await
}

#[cfg(feature = "http")]
async fn pub_req(topic: &str, payload: &str, subscribe: bool) -> bool {
    if !status().0 {
        return false;
    }
    let _g = PUB_LOCK.lock().await;
    let (Ok(topic), Ok(payload)) = (
        String::<128>::try_from(topic),
        String::<512>::try_from(payload),
    ) else {
        return false;
    };
    if PUB_REQ.try_send(PubReq {
        topic,
        payload,
        subscribe,
    })
    .is_err()
    {
        return false;
    }
    embassy_time::with_timeout(Duration::from_secs(3), PUB_ACK.wait())
        .await
        .unwrap_or_default()
}

fn set_state(f: impl FnOnce(&mut State)) {
    critical_section::with(|cs| f(&mut STATE.borrow(cs).borrow_mut()));
}

/// AT+MQTT=connect: start the client (runtime only, nothing persisted).
pub async fn start() -> Result<(), ()> {
    if cfg::get_str("mqtt.broker").await.is_empty() {
        return Err(());
    }
    set_state(|s| s.running = true);
    KICK.signal(());
    Ok(())
}

/// AT+MQTT=disconnect: graceful stop (retained offline + DISCONNECT).
pub fn stop() {
    set_state(|s| s.running = false);
    KICK.signal(());
}

/// (connected, running) for AT+MQTT=status / AT+STATUS.
pub fn status() -> (bool, bool) {
    critical_section::with(|cs| {
        let s = STATE.borrow(cs).borrow();
        (s.connected, s.running)
    })
}

/// AT+STATUS mqtt= field: off / connecting / connected (Zephyr cmd_status).
pub fn status_str() -> &'static str {
    let (connected, running) = status();
    if !running {
        "off"
    } else if connected {
        "connected"
    } else {
        "connecting"
    }
}

// ------------------------------------------------------- eio 0.6/0.7 bridges ---
// embassy-net/rust-mqtt use embedded-io-async 0.6, embedded-tls 0.19 uses
// 0.7. Bridge both directions; error kinds map best-effort (any error ends
// the session either way).

use embedded_io_07::Error as _;

#[derive(Debug)]
struct BridgeErr06(embedded_io_async_07::ErrorKind);

impl embedded_io_async::Error for BridgeErr06 {
    fn kind(&self) -> embedded_io_async::ErrorKind {
        use embedded_io_async::ErrorKind as K;
        match self.0 {
            embedded_io_async_07::ErrorKind::NotConnected => K::NotConnected,
            embedded_io_async_07::ErrorKind::ConnectionReset => K::ConnectionReset,
            embedded_io_async_07::ErrorKind::ConnectionAborted => K::ConnectionAborted,
            embedded_io_async_07::ErrorKind::TimedOut => K::TimedOut,
            embedded_io_async_07::ErrorKind::BrokenPipe => K::BrokenPipe,
            embedded_io_async_07::ErrorKind::WriteZero => K::WriteZero,
            _ => K::Other,
        }
    }
}



/// eio 0.6 traits over an eio 0.7 transport (TlsConnection -> rust-mqtt).
struct Eio6<T>(T);

impl<T: embedded_io_async_07::ErrorType> embedded_io_async::ErrorType for Eio6<T> {
    type Error = BridgeErr06;
}

impl<T: embedded_io_async_07::Read> embedded_io_async::Read for Eio6<T>
where
    T::Error: embedded_io_07::Error,
{
    async fn read(&mut self, buf: &mut [u8]) -> Result<usize, Self::Error> {
        self.0.read(buf).await.map_err(|e| BridgeErr06(e.kind()))
    }
}

impl<T: embedded_io_async_07::Write> embedded_io_async::Write for Eio6<T>
where
    T::Error: embedded_io_07::Error,
{
    async fn write(&mut self, buf: &[u8]) -> Result<usize, Self::Error> {
        self.0.write(buf).await.map_err(|e| BridgeErr06(e.kind()))
    }

    async fn flush(&mut self) -> Result<(), Self::Error> {
        self.0.flush().await.map_err(|e| BridgeErr06(e.kind()))
    }
}

// -------------------------------------------------------------- TLS glue ---

/// esp-hal's Rng is the hardware TRNG but does not carry the CryptoRng
/// marker; wrap it. (S3 TRNG is a true random source.)
struct S3CryptoRng(esp_hal::rng::Rng);

impl rand_core::RngCore for S3CryptoRng {
    fn next_u32(&mut self) -> u32 {
        self.0.random()
    }

    fn next_u64(&mut self) -> u64 {
        (self.0.random() as u64) << 32 | self.0.random() as u64
    }

    fn fill_bytes(&mut self, dest: &mut [u8]) {
        for chunk in dest.chunks_mut(4) {
            let r = self.0.random().to_le_bytes();
            chunk.copy_from_slice(&r[..chunk.len()]);
        }
    }

    fn try_fill_bytes(&mut self, dest: &mut [u8]) -> Result<(), rand_core::Error> {
        self.fill_bytes(dest);
        Ok(())
    }
}

impl rand_core::CryptoRng for S3CryptoRng {}

struct Provider<'a, R> {
    rng: R,
    #[cfg(have_ca)]
    verifier: CertVerifier<'a, Aes128GcmSha256, NoClock, 4096>,
    #[cfg(not(have_ca))]
    _marker: core::marker::PhantomData<&'a ()>,
}

impl<R> CryptoProvider for Provider<'_, R>
where
    R: CryptoRngCore,
{
    type CipherSuite = Aes128GcmSha256;
    type Signature = [u8; 0];

    fn rng(&mut self) -> impl CryptoRngCore {
        &mut self.rng
    }

    #[cfg(have_ca)]
    fn verifier(&mut self) -> Result<&mut impl TlsVerifier<Self::CipherSuite>, TlsError> {
        Ok(&mut self.verifier)
    }
}

// ------------------------------------------------------------- buffers ---

/// Compile-time capability flag (feature matrix in Cargo.toml).
pub fn enabled() -> bool {
    cfg!(feature = "mqtt")
}

/// All session buffers live in the dedicated PSRAM heap (main.rs
/// PSRAM_HEAP): CPU-only access (embedded-tls / smoltcp memcpy), never
/// DMA, and the control plane does not care about the extra latency.
/// Each Box is OWNED by its static cell (lives for the whole program).
macro_rules! psram_cell {
    ($name:ident, $n:expr) => {
        static mut $name: Option<
            allocator_api2::boxed::Box<[u8; $n], &'static esp_alloc::EspHeap>,
        > = None;
    };
}

psram_cell!(TLS_RX, 16640);
psram_cell!(TLS_TX, 8192);
psram_cell!(MQTT_TX, 3072);
psram_cell!(MQTT_RX, 2048);
psram_cell!(TCP_RX, 4096);
psram_cell!(TCP_TX, 2048);

/// One-time buffer carve-out (mqtt init). addr_of_mut! + raw deref: no
/// reference to the static itself is created (2024 static_mut_refs);
/// single cooperative executor, single call site.
macro_rules! take {
    ($cell:ident) => {
        unsafe {
            (*core::ptr::addr_of_mut!($cell))
                .get_or_insert_with(|| {
                    allocator_api2::boxed::Box::new_in([0u8; _], &crate::PSRAM_HEAP)
                })
                .as_mut()
        }
    };
}

pub struct Buffers<'a> {
    tls_rx: &'a mut [u8; 16640],
    tls_tx: &'a mut [u8; 8192],
    mqtt_tx: &'a mut [u8; 3072],
    mqtt_rx: &'a mut [u8; 2048],
    tcp_rx: &'a mut [u8; 4096],
    tcp_tx: &'a mut [u8; 2048],
}

/// One-time buffer carve-out (mqtt init).
pub fn take_buffers() -> Buffers<'static> {
    Buffers {
        tls_rx: take!(TLS_RX),
        tls_tx: take!(TLS_TX),
        mqtt_tx: take!(MQTT_TX),
        mqtt_rx: take!(MQTT_RX),
        tcp_rx: take!(TCP_RX),
        tcp_tx: take!(TCP_TX),
    }
}

// ------------------------------------------------------------------ task ---

struct Topics {
    cmd: String<80>,
    resp: String<80>,
    state: String<80>,
    info: String<80>,
}

impl Topics {
    fn new(name: &str) -> Self {
        let mut t = Topics {
            cmd: String::new(),
            resp: String::new(),
            state: String::new(),
            info: String::new(),
        };
        let _ = write!(t.cmd, "atnode/{name}/cmd");
        let _ = write!(t.resp, "atnode/{name}/resp");
        let _ = write!(t.state, "atnode/{name}/state");
        let _ = write!(t.info, "atnode/{name}/info");
        t
    }
}

/// Resolve broker host: IP literal fast path, DNS A otherwise (Zephyr
/// resolve_broker).
async fn resolve(stack: Stack<'static>, host: &str) -> Option<Ipv4Address> {
    if let Ok(ip) = host.parse::<Ipv4Address>() {
        return Some(ip);
    }
    match stack.dns_query(host, DnsQueryType::A).await {
        Ok(addrs) => addrs.first().map(|a| match a {
            embassy_net::IpAddress::Ipv4(v4) => *v4,
        }),
        Err(_) => None,
    }
}

/// One connect attempt: fresh TCP (+TLS) + MQTT client, run until drop.
/// Returns when the connection ends (error/disconnect/stop request).
#[allow(
    clippy::too_many_arguments,
    clippy::large_stack_frames,
    reason = "async fn futures live in the embassy static task pool, not on a call stack"
)]
async fn run_session(
    stack: Stack<'static>,
    bufs: Buffers<'_>,
    host: &str,
    port: u16,
    user: &str,
    pass: &str,
    name: &str,
    topics: &Topics,
    changed: &mut cfg::ChangedSub,
) {
    let Some(ip) = resolve(stack, host).await else {
        warn!("mqtt: resolve {host} failed");
        return;
    };

    let Buffers {
        tls_rx,
        tls_tx,
        mqtt_tx,
        mqtt_rx,
        tcp_rx,
        tcp_tx,
    } = bufs;
    let mut socket = TcpSocket::new(stack, &mut tcp_rx[..], &mut tcp_tx[..]);
    socket.set_timeout(Some(Duration::from_secs(10)));
    if let Err(e) = socket.connect((ip, port)).await {
        warn!("mqtt: tcp connect failed: {e:?}");
        return;
    }
    info!("mqtt: tcp ok");

    if port == 1883 {
        run_client(
            &mut Eio6(&mut socket),
            mqtt_tx,
            mqtt_rx,
            user,
            pass,
            name,
            topics,
            changed,
        )
        .await;
    } else {
        let mut tls: TlsConnection<'_, _, Aes128GcmSha256> =
            TlsConnection::new(&mut socket, &mut tls_rx[..], &mut tls_tx[..]);

        #[cfg(have_ca)]
        let mut provider = Provider {
            rng: S3CryptoRng(esp_hal::rng::Rng::new()),
            verifier: CertVerifier::new(Certificate::X509(CA_DER)),
        };
        #[cfg(not(have_ca))]
        let mut provider = Provider::<S3CryptoRng> {
            rng: S3CryptoRng(esp_hal::rng::Rng::new()),
            _marker: core::marker::PhantomData,
        };
        #[cfg(not(have_ca))]
        warn!("mqtt: CA DER missing (certs/ca.der), TLS verify DISABLED");

        // RSA dev CA: offer RSA signature schemes in the ClientHello.
        let tls_config = TlsConfig::new()
            .with_server_name(host)
            .enable_rsa_signatures();
        let ctx = TlsContext::new(&tls_config, &mut provider);
        if let Err(e) = tls.open(ctx).await {
            warn!("mqtt: TLS handshake failed: {e:?}");
            return;
        }
        run_client(
            &mut Eio6(&mut tls),
            mqtt_tx,
            mqtt_rx,
            user,
            pass,
            name,
            topics,
            changed,
        )
        .await;
    }
    // socket/TLS/client all drop here -> full resource release (H4)
}

#[allow(
    clippy::too_many_arguments,
    clippy::large_stack_frames,
    reason = "async fn futures live in the embassy static task pool, not on a call stack"
)]
async fn run_client<T>(
    transport: &mut T,
    mqtt_tx: &mut [u8; 3072],
    mqtt_rx: &mut [u8; 2048],
    user: &str,
    pass: &str,
    name: &str,
    topics: &Topics,
    changed: &mut cfg::ChangedSub,
) where
    T: embedded_io_async::Read + embedded_io_async::Write,
{
    let mut client = v3::Client::new(transport, mqtt_tx, mqtt_rx);

    info!("mqtt: sending CONNECT (v3.1.1)");
    match client
        .connect(name, topics.state.as_str(), user, pass)
        .await
    {
        Ok(()) => {}
        Err(v3::Error::Refused(rc)) => {
            warn!("mqtt: connect refused, rc={rc}");
            return;
        }
        Err(e) => {
            warn!("mqtt: connect failed: {e:?}");
            return;
        }
    }
    set_state(|s| s.connected = true);
    info!("mqtt: connected");

    if client.subscribe(topics.cmd.as_str()).await.is_err() {
        warn!("mqtt: subscribe failed");
        set_state(|s| s.connected = false);
        return;
    }
    let _ = client
        .publish(topics.state.as_str(), b"online", true)
        .await;
    let info_json = crate::api::sys_info_json().await;
    let _ = client
        .publish(topics.info.as_str(), info_json.as_bytes(), true)
        .await;

    let grace: Result<(), v3::Error> = loop {
        let pub_recv = async {
            #[cfg(feature = "http")]
            {
                PUB_REQ.receive().await
            }
            #[cfg(not(feature = "http"))]
            {
                core::future::pending::<PubReq>().await
            }
        };
        match embassy_futures::select::select4(
            client.recv(),
            Timer::after(Duration::from_secs(30)), // keep_alive 60 / 2
            select3_cfg(changed),
            pub_recv,
        )
        .await
        {
            embassy_futures::select::Either4::First(Ok(v3::Event::Publish)) => {
                if client.last_topic_matches(topics.cmd.as_str()) {
                    // Arduino RPC: "<reqid> <method> <query>" ->
                    // resp {"id":"<reqid>",<inner>}
                    let payload = client.last_payload();
                    let n = payload.len().min(PAYLOAD_MAX);
                    let text = core::str::from_utf8(&payload[..n]).unwrap_or("");
                    if let Some(sp1) = text.find(' ') {
                        let reqid = &text[..sp1];
                        let rest = &text[sp1 + 1..];
                        let (method, query) = match rest.find(' ') {
                            Some(sp2) => (&rest[..sp2], &rest[sp2 + 1..]),
                            None => (rest, ""),
                        };
                        let inner = mqtt_exec(method, query).await;
                        let mut resp: String<2560> = String::new();
                        let _ = write!(resp, "{{\"id\":\"{reqid}\",{inner}}}");
                        if client
                            .publish(topics.resp.as_str(), resp.as_bytes(), false)
                            .await
                            .is_err()
                        {
                            break Err(v3::Error::Io);
                        }
                    }
                }
            }
            embassy_futures::select::Either4::First(Ok(v3::Event::Pingresp)) => {}
            embassy_futures::select::Either4::First(Ok(v3::Event::Disconnect)) => {
                break Err(v3::Error::Io)
            }
            embassy_futures::select::Either4::First(Err(e)) => break Err(e),
            embassy_futures::select::Either4::Second(()) => {
                if let Err(e) = client.ping().await {
                    break Err(e);
                }
            }
            embassy_futures::select::Either4::Third(Ctl::Stop) => break Ok(()),
            embassy_futures::select::Either4::Third(Ctl::CfgChanged) => {
                break Err(v3::Error::Io)
            }
            embassy_futures::select::Either4::Third(Ctl::None) => continue,
            embassy_futures::select::Either4::Fourth(req) => {
                #[cfg(feature = "http")]
                {
                    let ok = if req.subscribe {
                        client.subscribe(req.topic.as_str()).await.is_ok()
                    } else {
                        client
                            .publish(req.topic.as_str(), req.payload.as_bytes(), false)
                            .await
                            .is_ok()
                    };
                    PUB_ACK.signal(ok);
                }
                #[cfg(not(feature = "http"))]
                let _ = req;
            }
        }
        if !status().1 {
            break Ok(()); // AT+MQTT=disconnect during idle
        }
    };

    set_state(|s| s.connected = false);
    // Graceful exit: retained "offline" + DISCONNECT (Zephyr parity).
    let _ = client.publish(topics.state.as_str(), b"offline", true).await;
    let _ = client.disconnect().await;
    if let Err(e) = grace {
        info!("mqtt: session ended: {e:?}");
    }
    // Heap watermark per session (H4: leaks would drift this down).
    info!("mqtt: internal heap free {} bytes", esp_alloc::HEAP.free());
}

/// keyboard/tap, keyboard/text, keyboard/key — broker-documented method
/// shapes. Params: tap=k,mods,ms; text=s,ms,gap; key=mods,k0..k5.
#[cfg(feature = "mqtt")]
fn keyboard_exec(method: &str, query: &str) -> String<2560> {
    let mut out: String<2560> = String::new();
    let num = |key: &str, def: u32| -> Option<u32> {
        let mut buf: String<256> = String::new();
        let v = crate::api::query_get(query, key, &mut buf);
        if v.is_empty() {
            Some(def)
        } else if let Some(h) = v.strip_prefix("0x").or_else(|| v.strip_prefix("0X")) {
            u32::from_str_radix(h, 16).ok()
        } else {
            v.parse().ok()
        }
    };
    match method {
        "keyboard/tap" => {
            match (num("k", 0), num("mods", 0), num("ms", 50)) {
                (Some(k), Some(m), Some(ms))
                    if k <= 255 && m <= 255 && ms <= 60_000
                        && crate::kb::tap(m as u8, k as u8, ms) =>
                {
                    let _ = out.push_str("\"ok\":true");
                }
                _ => {
                    let _ = out.push_str("\"ok\":false,\"error\":\"bad args or busy\"");
                }
            }
        }
        "keyboard/text" => {
            let mut sbuf: String<256> = String::new();
            let s = crate::api::query_get(query, "s", &mut sbuf);
            if s.is_empty() {
                let _ = out.push_str("\"ok\":false,\"error\":\"missing s\"");
            } else {
                let mut text: heapless::String<192> = heapless::String::new();
                let (Some(ms), Some(gap)) = (num("ms", 40), num("gap", 40)) else {
                    let _ = out.push_str("\"ok\":false,\"error\":\"bad args\"");
                    return out;
                };
                if text.push_str(s).is_err() {
                    let _ = out.push_str("\"ok\":false,\"error\":\"too long\"");
                } else if crate::kb::text(text, ms, gap) {
                    let _ = out.push_str("\"ok\":true");
                } else {
                    let _ = out.push_str("\"ok\":false,\"error\":\"busy\"");
                }
            }
        }
        "keyboard/key" => {
            let Some(mods) = num("mods", 0).filter(|m| *m <= 255) else {
                let _ = out.push_str("\"ok\":false,\"error\":\"bad args\"");
                return out;
            };
            let mut r = crate::kb::Report {
                mods: mods as u8,
                ..Default::default()
            };
            let mut bad = false;
            for i in 0..6 {
                let mut kn: heapless::String<8> = heapless::String::new();
                let _ = write!(kn, "k{i}");
                match num(&kn, 0) {
                    Some(v) if v <= 255 => r.keys[i] = v as u8,
                    Some(_) => bad = true,
                    None => {}
                }
            }
            if bad || !crate::kb::hold(r) {
                let _ = out.push_str("\"ok\":false,\"error\":\"bad args or busy\"");
            } else {
                let _ = out.push_str("\"ok\":true");
            }
        }
        _ => {
            let _ = out.push_str("\"ok\":false,\"error\":\"unknown method\"");
        }
    }
    out
}

/// Arduino mqtt_exec(): RPC methods over the cmd channel. Returns the
/// inner JSON fragment of the resp object (without {"id":..} wrapper).
/// Method set follows the implemented stages; keyboard/* and ble/* join
/// in R5/R6.
async fn mqtt_exec(method: &str, query: &str) -> String<2560> {
    let mut out: String<2560> = String::new();
    let mut scratch: String<256> = String::new();

    if matches!(method, "gpio/write" | "gpio/read" | "adc/read") && !crate::hws::enabled() {
        let _ = out.push_str("\"ok\":false,\"error\":\"hws disabled\"");
        return out;
    }
    if method.starts_with("keyboard/") && !crate::kb::enabled() {
        let _ = out.push_str("\"ok\":false,\"error\":\"kbd disabled\"");
        return out;
    }
    if method.starts_with("keyboard/") {
        return keyboard_exec(method, query);
    }

    match method {
        "gpio/write" => {
            let pin: String<256> = crate::api::query_get(query, "pin", &mut scratch)
                .try_into()
                .unwrap_or_default();
            let level: String<256> = crate::api::query_get(query, "level", &mut scratch)
                .try_into()
                .unwrap_or_default();
            match (pin.parse::<u32>(), level.parse::<u32>()) {
                (Ok(p), Ok(l)) if p <= 255 && crate::hws::gpio_write(p as u8, l != 0).is_ok() => {
                    let _ = out.push_str("\"ok\":true");
                }
                _ => {
                    let _ = out.push_str("\"ok\":false,\"error\":\"bad pin\"");
                }
            }
        }
        "gpio/read" => {
            let pin = crate::api::query_get(query, "pin", &mut scratch);
            match pin.parse::<u32>().ok().and_then(|p| {
                if p <= 255 {
                    crate::hws::gpio_read(p as u8).ok()
                } else {
                    None
                }
            }) {
                Some(level) => {
                    let _ = write!(out, "\"ok\":true,\"level\":{level}");
                }
                None => {
                    let _ = out.push_str("\"ok\":false,\"error\":\"bad pin\"");
                }
            }
        }
        "led" => {
            // Empty query = status; color=#RRGGBB|r,g,b|off|auto = set
            // (same spec as AT+LED and POST /at-node/cmd/led).
            let color = crate::api::query_get(query, "color", &mut scratch);
            if color.is_empty() {
                let (r, g, b, mode) = crate::led::current();
                let _ = write!(out, "\"ok\":true,\"r\":{r},\"g\":{g},\"b\":{b},\"mode\":\"{mode}\"");
            } else if !crate::led::enabled() {
                let _ = out.push_str("\"ok\":false,\"error\":\"led disabled\"");
            } else {
                match crate::led::parse(color) {
                    Some(act) => {
                        crate::led::apply_action(act);
                        let _ = out.push_str("\"ok\":true");
                    }
                    None => {
                        let _ = out.push_str("\"ok\":false,\"error\":\"bad color\"");
                    }
                }
            }
        }
        "adc/read" => {
            let ch = crate::api::query_get(query, "ch", &mut scratch);
            match ch.parse::<u32>().ok().and_then(|c| {
                crate::hws::adc_read_mv(c as u8).ok()
            }) {
                Some(mv) => {
                    let _ = write!(out, "\"ok\":true,\"mv\":{mv}");
                }
                None => {
                    let _ = out.push_str("\"ok\":false,\"error\":\"bad channel\"");
                }
            }
        }
        "sys/info" => {
            let info = crate::api::sys_info_json().await;
            let _ = write!(out, "\"ok\":true,\"info\":{info}");
        }
        "tunnel/status" => {
            if !crate::rathole::enabled() {
                let _ = out.push_str("\"ok\":false,\"error\":\"tunnel disabled\"");
            } else {
                let t = crate::rathole::status_json().await;
                let _ = write!(out, "\"ok\":true,\"tunnels\":[{t}]");
            }
        }
        "tunnel/enable" => {
            let v: String<256> = crate::api::query_get(query, "enable", &mut scratch)
                .try_into()
                .unwrap_or_default();
            if !crate::rathole::enabled() {
                let _ = out.push_str("\"ok\":false,\"error\":\"tunnel disabled\"");
            } else if crate::cfg::set("rathole.enable", v.as_str()).await.is_ok() {
                let _ = out.push_str("\"ok\":true");
            } else {
                let _ = out.push_str("\"ok\":false,\"error\":\"bad value\"");
            }
        }
        m if m.starts_with("tunnel/") && crate::rathole::enabled() => {
            // id is always 1 on this firmware (single tunnel).
            let id = crate::api::query_get(query, "id", &mut scratch);
            if !id.is_empty() && id != "1" {
                let _ = out.push_str("\"ok\":false,\"error\":\"invalid id\"");
            } else {
                match m {
                    "tunnel/connect" => {
                        if crate::rathole::connect().await {
                            let _ = out.push_str("\"ok\":true");
                        } else {
                            let _ = out.push_str("\"ok\":false,\"error\":\"start failed\"");
                        }
                    }
                    "tunnel/disconnect" => {
                        crate::rathole::disconnect();
                        let _ = out.push_str("\"ok\":true");
                    }
                    "tunnel/clear" => {
                        crate::rathole::clear().await;
                        let _ = out.push_str("\"ok\":true");
                    }
                    "tunnel/config" => {
                        let mut bad = false;
                        for (arg, key) in [
                            ("server", "tunnel.1.server"),
                            ("token", "tunnel.1.token"),
                            ("service", "tunnel.1.service"),
                            ("local", "tunnel.1.local"),
                            ("retry", "tunnel.1.retry"),
                            ("auto", "tunnel.1.auto"),
                            ("enable", "tunnel.1.enable"),
                        ] {
                            let v: String<256> =
                                crate::api::query_get(query, arg, &mut scratch)
                                    .try_into()
                                    .unwrap_or_default();
                            if !v.is_empty() && crate::cfg::set(key, v.as_str()).await.is_err() {
                                bad = true;
                            }
                        }
                        if bad {
                            let _ = out.push_str("\"ok\":false,\"error\":\"bad value\"");
                        } else {
                            let t = crate::rathole::status_json().await;
                            let _ = write!(out, "\"ok\":true,\"tunnel\":{t}");
                        }
                    }
                    _ => {
                        let _ = out.push_str("\"ok\":false,\"error\":\"unknown method\"");
                    }
                }
            }
        }
        m if m.starts_with("tunnel/") => {
            let _ = out.push_str("\"ok\":false,\"error\":\"tunnel disabled\"");
        }
        "ability" => {
            let ab = crate::api::ability_json();
            let _ = write!(out, "\"ok\":true,\"ability\":{ab}");
        }
        "config/set" => {
            let key: String<256> = crate::api::query_get(query, "key", &mut scratch)
                .try_into()
                .unwrap_or_default();
            let val: String<256> = crate::api::query_get(query, "val", &mut scratch)
                .try_into()
                .unwrap_or_default();
            if key.is_empty() {
                let _ = out.push_str("\"ok\":false,\"error\":\"missing key\"");
            } else if crate::cfg::set(key.as_str(), val.as_str()).await.is_ok() {
                let _ = out.push_str("\"ok\":true");
            } else {
                let _ = out.push_str(
                    "\"ok\":false,\"error\":\"unknown key or invalid value\"",
                );
            }
        }
        "config/get" => {
            let key: String<256> = crate::api::query_get(query, "key", &mut scratch)
                .try_into()
                .unwrap_or_default();
            match crate::cfg::get(key.as_str()).await {
                Ok(val) => {
                    let _ = write!(out, "\"ok\":true,\"value\":\"{val}\"");
                }
                Err(_) => {
                    let _ = out.push_str("\"ok\":false,\"error\":\"unknown key\"");
                }
            }
        }
        "config/list" => {
            let mut keys: String<1600> = String::new();
            crate::cfg::list_json(&mut keys).await;
            let _ = write!(out, "\"ok\":true,\"keys\":{keys}");
        }
        _ => {
            let _ = out.push_str("\"ok\":false,\"error\":\"unknown method\"");
        }
    }
    out
}

// -------------------------------------------------- MQTT 3.1.1 mini client ---
// rust-mqtt 0.3 has v3.1.1 stubbed out (v5 only) and the atnode broker
// (amqtt) speaks 3.1.1 only, so the client is implemented here against the
// Zephyr contract: QoS0 everywhere, single subscription, LWT, clean session.
mod v3 {
    #[derive(Debug)]
    pub enum Error {
        Io,
        Protocol,
        Refused(u8),
    }

    pub enum Event {
        Publish,
        Pingresp,
        Disconnect,
    }

    pub struct Client<'a, T> {
        t: &'a mut T,
        tx: &'a mut [u8; 3072],
        rx: &'a mut [u8; 2048],
        topic_len: usize,
        payload_len: usize,
    }

    impl<'a, T> Client<'a, T> {
        pub fn new(t: &'a mut T, tx: &'a mut [u8; 3072], rx: &'a mut [u8; 2048]) -> Self {
            Self {
                t,
                tx,
                rx,
                topic_len: 0,
                payload_len: 0,
            }
        }
    }

    impl<T: embedded_io_async::Read + embedded_io_async::Write> Client<'_, T> {
        fn put_str(&mut self, s: &[u8], pos: &mut usize) {
            self.tx[*pos] = (s.len() >> 8) as u8;
            self.tx[*pos + 1] = (s.len() & 0xFF) as u8;
            self.tx[*pos + 2..*pos + 2 + s.len()].copy_from_slice(s);
            *pos += 2 + s.len();
        }

        async fn send(&mut self, len: usize) -> Result<(), Error> {
            // Short-write safe loop (same lesson as the UART path).
            let mut left = &self.tx[..len];
            while !left.is_empty() {
                let n = self.t.write(left).await.map_err(|_| Error::Io)?;
                if n == 0 {
                    return Err(Error::Io);
                }
                left = &left[n..];
            }
            self.t.flush().await.map_err(|_| Error::Io)
        }

        pub async fn connect(
            &mut self,
            client_id: &str,
            will_topic: &str,
            user: &str,
            pass: &str,
        ) -> Result<(), Error> {
            let mut p = 0usize;
            self.tx[p] = 0x10; // CONNECT
            p += 1;
            let rem_len_pos = p;
            p += 2; // remaining length varint (1-2 bytes)
            self.put_str(b"MQTT", &mut p);
            self.tx[p] = 4; // protocol level 3.1.1
            p += 1;
            let mut flags = 0x02u8; // clean session
            flags |= 0x04 | 0x20; // will flag + will retain
            if !user.is_empty() {
                flags |= 0x80;
            }
            if !pass.is_empty() {
                flags |= 0x40;
            }
            self.tx[p] = flags;
            self.tx[p + 1] = 0;
            self.tx[p + 2] = 60; // keep alive 60 s
            p += 3;
            self.put_str(client_id.as_bytes(), &mut p);
            self.put_str(will_topic.as_bytes(), &mut p);
            self.put_str(b"offline", &mut p);
            if !user.is_empty() {
                self.put_str(user.as_bytes(), &mut p);
            }
            if !pass.is_empty() {
                self.put_str(pass.as_bytes(), &mut p);
            }
            let body = p - rem_len_pos - 2;
            // varint remaining length (1-2 bytes)
            let mut n = body;
            let mut rp = rem_len_pos;
            loop {
                let mut b = (n % 128) as u8;
                n /= 128;
                if n > 0 {
                    b |= 0x80;
                }
                self.tx[rp] = b;
                rp += 1;
                if n == 0 {
                    break;
                }
            }
            if rp - rem_len_pos == 1 {
                // encoded in 1 byte: shift body left to close the gap
                self.tx
                    .copy_within(rem_len_pos + 2..p, rem_len_pos + 1);
                p -= 1;
            }
            self.send(p).await?;

            // CONNACK: 0x20 0x02 sp rc
            self.read_into(4).await?;
            if self.rx[0] != 0x20 || self.rx[1] != 0x02 {
                return Err(Error::Protocol);
            }
            if self.rx[3] != 0 {
                return Err(Error::Refused(self.rx[3]));
            }
            Ok(())
        }

        /// Send SUBSCRIBE. The SUBACK is consumed by recv() as an
        /// ignorable event: brokers (amqtt) may deliver live PUBLISHes
        /// before the SUBACK, and QoS0 does not need the ack.
        pub async fn subscribe(&mut self, topic: &str) -> Result<(), Error> {
            let mut p = 0usize;
            self.tx[p] = 0x82; // SUBSCRIBE qos1
            p += 1;
            let rem_pos = p;
            p += 1;
            self.tx[p] = 0;
            self.tx[p + 1] = 1; // packet id 1
            p += 2;
            self.put_str(topic.as_bytes(), &mut p);
            self.tx[p] = 0; // qos 0
            p += 1;
            let body = p - rem_pos - 1;
            if body > 127 {
                return Err(Error::Protocol);
            }
            self.tx[rem_pos] = body as u8;
            self.send(p).await
        }

        pub async fn publish(
            &mut self,
            topic: &str,
            payload: &[u8],
            retain: bool,
        ) -> Result<(), Error> {
            let mut p = 0usize;
            self.tx[p] = if retain { 0x31 } else { 0x30 };
            p += 1;
            let rem_pos = p;
            let body = 2 + topic.len() + payload.len();
            // write remaining length varint (1-2 bytes here)
            let mut n = body;
            let mut rp = rem_pos;
            loop {
                let mut b = (n % 128) as u8;
                n /= 128;
                if n > 0 {
                    b |= 0x80;
                }
                self.tx[rp] = b;
                rp += 1;
                if n == 0 {
                    break;
                }
            }
            p = rp;
            self.put_str(topic.as_bytes(), &mut p);
            self.tx[p..p + payload.len()].copy_from_slice(payload);
            p += payload.len();
            self.send(p).await
        }

        pub async fn ping(&mut self) -> Result<(), Error> {
            self.tx[0] = 0xC0;
            self.tx[1] = 0;
            self.send(2).await
        }

        pub async fn disconnect(&mut self) -> Result<(), Error> {
            self.tx[0] = 0xE0;
            self.tx[1] = 0;
            self.send(2).await
        }

        /// Read exactly n bytes into rx[0..n].
        async fn read_into(&mut self, n: usize) -> Result<(), Error> {
            let mut got = 0usize;
            while got < n {
                let r = self
                    .t
                    .read(&mut self.rx[got..n])
                    .await
                    .map_err(|_| Error::Io)?;
                if r == 0 {
                    return Err(Error::Io);
                }
                got += r;
            }
            Ok(())
        }

        /// Read one packet. PUBLISH payload is placed in rx (truncated to
        /// the buffer, stream kept aligned). Returns the event kind;
        /// topic/payload metadata is queryable after Event::Publish.
        pub async fn recv(&mut self) -> Result<Event, Error> {
            self.read_into(2).await?;
            let packet_type = self.rx[0] & 0xF0;
            // remaining length varint (up to 3 more bytes)
            let mut rem = (self.rx[1] & 0x7F) as usize;
            let mut shift = 7;
            let mut b = self.rx[1];
            while b & 0x80 != 0 {
                self.read_into(1).await?;
                b = self.rx[0];
                rem += ((b & 0x7F) as usize) << shift;
                shift += 7;
            }
            match packet_type {
                0x30 => {
                    // PUBLISH qos0: topic(2+len) payload
                    self.read_into(2).await?;
                    let topic_len = ((self.rx[0] as usize) << 8) | self.rx[1] as usize;
                    if 2 + topic_len > rem || rem - 2 - topic_len > self.rx.len() {
                        // Oversized topic or payload: drain the body, skip.
                        self.drain(rem).await?;
                        return Ok(Event::Pingresp); // caller ignores
                    }
                    // read topic+payload into rx in one go
                    let total = rem - 2;
                    let mut got = 0usize;
                    while got < total {
                        let r = self
                            .t
                            .read(&mut self.rx[got..total])
                            .await
                            .map_err(|_| Error::Io)?;
                        if r == 0 {
                            return Err(Error::Io);
                        }
                        got += r;
                    }
                    self.topic_len = topic_len;
                    self.payload_len = total - topic_len;
                    Ok(Event::Publish)
                }
                0x90 => {
                    self.drain(rem).await?;
                    Ok(Event::Pingresp) // SUBACK: ignored (QoS0)
                }
                0xD0 => Ok(Event::Pingresp),
                0xE0 => Ok(Event::Disconnect),
                _ => {
                    self.drain(rem).await?;
                    Ok(Event::Pingresp) // unknown packet: ignore
                }
            }
        }

        async fn drain(&mut self, mut n: usize) -> Result<(), Error> {
            while n > 0 {
                let chunk = n.min(self.rx.len());
                let mut got = 0usize;
                while got < chunk {
                    let r = self
                        .t
                        .read(&mut self.rx[got..chunk])
                        .await
                        .map_err(|_| Error::Io)?;
                    if r == 0 {
                        return Err(Error::Io);
                    }
                    got += r;
                }
                n -= chunk;
            }
            Ok(())
        }

        pub fn last_topic_matches(&self, topic: &str) -> bool {
            self.topic_len == topic.len()
                && &self.rx[..self.topic_len] == topic.as_bytes()
        }

        /// Payload slice of the last Event::Publish (after the topic).
        pub fn last_payload(&self) -> &[u8] {
            &self.rx[self.topic_len..self.topic_len + self.payload_len]
        }
    }
}

enum Ctl {
    Stop,
    CfgChanged,
    None,
}

/// Wait for a stop request or a cfg change that forces a reconnect
/// (mqtt.* / device.hostname identity; everything else is ignored).
async fn select3_cfg(changed: &mut cfg::ChangedSub) -> Ctl {
    match embassy_futures::select::select(KICK.wait(), changed.next_message_pure()).await {
        embassy_futures::select::Either::First(()) => Ctl::Stop,
        embassy_futures::select::Either::Second(key) => {
            if key.starts_with("mqtt.") || key == "device.hostname" {
                Ctl::CfgChanged
            } else {
                Ctl::None
            }
        }
    }
}

/// MQTT service task. Boot state: running = mqtt.auto && mqtt.enable.
#[allow(
    clippy::large_stack_frames,
    reason = "async fn futures live in the embassy static task pool, not on a call stack"
)]
pub async fn task(stack: Stack<'static>, bufs: Buffers<'static>) -> ! {
    let mut changed = cfg::changed().expect("cfg change subscriber");
    {
        let auto = cfg::get_str("mqtt.auto").await == "1";
        let enable = cfg::get_str("mqtt.enable").await == "1";
        set_state(|s| s.running = auto && enable);
    }

    loop {
        if !status().1 {
            // Idle until start() or mqtt.enable flips to 1. Other cfg
            // changes must NOT re-arm a manual disconnect (Arduino parity:
            // enable is a master switch, not an any-change trigger).
            match embassy_futures::select::select(
                KICK.wait(),
                changed.next_message_pure(),
            )
            .await
            {
                embassy_futures::select::Either::First(()) => {}
                embassy_futures::select::Either::Second(key) => {
                    if key == "mqtt.enable" && cfg::get_str("mqtt.enable").await == "1" {
                        set_state(|s| s.running = true);
                    }
                }
            }
            continue;
        }

        if !wifi::link_up() {
            Timer::after(WIFI_WAIT).await;
            continue;
        }

        let host = cfg::get_str("mqtt.broker").await;
        if host.is_empty() {
            warn!("mqtt: mqtt.broker unset, not starting");
            set_state(|s| s.running = false);
            continue;
        }
        let port: u16 = cfg::get_str("mqtt.port")
            .await
            .parse()
            .unwrap_or(8883);
        let user = cfg::get_str("mqtt.user").await;
        let pass = cfg::get_str("mqtt.pass").await;
        let hostname = cfg::get_str("device.hostname").await;
        let mut name: heapless::String<80> = heapless::String::new();
        let _ = write!(name, "atnode-{hostname}"); // client_id (Arduino)
        let topics = Topics::new(hostname.as_str());

        info!("mqtt: connecting to {host}:{port}");
        run_session(
            stack,
            Buffers {
                tls_rx: &mut *bufs.tls_rx,
                tls_tx: &mut *bufs.tls_tx,
                mqtt_tx: &mut *bufs.mqtt_tx,
                mqtt_rx: &mut *bufs.mqtt_rx,
                tcp_rx: &mut *bufs.tcp_rx,
                tcp_tx: &mut *bufs.tcp_tx,
            },
            host.as_str(),
            port,
            user.as_str(),
            pass.as_str(),
            name.as_str(),
            &topics,
            &mut changed,
        )
        .await;
        set_state(|s| s.connected = false);

        if status().1 {
            Timer::after(RECONNECT_BACKOFF).await;
        }
    }
}
