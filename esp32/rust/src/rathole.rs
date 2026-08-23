//! AT-Node rust-s3 — rathole reverse-tunnel client (protocol v1, plain TCP).
//!
//! Port of esp32/arduino/rathole_client.cpp (see esp32/arduino/RATHOLE.md
//! for the full architecture and field notes). Same scope cuts: plain TCP
//! transport only (no TLS/noise — tunnel only self-encrypting protocols),
//! TCP forwarding only (no UDP), single tunnel (id = 1).
//!
//! `tunnel.1.local` MUST be another LAN host (e.g. 192.168.1.10:22 to
//! expose a LAN machine's SSH). Forwarding to the DEVICE ITSELF is not
//! possible on this firmware: smoltcp has no host loopback, so neither
//! 127.0.0.1 nor the device's own LAN IP can be connected to. (The
//! Arduino variant's lwIP has no such restriction.)
//!
//! Wire format (bincode, little-endian, fixed-size ints; enum = u32 variant):
//!   Hello::ControlChannelHello = u32(0) + u8(version=1) + sha256(service)[32]  (37B)
//!   Hello::DataChannelHello    = u32(1) + u8(version=1) + session_key[32]      (37B)
//!   Auth                       = sha256(token || nonce)[32]                    (32B)
//!   Ack                        = u32: 0=Ok 1=ServiceNotExist 2=AuthFailed       (4B)
//!   ControlChannelCmd          = u32: 0=CreateDataChannel 1=HeartBeat           (4B)
//!   DataChannelCmd             = u32: 0=StartForwardTcp 1=StartForwardUdp       (4B)
//!
//! Concurrency discipline (Arduino R3, non-negotiable): the manager task is
//! the ONLY owner of the control channel and the standby pool. Every other
//! context (AT/HTTP/MQTT) only flips WANT_RUN / RECONFIG atomics and pokes
//! KICK — it never touches a socket. Config changes arrive through the cfg
//! pubsub watcher, which applies the Arduino rules:
//!   rathole.enable=0  -> stop;   =1 -> start if enabled && auto && configured
//!   tunnel.1.enable=0 -> stop;   =1 -> start if configured
//!   other tunnel.1.*  -> reconnect with the new config
//!
//! embassy-net/smoltccp note: there is no Nagle to disable (sends are
//! immediate), so the Arduino R6 setNoDelay fix is inherent here.
//!
//! Cargo feature `rathole` (default on): comm-plane feature. Built without
//! it, every channel reports "tunnel disabled" and ability says rathole:false.

use core::sync::atomic::{AtomicBool, AtomicU8, Ordering};

use heapless::String;

use crate::cfg;

pub fn enabled() -> bool {
    cfg!(feature = "rathole")
}

// ------------------------------------------------------------- runtime ---

/// Desired run state (connect/disconnect/master/enable rules).
static WANT_RUN: AtomicBool = AtomicBool::new(false);
/// Config changed while running: tear down and reconnect.
static RECONFIG: AtomicBool = AtomicBool::new(false);
/// Wakes the manager from backoff and breaks the connected select loop.
static KICK: embassy_sync::signal::Signal<
    embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex,
    (),
> = embassy_sync::signal::Signal::new();

static CONNECTED: AtomicBool = AtomicBool::new(false);
static POOLED: AtomicBool = AtomicBool::new(false);
static DATA_CH: AtomicU8 = AtomicU8::new(0);

static LAST_ERROR: critical_section::Mutex<core::cell::RefCell<String<64>>> =
    critical_section::Mutex::new(core::cell::RefCell::new(String::new()));

#[cfg(feature = "rathole")]
fn set_err(e: &str) {
    critical_section::with(|cs| {
        let mut b = LAST_ERROR.borrow(cs).borrow_mut();
        b.clear();
        let _ = b.push_str(&e[..e.len().min(63)]);
    });
}

/// Runtime start request (AT/HTTP/MQTT "connect"). Mirrors
/// rathole_start(): refused while the master/per-tunnel switch is off or
/// the tunnel is unconfigured.
pub async fn connect() -> bool {
    if !enabled() || !master_on().await || !tunnel_enabled().await || !configured().await {
        return false;
    }
    WANT_RUN.store(true, Ordering::Relaxed);
    KICK.signal(());
    true
}

/// Runtime stop ("disconnect"). Flags only — the manager tears its own
/// sockets down (see the discipline note in the module header).
pub fn disconnect() {
    WANT_RUN.store(false, Ordering::Relaxed);
    RECONFIG.store(true, Ordering::Relaxed);
    KICK.signal(());
}

/// Stop + reset all tunnel keys to defaults (Arduino rathole_clear).
pub async fn clear() {
    disconnect();
    for (key, val) in [
        ("tunnel.1.server", ""),
        ("tunnel.1.token", ""),
        ("tunnel.1.service", ""),
        ("tunnel.1.local", ""),
        ("tunnel.1.auto", "0"),
        ("tunnel.1.retry", "1"),
        ("tunnel.1.enable", "1"),
    ] {
        let _ = cfg::set(key, val).await;
    }
}

async fn master_on() -> bool {
    cfg::get_str("rathole.enable").await.as_str() == "1"
}

async fn tunnel_enabled() -> bool {
    cfg::get_str("tunnel.1.enable").await.as_str() == "1"
}

async fn configured() -> bool {
    !cfg::get_str("tunnel.1.server").await.is_empty()
        && !cfg::get_str("tunnel.1.token").await.is_empty()
        && !cfg::get_str("tunnel.1.service").await.is_empty()
        && !cfg::get_str("tunnel.1.local").await.is_empty()
}

/// Boot-time decision (master && enabled && auto && configured), then the
/// manager task evaluates changes itself. Call once from main.
#[cfg(feature = "rathole")]
pub async fn init() {
    let run = master_on().await
        && tunnel_enabled().await
        && cfg::get_str("tunnel.1.auto").await.as_str() == "1"
        && configured().await;
    WANT_RUN.store(run, Ordering::Relaxed);
}

/// Per-tunnel status object (Arduino rathole_status_json fields).
pub async fn status_json() -> String<768> {
    let server = cfg::get_str("tunnel.1.server").await;
    let service = cfg::get_str("tunnel.1.service").await;
    let local = cfg::get_str("tunnel.1.local").await;
    let auto = cfg::get_str("tunnel.1.auto").await.as_str() == "1";
    let retry = cfg::get_str("tunnel.1.retry").await;
    let last_error = critical_section::with(|cs| LAST_ERROR.borrow(cs).borrow().clone());
    let mut j: String<768> = String::new();
    let _ = core::fmt::Write::write_fmt(
        &mut j,
        format_args!(
            "{{\"id\":1,\"configured\":{},\"server\":\"{server}\",\"service\":\"{service}\",\
\"local\":\"{local}\",\"auto\":{auto},\"retry\":{retry},\"master\":{},\"enabled\":{},\
\"running\":{},\"connected\":{},\"pool\":{},\"data_channels\":{},\"free_heap\":{},\
\"last_error\":\"{last_error}\"}}",
            if configured().await { "true" } else { "false" },
            if master_on().await { "true" } else { "false" },
            if tunnel_enabled().await { "true" } else { "false" },
            if WANT_RUN.load(Ordering::Relaxed) {
                "true"
            } else {
                "false"
            },
            if CONNECTED.load(Ordering::Relaxed) {
                "true"
            } else {
                "false"
            },
            if POOLED.load(Ordering::Relaxed) { 1 } else { 0 },
            DATA_CH.load(Ordering::Relaxed),
            esp_alloc::HEAP.free(),
        ),
    );
    j
}

// ------------------------------------------------ cfg change watcher -----

/// Applies the Arduino enable/master rules to WANT_RUN whenever the
/// tunnel keys change, then kicks the manager. Own task (spawned once).
#[cfg(feature = "rathole")]
pub async fn watch_task() -> ! {
    let mut sub = cfg::changed().expect("cfg sub");
    loop {
        let key = sub.next_message_pure().await;
        match key {
            "rathole.enable" => {
                if !master_on().await {
                    WANT_RUN.store(false, Ordering::Relaxed);
                } else if tunnel_enabled().await
                    && cfg::get_str("tunnel.1.auto").await.as_str() == "1"
                    && configured().await
                {
                    WANT_RUN.store(true, Ordering::Relaxed);
                }
                RECONFIG.store(true, Ordering::Relaxed);
                KICK.signal(());
            }
            "tunnel.1.enable" => {
                if !tunnel_enabled().await {
                    WANT_RUN.store(false, Ordering::Relaxed);
                } else if configured().await {
                    WANT_RUN.store(true, Ordering::Relaxed);
                }
                RECONFIG.store(true, Ordering::Relaxed);
                KICK.signal(());
            }
            k if k.starts_with("tunnel.1.") => {
                RECONFIG.store(true, Ordering::Relaxed);
                KICK.signal(());
            }
            _ => {}
        }
    }
}

// ---------------------------------------------------- driver (gated) -----

#[cfg(feature = "rathole")]
pub use driver::{forward_loop, task};

#[cfg(feature = "rathole")]
mod driver {
    use super::*;
    use core::future::pending;
    use core::ptr::addr_of_mut;
    use core::sync::atomic::AtomicUsize;

    use embassy_futures::select::{Either, Either4, select, select4};
    use embassy_net::IpAddress;
    use embassy_net::dns::DnsQueryType;
    use embassy_net::tcp::TcpSocket;
    use embassy_net::{IpEndpoint, Ipv4Address, Stack};
    use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
    use embassy_sync::channel::Channel;
    use embassy_time::{Duration, Instant, Timer};
    use embedded_io_async_07::Write as _; // write_all (read is inherent)
    use log::{info, warn};
    use sha2::{Digest, Sha256};

    const HELLO_LEN: usize = 37;
    const PROTO_VERSION: u8 = 1;
    /// Server heartbeat interval 30s / timeout 40s; client margin (Arduino).
    const HEARTBEAT: Duration = Duration::from_secs(45);
    const HANDSHAKE_TIMEOUT: Duration = Duration::from_secs(10);
    /// Heap guard (Arduino R4): reconnect churn piles lwIP sockets in
    /// TIME_WAIT (~120 s); refuse new sockets when tight and let it drain.
    const MIN_FREE_HEAP: usize = 12000;
    const MAX_FWD: usize = 2;
    const PUMP_BUF: usize = 1460;
    const SOCK_BUF: usize = 1536;

    // ------------------------------------------------------ protocol -----

    fn sha256(data: &[u8]) -> [u8; 32] {
        let mut h = Sha256::new();
        h.update(data);
        h.finalize().into()
    }

    /// hello: u32 variant + u8 version + 32B digest.
    fn build_hello(out: &mut [u8; HELLO_LEN], variant: u32, digest: &[u8; 32]) {
        out[..4].copy_from_slice(&variant.to_le_bytes());
        out[4] = PROTO_VERSION;
        out[5..].copy_from_slice(digest);
    }

    fn le_u32(b: &[u8]) -> u32 {
        u32::from_le_bytes([b[0], b[1], b[2], b[3]])
    }

    // ------------------------------------------------------ helpers ------

    /// Split "host:port" (Arduino split_host_port).
    fn split_host_port(s: &str, host: &mut String<64>) -> Option<u16> {
        let colon = s.rfind(':')?;
        if colon == 0 || colon == s.len() - 1 {
            return None;
        }
        let port: u16 = s[colon + 1..].parse().ok()?;
        if port == 0 || host.push_str(&s[..colon]).is_err() {
            return None;
        }
        Some(port)
    }

    async fn resolve(stack: Stack<'static>, host: &str) -> Option<Ipv4Address> {
        if let Ok(v4) = host.parse::<Ipv4Address>() {
            return Some(v4);
        }
        match stack.dns_query(host, DnsQueryType::A).await {
            Ok(addrs) => addrs.first().map(|a| match a {
                IpAddress::Ipv4(v4) => *v4,
            }),
            Err(_) => None,
        }
    }

    async fn read_exact(
        s: &mut TcpSocket<'_>,
        buf: &mut [u8],
        timeout: Duration,
    ) -> Result<(), ()> {
        let deadline = Instant::now() + timeout;
        let mut got = 0;
        while got < buf.len() {
            match select(s.read(&mut buf[got..]), Timer::at(deadline)).await {
                Either::First(Ok(0)) => return Err(()),
                Either::First(Ok(n)) => got += n,
                Either::First(Err(_)) => return Err(()),
                Either::Second(()) => return Err(()),
            }
        }
        Ok(())
    }

    // ------------------------------------------- data-channel slot pool --
    // Data-channel sockets cross tasks (manager -> forward task), so their
    // buffers are statics recycled through a slot bitmask (httpd FREE_IDX
    // pattern). One live socket per slot, ever.

    static mut DCH_RX: [[u8; SOCK_BUF]; MAX_FWD] = [[0; SOCK_BUF]; MAX_FWD];
    static mut DCH_TX: [[u8; SOCK_BUF]; MAX_FWD] = [[0; SOCK_BUF]; MAX_FWD];
    static SLOTS: AtomicUsize = AtomicUsize::new(0);

    fn slot_alloc() -> Option<usize> {
        for i in 0..MAX_FWD {
            let mask = 1 << i;
            if SLOTS.fetch_or(mask, Ordering::Relaxed) & mask == 0 {
                return Some(i);
            }
        }
        None
    }

    fn slot_free(i: usize) {
        SLOTS.fetch_and(!(1 << i), Ordering::Relaxed);
    }

    /// embassy-net sockets are !Send (smoltcp per-socket state), but every
    /// task runs on ONE cooperative executor on core 0 — nothing ever
    /// crosses a real thread boundary (httpd ConnSocket precedent).
    struct ConnSocket(TcpSocket<'static>);

    unsafe impl Send for ConnSocket {}

    struct FwdReq {
        sock: ConnSocket,
        slot: usize,
        lhost: String<64>,
        lport: u16,
    }

    static FWD_CH: Channel<CriticalSectionRawMutex, FwdReq, MAX_FWD> = Channel::new();

    // -------------------------------------------------------- manager ----

    struct TunCfg {
        server: String<64>,
        token: String<64>,
        service: String<64>,
        local: String<64>,
        retry_s: u8,
    }

    async fn load_cfg() -> TunCfg {
        TunCfg {
            server: cfg::get_str("tunnel.1.server").await,
            token: cfg::get_str("tunnel.1.token").await,
            service: cfg::get_str("tunnel.1.service").await,
            local: cfg::get_str("tunnel.1.local").await,
            retry_s: cfg::get_str("tunnel.1.retry")
                .await
                .parse()
                .unwrap_or(1),
        }
    }

    /// One connection attempt: TCP connect + rathole control handshake.
    /// On success returns the control socket and the session key (which
    /// doubles as the data-channel hello digest).
    async fn control_handshake(
        stack: Stack<'static>,
        cfg: &TunCfg,
    ) -> Result<(TcpSocket<'static>, [u8; 32]), &'static str> {
        let mut host: String<64> = String::new();
        let Some(port) = split_host_port(&cfg.server, &mut host) else {
            return Err("bad server addr");
        };
        let Some(ip) = resolve(stack, &host).await else {
            return Err("dns resolve failed");
        };
        // Control-socket buffers live in the manager future; the socket
        // never leaves the manager task.
        let rx = unsafe { &mut *addr_of_mut!(CTL_RX) };
        let tx = unsafe { &mut *addr_of_mut!(CTL_TX) };
        let mut sock = TcpSocket::new(stack, rx, tx);
        sock.set_timeout(Some(HANDSHAKE_TIMEOUT));
        if sock
            .connect(IpEndpoint::new(IpAddress::Ipv4(ip), port))
            .await
            .is_err()
        {
            return Err("tcp connect failed");
        }
        // embassy-net closes the socket when its timeout fires; keep it
        // as a connect-time bound only — the connected phase has its own
        // application-level deadlines (read_exact / 45 s heartbeat).
        sock.set_timeout(None);

        // Hello: ControlChannelHello(version, sha256(service name))
        let digest = sha256(cfg.service.as_bytes());
        let mut hello = [0u8; HELLO_LEN];
        build_hello(&mut hello, 0, &digest);
        if sock.write_all(&hello).await.is_err() {
            return Err("write hello failed");
        }

        // Server hello carries a random nonce at [5..37].
        let mut sh = [0u8; HELLO_LEN];
        if read_exact(&mut sock, &mut sh, HANDSHAKE_TIMEOUT)
            .await
            .is_err()
        {
            return Err("read hello failed");
        }
        if le_u32(&sh[..4]) != 0 || sh[4] != PROTO_VERSION {
            return Err("bad server hello");
        }

        // Auth: sha256(token bytes || nonce bytes) — also the data-channel
        // session key.
        let mut auth_input: heapless::Vec<u8, 96> = heapless::Vec::new();
        auth_input
            .extend_from_slice(cfg.token.as_bytes())
            .map_err(|_| "token too long")?;
        auth_input
            .extend_from_slice(&sh[5..])
            .map_err(|_| "token too long")?;
        let session_key = sha256(&auth_input);
        if sock.write_all(&session_key).await.is_err() {
            return Err("write auth failed");
        }

        let mut ack = [0u8; 4];
        if read_exact(&mut sock, &mut ack, HANDSHAKE_TIMEOUT)
            .await
            .is_err()
        {
            return Err("read ack failed");
        }
        match le_u32(&ack) {
            0 => Ok((sock, session_key)),
            1 => Err("service not exist"),
            _ => Err("auth failed"),
        }
    }

    static mut CTL_RX: [u8; SOCK_BUF] = [0; SOCK_BUF];
    static mut CTL_TX: [u8; SOCK_BUF] = [0; SOCK_BUF];

    /// Connect one standby data channel (DataChannelHello(session_key)).
    /// The socket takes a static slot so it can move to a forward task.
    async fn pool_fill_one(
        stack: Stack<'static>,
        server: &str,
        session_key: &[u8; 32],
    ) -> Option<(ConnSocket, usize)> {
        if esp_alloc::HEAP.free() < MIN_FREE_HEAP {
            return None; // let TIME_WAITs drain (Arduino R4)
        }
        let slot = slot_alloc()?;
        let mut host: String<64> = String::new();
        let port = split_host_port(server, &mut host)?;
        let ip = resolve(stack, &host).await?;
        let rx = unsafe { &mut *addr_of_mut!(DCH_RX[slot]) };
        let tx = unsafe { &mut *addr_of_mut!(DCH_TX[slot]) };
        let mut sock = TcpSocket::new(stack, rx, tx);
        sock.set_timeout(Some(HANDSHAKE_TIMEOUT));
        if sock
            .connect(IpEndpoint::new(IpAddress::Ipv4(ip), port))
            .await
            .is_err()
        {
            slot_free(slot);
            return None;
        }
        sock.set_timeout(None); // see control_handshake: timeout kills idle standbys
        let mut hello = [0u8; HELLO_LEN];
        build_hello(&mut hello, 1, session_key);
        if sock.write_all(&hello).await.is_err() {
            slot_free(slot);
            return None;
        }
        POOLED.store(true, Ordering::Relaxed);
        DATA_CH.fetch_add(1, Ordering::Relaxed);
        Some((ConnSocket(sock), slot))
    }

    /// Manager: reconnect loop. Owns the control channel and the standby
    /// pool for their entire lifetime.
    pub async fn task(stack: Stack<'static>) -> ! {
        let mut backoff_ms: u32 = 0;
        loop {
            if !WANT_RUN.load(Ordering::Relaxed) {
                backoff_ms = 0;
                // Park: wake on KICK (connect request) or poll slowly.
                let _ = select(KICK.wait(), Timer::after(Duration::from_millis(500))).await;
                continue;
            }
            if !crate::wifi::link_up() {
                let _ = select(KICK.wait(), Timer::after(Duration::from_secs(1))).await;
                continue;
            }
            if esp_alloc::HEAP.free() < MIN_FREE_HEAP {
                set_err("low heap, draining");
                let _ = select(KICK.wait(), Timer::after(Duration::from_secs(2))).await;
                continue;
            }
            if !configured().await {
                set_err("not configured");
                let _ = select(KICK.wait(), Timer::after(Duration::from_secs(2))).await;
                continue;
            }

            let tcfg = load_cfg().await; // snapshot for the whole connect cycle
            let base_ms = backoff_ms.max(u32::from(tcfg.retry_s) * 1000);
            RECONFIG.store(false, Ordering::Relaxed);

            match control_handshake(stack, &tcfg).await {
                Err(e) => {
                    set_err(e);
                    warn!("rathole: handshake failed: {e}");
                }
                Ok((mut cli, session_key)) => {
                    CONNECTED.store(true, Ordering::Relaxed);
                    set_err("");
                    info!(
                        "rathole: control channel up ({} -> {})",
                        tcfg.service, tcfg.local
                    );
                    let up_since = Instant::now();

                    run_connected(stack, &mut cli, &session_key, &tcfg).await;

                    CONNECTED.store(false, Ordering::Relaxed);
                    POOLED.store(false, Ordering::Relaxed);
                    info!("rathole: control channel down");
                    if up_since.elapsed() > Duration::from_secs(3) {
                        backoff_ms = 0; // a healthy session resets the backoff
                    }
                }
            }

            // Sliced backoff: stop/reconfig takes effect within ~100ms
            // instead of waiting out a backoff that can reach 30s.
            backoff_ms = (if backoff_ms == 0 {
                base_ms
            } else {
                backoff_ms * 2
            })
            .min(30_000);
            let mut waited = 0;
            while waited < backoff_ms {
                if matches!(
                    select(KICK.wait(), Timer::after(Duration::from_millis(100))).await,
                    Either::First(())
                ) {
                    break;
                }
                waited += 100;
            }
        }
    }

    /// Connected phase: multiplex control cmds, standby-pool activation,
    /// heartbeat timeout and KICK in one select loop.
    async fn run_connected(
        stack: Stack<'static>,
        cli: &mut TcpSocket<'static>,
        session_key: &[u8; 32],
        tcfg: &TunCfg,
    ) {
        let mut pool: Option<(ConnSocket, usize)> = None;
        let mut cmd = [0u8; 4];
        let mut clen = 0usize;
        let mut dcmd = [0u8; 4];
        let mut dlen = 0usize;
        let mut last_rx = Instant::now();

        loop {
            if !WANT_RUN.load(Ordering::Relaxed) || RECONFIG.load(Ordering::Relaxed) {
                set_err("");
                break;
            }
            let remain = HEARTBEAT
                .checked_sub(last_rx.elapsed())
                .unwrap_or(Duration::from_ticks(0));
            let pool_read = async {
                match pool.as_mut() {
                    Some(s) => Some(s.0.0.read(&mut dcmd[dlen..]).await),
                    None => pending().await,
                }
            };
            match select4(
                cli.read(&mut cmd[clen..]),
                pool_read,
                Timer::after(remain),
                KICK.wait(),
            )
            .await
            {
                Either4::First(Ok(0)) => {
                    set_err("control channel closed");
                    break;
                }
                Either4::First(Ok(n)) => {
                    clen += n;
                    last_rx = Instant::now();
                    if clen == 4 {
                        clen = 0;
                        if le_u32(&cmd) == 0 && pool.is_none() {
                            // CreateDataChannel (pool full: request dropped,
                            // the server retries when a visitor arrives).
                            pool = pool_fill_one(stack, &tcfg.server, session_key).await;
                        }
                        // le_u32 == 1: HeartBeat — last_rx already bumped.
                    }
                }
                Either4::First(Err(_)) => {
                    set_err("control channel error");
                    break;
                }
                Either4::Second(Some(Ok(0))) => {
                    // Server culled this standby.
                    if let Some((_, slot)) = pool.take() {
                        slot_free(slot);
                        POOLED.store(false, Ordering::Relaxed);
                        DATA_CH.fetch_sub(1, Ordering::Relaxed);
                    }
                }
                Either4::Second(Some(Ok(n))) => {
                    dlen += n;
                    if dlen == 4 {
                        dlen = 0;
                        if le_u32(&dcmd) == 0 {
                            // StartForwardTcp: ownership moves to a forward
                            // task; the server retries if no task is free.
                            let (sock, slot) = pool.take().expect("pool socket");
                            POOLED.store(false, Ordering::Relaxed);
                            start_forward(sock, slot, tcfg);
                        } else {
                            // Desync (UDP forward or garbage): drop it.
                            if let Some((_, slot)) = pool.take() {
                                warn!("rathole: standby dropped (bad dcmd)");
                                slot_free(slot);
                                POOLED.store(false, Ordering::Relaxed);
                                DATA_CH.fetch_sub(1, Ordering::Relaxed);
                            }
                        }
                    }
                }
                Either4::Second(Some(Err(_))) => {
                    if let Some((_, slot)) = pool.take() {
                        slot_free(slot);
                        POOLED.store(false, Ordering::Relaxed);
                        DATA_CH.fetch_sub(1, Ordering::Relaxed);
                    }
                }
                Either4::Second(None) => unreachable!(),
                Either4::Third(()) => {
                    set_err("heartbeat timeout");
                    break;
                }
                Either4::Fourth(()) => continue, // KICK: re-check flags
            }
        }
        if let Some((_, slot)) = pool.take() {
            slot_free(slot);
            POOLED.store(false, Ordering::Relaxed);
            DATA_CH.fetch_sub(1, Ordering::Relaxed);
        }
    }

    /// Hand an activated data channel to a forward task (Arduino
    /// start_forward: refuse when tight — the server retries the visitor).
    /// On success the slot ownership moves to the forward task, which
    /// frees it at session end.
    fn start_forward(sock: ConnSocket, slot: usize, tcfg: &TunCfg) {
        let mut lhost: String<64> = String::new();
        let lport = if esp_alloc::HEAP.free() >= MIN_FREE_HEAP {
            split_host_port(&tcfg.local, &mut lhost)
        } else {
            None
        };
        match lport {
            Some(lport) => {
                if FWD_CH
                    .try_send(FwdReq {
                        sock,
                        slot,
                        lhost,
                        lport,
                    })
                    .is_err()
                {
                    // Queue full: the dropped FwdReq closes the socket;
                    // the server retries the visitor.
                    slot_free(slot);
                    DATA_CH.fetch_sub(1, Ordering::Relaxed);
                    warn!("rathole: fwd queue full, visitor dropped");
                }
            }
            None => {
                drop(sock);
                slot_free(slot);
                DATA_CH.fetch_sub(1, Ordering::Relaxed);
            }
        }
    }

    /// Forward task body: one visitor session at a time per task instance.
    pub async fn forward_loop(stack: Stack<'static>) -> ! {
        let mut lrx = [0u8; 1024];
        let mut ltx = [0u8; 1024];
        loop {
            let FwdReq {
                sock: ConnSocket(mut remote),
                slot,
                lhost,
                lport,
            } = FWD_CH.receive().await;
            let Some(ip) = resolve(stack, &lhost).await else {
                warn!("rathole: fwd local resolve failed");
                slot_free(slot);
                DATA_CH.fetch_sub(1, Ordering::Relaxed);
                continue;
            };
            let mut local = TcpSocket::new(stack, &mut lrx, &mut ltx);
            local.set_timeout(Some(Duration::from_secs(10)));
            if local
                .connect(IpEndpoint::new(IpAddress::Ipv4(ip), lport))
                .await
                .is_err()
            {
                warn!("rathole: fwd local connect failed");
                slot_free(slot);
                DATA_CH.fetch_sub(1, Ordering::Relaxed);
                continue;
            }
            local.set_timeout(None); // connect-time bound only (socket would close)

            // Bidirectional pump with proper half-close: when one side
            // sends FIN, flush + close the other direction and keep
            // pumping it until its own EOF — dropping the socket right
            // away would discard whatever is still queued in its tx
            // buffer (observed: HTTP body truncated by ~843 B).
            let mut rb = [0u8; PUMP_BUF];
            let mut lb = [0u8; PUMP_BUF];
            let mut remote_open = true;
            let mut local_open = true;
            loop {
                if !remote_open && !local_open {
                    break;
                }
                let r = async {
                    if remote_open {
                        remote.read(&mut rb).await
                    } else {
                        pending().await
                    }
                };
                let l = async {
                    if local_open {
                        local.read(&mut lb).await
                    } else {
                        pending().await
                    }
                };
                match select(r, l).await {
                    Either::First(Ok(0)) => {
                        remote_open = false;
                        let _ = local.flush().await;
                        local.close();
                    }
                    Either::First(Ok(n)) => {
                        if local.write_all(&rb[..n]).await.is_err() {
                            break;
                        }
                    }
                    Either::First(Err(_)) => break,
                    Either::Second(Ok(0)) => {
                        local_open = false;
                        let _ = remote.flush().await;
                        remote.close();
                    }
                    Either::Second(Ok(n)) => {
                        if remote.write_all(&lb[..n]).await.is_err() {
                            break;
                        }
                    }
                    Either::Second(Err(_)) => break,
                }
            }
            slot_free(slot);
            DATA_CH.fetch_sub(1, Ordering::Relaxed);
        }
    }
}
