//! AT-Node rust-s3 — BLE HID keyboard backend (kbd-ble feature, R6).
//!
//! esp-radio ble (precompiled Bluedroid-derived controller) + trouble-host
//! 0.6 (bt-hci 0.8 — matches esp-radio 0.18; 0.4 pins bt-hci 0.6 and is
//! incompatible). The kb engine owns all key timing; this backend only
//! applies raw 8-byte boot reports as HID input report notifications.
//!
//! R6.0 scope: controller bring-up + minimal advertising (device name
//! discoverable via bluetoothctl). GATT HID lands in R6.1, kb routing in
//! R6.2, pairing window + NVS bonds in R6.3.
//!
//! Memory discipline (README layout): HostResources connection/channel
//! storage stays in internal RAM via StaticCell; the trouble packet pool
//! (DefaultPacketPool statics) is small. Heap watermarks are printed at
//! bring-up as the R6 baseline.

use core::fmt::Write as _;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use embassy_executor::Spawner;
use embassy_futures::select::{Either, Either3, select, select3};
use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::channel::Channel;
use embassy_time::{Duration, Instant, Timer};
use esp_hal::peripherals::BT;
use esp_radio::ble::controller::BleConnector;
use static_cell::StaticCell;
use trouble_host::prelude::*;

use crate::cfg;
use crate::kb::Report;

// ------------------------------------------------------------ backend ---

static REPORTS: Channel<CriticalSectionRawMutex, Report, 8> = Channel::new();
static CONNECTED: AtomicBool = AtomicBool::new(false);

/// Queue a raw report (kb engine only). CH582 semantics: with no BLE
/// connection the report is dropped — the engine never blocks on a dead
/// link.
pub async fn send_report(r: Report) {
    if CONNECTED.load(Ordering::Relaxed) {
        REPORTS.send(r).await;
    }
}

/// BLE backend state for AT+BLE=status (R6.3).
pub fn connected() -> bool {
    CONNECTED.load(Ordering::Relaxed)
}
// ------------------------------------------- pairing window + bond ----

/// AT+PAIR opens this many seconds of pairable time (R6.3; Arduino
/// pairing-window parity). Uptime seconds; 0 = closed.
// xtensa has no 64-bit atomics; uptime seconds fit u32 for ~136 years.
static PAIR_UNTIL: AtomicU32 = AtomicU32::new(0);

/// RAM mirror of the persisted bond's peer address (HCI byte order).
/// CH582 single-bond semantics: a new pairing overwrites the old one.
static BONDED_ADDR: critical_section::Mutex<core::cell::RefCell<Option<[u8; 6]>>> =
    critical_section::Mutex::new(core::cell::RefCell::new(None));

pub const PAIR_WINDOW_SECS: u64 = 60;

/// AT+PAIR: open the pairing window (Just Works while open).
pub fn pair_open() {
    PAIR_UNTIL.store(
        Instant::now().as_secs() as u32 + PAIR_WINDOW_SECS as u32,
        Ordering::Relaxed,
    );
}

/// Window closes on successful pairing or AT+BLE=clear.
pub fn pair_close() {
    PAIR_UNTIL.store(0, Ordering::Relaxed);
}

pub fn pair_window_open() -> bool {
    let t = PAIR_UNTIL.load(Ordering::Relaxed);
    t != 0 && (Instant::now().as_secs() as u32) < t
}

/// Bonded peer address (HCI order) when a bond exists.
pub fn bond_addr() -> Option<[u8; 6]> {
    critical_section::with(|cs| *BONDED_ADDR.borrow(cs).borrow())
}
/// Our own BLE address in display order (same efuse-derived static
/// random address init() programs into the controller).
#[cfg_attr(not(any(feature = "http", feature = "mqtt")), allow(dead_code))]
pub fn our_addr_display() -> heapless::String<17> {
    let mac = esp_hal::efuse::base_mac_address();
    let b = mac.as_bytes();
    let mut s = heapless::String::new();
    let _ = write!(
        s,
        "{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
        b[0] | 0xC0,
        b[1],
        b[2],
        b[3],
        b[4],
        b[5]
    );
    s
}

/// Human-readable bonded peer address ("AA:BB:CC:DD:EE:FF") for the
/// AT/HTTP/MQTT surfaces.
#[cfg_attr(not(any(feature = "http", feature = "mqtt")), allow(dead_code))]
pub fn bond_addr_display() -> Option<heapless::String<17>> {
    let a = bond_addr()?;
    let mut s = heapless::String::new();
    let _ = write!(
        s,
        "{:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
        a[5], a[4], a[3], a[2], a[1], a[0]
    );
    Some(s)
}

fn set_bond_addr(addr: Option<[u8; 6]>) {
    critical_section::with(|cs| *BONDED_ADDR.borrow(cs).borrow_mut() = addr);
}

/// Bond serialization layout (40 bytes, hex-encoded to 80 chars in
/// cfg `ble.bond`): LTK(16 LE) | bd_addr(6) | has_irk(1) | IRK(16 LE) |
/// security_level(1: 0/1/2 = NoEncryption/Encrypted/EncryptedAuthenticated).
const BOND_LEN: usize = 40;

fn bond_encode(b: &BondInformation) -> heapless::String<96> {
    let mut raw = [0u8; BOND_LEN];
    raw[..16].copy_from_slice(&b.ltk.0.to_le_bytes());
    raw[16..22].copy_from_slice(b.identity.bd_addr.raw());
    if let Some(irk) = &b.identity.irk {
        raw[22] = 1;
        raw[23..39].copy_from_slice(&irk.0.to_le_bytes());
    }
    raw[39] = match b.security_level {
        SecurityLevel::NoEncryption => 0,
        SecurityLevel::Encrypted => 1,
        SecurityLevel::EncryptedAuthenticated => 2,
    };
    let mut out = heapless::String::new();
    for byte in raw {
        let _ = write!(out, "{byte:02x}");
    }
    out
}

fn bond_decode(s: &str) -> Option<BondInformation> {
    let mut raw = [0u8; BOND_LEN];
    if s.len() != BOND_LEN * 2 {
        return None;
    }
    for (i, chunk) in s.as_bytes().chunks_exact(2).enumerate() {
        let hi = (chunk[0] as char).to_digit(16)?;
        let lo = (chunk[1] as char).to_digit(16)?;
        raw[i] = (hi * 16 + lo) as u8;
    }
    let ltk = LongTermKey(u128::from_le_bytes(raw[..16].try_into().ok()?));
    let bd_addr = BdAddr::new(raw[16..22].try_into().ok()?);
    let irk = if raw[22] == 1 {
        Some(IdentityResolvingKey(u128::from_le_bytes(
            raw[23..39].try_into().ok()?,
        )))
    } else {
        None
    };
    let level = match raw[39] {
        1 => SecurityLevel::Encrypted,
        2 => SecurityLevel::EncryptedAuthenticated,
        _ => SecurityLevel::NoEncryption,
    };
    Some(BondInformation::new(
        Identity { bd_addr, irk },
        ltk,
        level,
        true,
    ))
}

/// Load the persisted bond into the security manager (boot path).
async fn load_bond(stack: &'static Stack<'static, Controller, DefaultPacketPool>) {
    let hex = cfg::get_str("ble.bond").await;
    let Some(bond) = bond_decode(hex.as_str()) else {
        return;
    };
    let mut addr = [0u8; 6];
    addr.copy_from_slice(bond.identity.bd_addr.raw());
    match stack.add_bond_information(bond) {
        Ok(()) => {
            set_bond_addr(Some(addr));
            esp_println::println!("ble: bond restored");
        }
        Err(e) => esp_println::println!("ble: bond restore failed {:?}", e),
    }
}

/// Pairing succeeded: persist (CH582: new pairing overwrites), mirror the
/// address, close the window.
async fn on_new_bond(bond: &BondInformation) {
    let mut addr = [0u8; 6];
    addr.copy_from_slice(bond.identity.bd_addr.raw());
    let hex = bond_encode(bond);
    match cfg::set("ble.bond", hex.as_str()).await {
        Ok(()) => {
            set_bond_addr(Some(addr));
            pair_close();
            esp_println::println!("ble: bond stored");
        }
        Err(e) => esp_println::println!("ble: bond store failed {:?}", e),
    }
}

/// AT+BLE=clear: erase flash + close the window immediately, then have
/// the host task purge the security manager (Stack is !Sync) and ack.
pub async fn clear_bond() {
    let _ = cfg::set("ble.bond", "").await;
    pair_close();
    BOND_CLEAR.signal(());
    let _ = embassy_time::with_timeout(Duration::from_secs(2), BOND_CLEARED.wait()).await;
}

/// Host-task side of AT+BLE=clear.
fn handle_bond_clear(stack: &'static Stack<'static, Controller, DefaultPacketPool>) {
    if let Some(addr) = bond_addr() {
        let _ = stack.remove_bond_information(Identity {
            bd_addr: BdAddr::new(addr),
            irk: None,
        });
    }
    set_bond_addr(None);
    BOND_CLEARED.signal(());
    esp_println::println!("ble: bond cleared");
}
/// HID boot keyboard report map — byte-identical to usbd-hid
/// KeyboardReport::desc() so USB and BLE behave the same (dual-mode
/// parity, R5/R6 share kb.rs semantics).
const REPORT_MAP: [u8; 69] = [
    0x05, 0x01, // Usage Page (Generic Desktop)
    0x09, 0x06, // Usage (Keyboard)
    0xA1, 0x01, // Collection (Application)
    0x05, 0x07, //   Usage Page (Key Codes)
    0x19, 0xE0, //   Usage Minimum (224)
    0x29, 0xE7, //   Usage Maximum (231)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x01, //   Logical Maximum (1)
    0x75, 0x01, //   Report Size (1)
    0x95, 0x08, //   Report Count (8)
    0x81, 0x02, //   Input (Data, Variable, Absolute) — modifier byte
    0x19, 0x00, //   Usage Minimum (0)
    0x29, 0xFF, //   Usage Maximum (255)
    0x26, 0xFF, 0x00, // Logical Maximum (255)
    0x75, 0x08, //   Report Size (8)
    0x95, 0x01, //   Report Count (1)
    0x81, 0x03, //   Input (Const, Variable, Absolute) — reserved byte
    0x05, 0x08, //   Usage Page (LEDs)
    0x19, 0x01, //   Usage Minimum (1)
    0x29, 0x05, //   Usage Maximum (5)
    0x25, 0x01, //   Logical Maximum (1)
    0x75, 0x01, //   Report Size (1)
    0x95, 0x05, //   Report Count (5)
    0x91, 0x02, //   Output (Data, Variable, Absolute) — LED report
    0x95, 0x03, //   Report Count (3)
    0x91, 0x03, //   Output (Const, Variable, Absolute) — LED padding
    0x05, 0x07, //   Usage Page (Key Codes)
    0x19, 0x00, //   Usage Minimum (0)
    0x29, 0xDD, //   Usage Maximum (221)
    0x26, 0xFF, 0x00, // Logical Maximum (255)
    0x75, 0x08, //   Report Size (8)
    0x95, 0x06, //   Report Count (6)
    0x81, 0x00, //   Input (Data, Array, Absolute) — keycode array
    0xC0, // End Collection
];

#[gatt_server]
struct Server {
    hid: HidService,
}

/// HID Service 0x1812 (R6.1): Report Map / HID Information / HID Control
/// Point / Protocol Mode / Input+Output Report / Boot Keyboard In+Out.
#[gatt_service(uuid = service::HUMAN_INTERFACE_DEVICE)]
struct HidService {
    /// Report Map (0x2A4B).
    #[characteristic(uuid = "2a4b", read, value = REPORT_MAP)]
    report_map: [u8; 69],
    /// HID Information (0x2A4A): HID 1.11, no country code, remote-wake +
    /// normally-connectable.
    #[characteristic(uuid = "2a4a", read, value = [0x11, 0x01, 0x00, 0x03])]
    hid_info: [u8; 4],
    /// HID Control Point (0x2A4C): 0 = suspend, 1 = exit-suspend.
    #[characteristic(uuid = "2a4c", write_without_response)]
    hid_control_point: u8,
    /// Protocol Mode (0x2A4E): 0 = boot, 1 = report (default).
    #[characteristic(uuid = "2a4e", read, write_without_response, value = 1)]
    protocol_mode: u8,
    /// Input Report (0x2A4D, Report Reference: id 0 — the report map has
    /// no Report ID item — type input). A nonzero id here makes hosts
    /// (BlueZ HoG) strip the first byte of every notification and drop
    /// the mangled report.
    #[descriptor(uuid = "2908", read, value = [0u8, 1u8])]
    #[characteristic(uuid = "2a4d", read, notify)]
    input_report: [u8; 8],
    /// Output Report (0x2A4D, Report Reference: id 0, type output) — LEDs.
    #[descriptor(uuid = "2908", read, value = [0u8, 2u8])]
    #[characteristic(uuid = "2a4d", read, write)]
    output_report: [u8; 1],
    /// Boot Keyboard Input Report (0x2A22).
    #[characteristic(uuid = "2a22", read, notify)]
    boot_keyboard_input: [u8; 8],
    /// Boot Keyboard Output Report (0x2A32) — LEDs.
    #[characteristic(uuid = "2a32", read, write)]
    boot_keyboard_output: [u8; 1],
}

/// One BLE connection, two L2CAP channels (SIG + ATT) for the keyboard.
type Controller = ExternalController<BleConnector<'static>, 1>;
type Resources = HostResources<DefaultPacketPool, 1, 2>;

static RESOURCES: StaticCell<Resources> = StaticCell::new();
static STACK: StaticCell<Stack<'static, Controller, DefaultPacketPool>> = StaticCell::new();

/// AT+BLE=clear crosses into the host task through these signals: the
/// Stack is !Sync, so only the host task may touch the security manager.
static BOND_CLEAR: embassy_sync::signal::Signal<CriticalSectionRawMutex, ()> =
    embassy_sync::signal::Signal::new();
static BOND_CLEARED: embassy_sync::signal::Signal<CriticalSectionRawMutex, ()> =
    embassy_sync::signal::Signal::new();

/// esp-hal's Rng implements rand_core 0.6 RngCore but not the CryptoRng
/// marker; trouble's security manager requires both before Stack::build().
/// The hardware TRNG justifies the marker.
struct Trng(esp_hal::rng::Rng);

impl rand_core::RngCore for Trng {
    fn next_u32(&mut self) -> u32 {
        self.0.random()
    }
    fn next_u64(&mut self) -> u64 {
        (self.0.random() as u64) << 32 | self.0.random() as u64
    }
    fn fill_bytes(&mut self, dest: &mut [u8]) {
        rand_core::RngCore::fill_bytes(&mut self.0, dest)
    }
    fn try_fill_bytes(&mut self, dest: &mut [u8]) -> Result<(), rand_core::Error> {
        rand_core::RngCore::try_fill_bytes(&mut self.0, dest)
    }
}

impl rand_core::CryptoRng for Trng {}

/// Bring up the BLE controller and spawn the host runner + peripheral
/// task. Call once from main when the kbd-ble feature is enabled.
pub fn init(spawner: Spawner, bt: BT<'static>) {
    esp_println::println!(
        "ble: internal heap free {} bytes (pre-controller)",
        esp_alloc::HEAP.free()
    );

    let connector = BleConnector::new(bt, esp_radio::ble::Config::default())
        .expect("ble connector init");
    let controller: Controller = ExternalController::new(connector);
    // Static random address derived from the efuse base MAC (top two bits
    // of the MSB = 0b11 per spec). The S3 controller's public BD_ADDR is
    // not reliably provisioned; advertising without an explicit address
    // never hits the air (observed: bluetoothctl sees nothing).
    let mac = esp_hal::efuse::base_mac_address();
    let mut raw = [0u8; 6];
    raw.copy_from_slice(mac.as_bytes());
    raw.reverse(); // BdAddr is little-endian (HCI order)
    raw[5] |= 0xC0;
    let address = Address::random(raw);
    esp_println::println!("ble: address {:?}", address.addr);

    // Seed the security manager CSPRNG before build() (panics otherwise);
    // set_random_generator_seed consumes self, so chain before the
    // StaticCell takes ownership.

    let stack = STACK.init(
        trouble_host::new(controller, RESOURCES.init(Resources::new()))
            .set_random_generator_seed(&mut Trng(esp_hal::rng::Rng::new()))
            .set_random_address(address),
    );
    // Just Works: keyboard has no display/keyboard for MITM input.
    stack.set_io_capabilities(IoCapabilities::NoInputNoOutput);
    let Host {
        peripheral,
        runner,
        ..
    } = stack.build();

    spawner.spawn(ble_runner_task(runner).expect("spawn ble runner"));
    spawner.spawn(ble_host_task(peripheral, stack).expect("spawn ble host"));

    esp_println::println!(
        "ble: internal heap free {} bytes (post-controller)",
        esp_alloc::HEAP.free()
    );
}

#[embassy_executor::task]
async fn ble_runner_task(mut runner: Runner<'static, Controller, DefaultPacketPool>) {
    loop {
        if let Err(e) = runner.run().await {
            esp_println::println!("ble runner error: {:?}", e);
            Timer::after(Duration::from_millis(500)).await;
        }
    }
}

/// Advertising cadence after a disconnect (CH582 dongle reconnect rhythm).
const READV: Duration = Duration::from_millis(500);

#[embassy_executor::task]
async fn ble_host_task(
    mut peripheral: Peripheral<'static, Controller, DefaultPacketPool>,
    stack: &'static Stack<'static, Controller, DefaultPacketPool>,
) {
    let name = cfg::get_str("device.name").await;
    let mut scan_data = [0u8; 31];
    let scan_len = AdStructure::encode_slice(
        &[AdStructure::CompleteLocalName(name.as_bytes())],
        &mut scan_data,
    )
    .expect("scan data fits");
    // HID UUID 0x1812 + keyboard appearance 0x03C1 in the advertising
    // payload (R6.1); no AdStructure::Appearance variant exists, so the
    // appearance goes out as a raw type-0x19 structure (little-endian).
    let mut adv_data = [0u8; 31];
    let adv_len = AdStructure::encode_slice(
        &[
            AdStructure::Flags(LE_GENERAL_DISCOVERABLE | BR_EDR_NOT_SUPPORTED),
            AdStructure::ServiceUuids16(&[[0x12, 0x18]]),
            AdStructure::Unknown {
                ty: 0x19,
                data: &[0xC1, 0x03],
            },
        ],
        &mut adv_data,
    )
    .expect("adv data fits");

    let server = Server::new_with_config(GapConfig::Peripheral(PeripheralConfig {
        name: name.as_str(),
        appearance: &appearance::human_interface_device::KEYBOARD,
    }))
    .expect("gatt server");
    load_bond(stack).await;

    esp_println::println!("ble: advertising as {}", name.as_str());
    loop {
        let advertiser = match peripheral
            .advertise(
                &AdvertisementParameters::default(),
                Advertisement::ConnectableScannableUndirected {
                    adv_data: &adv_data[..adv_len],
                    scan_data: &scan_data[..scan_len],
                },
            )
            .await
        {
            Ok(a) => a,
            Err(e) => {
                esp_println::println!("ble advertise error: {:?}", e);
                Timer::after(READV).await;
                continue;
            }
        };
        let conn = match select(advertiser.accept(), BOND_CLEAR.wait()).await {
            Either::Second(()) => {
                // AT+BLE=clear while advertising: purge, then re-advertise.
                handle_bond_clear(stack);
                continue;
            }
            Either::First(Err(e)) => {
                esp_println::println!("ble accept error: {:?}", e);
                Timer::after(READV).await;
                continue;
            }
            Either::First(Ok(conn)) => match conn.with_attribute_server(&server) {
                Ok(conn) => conn,
                Err(e) => {
                    esp_println::println!("ble gatt attach error: {:?}", e);
                    Timer::after(READV).await;
                    continue;
                }
            },
        };
        esp_println::println!("ble: connected (heap {} B)", esp_alloc::HEAP.free());
        // R6.3 bonded-only discipline: outside the AT+PAIR window only the
        // bonded host may stay; anything else is dropped post-connect
        // (trouble has no pre-connection address filter). Raw-address
        // compare — fine for static/public peer addresses; peers behind
        // unresolved RPAs would need IRK matching (not exercised here).
        let mut peer = [0u8; 6];
        peer.copy_from_slice(conn.raw().peer_address().raw());
        if !pair_window_open() && bond_addr() != Some(peer) {
            esp_println::println!("ble: reject non-bonded {:02x?}", peer);
            conn.raw().disconnect();
            Timer::after(READV).await;
            continue;
        }
        // Pairing (Just Works) is only offered while the window is open.
        let _ = conn.raw().set_bondable(pair_window_open());
        serve_connection(&server, &conn, stack).await;
        esp_println::println!("ble: disconnected");
        Timer::after(READV).await;
    }
}

/// One connection's event + notification pump. Two interleaved jobs:
/// ATT reads/writes are accepted (the attribute server stores values in
/// the Server fields), and queued kb reports go out as HID input report
/// notifications — written to BOTH the report-protocol and boot-protocol
/// characteristics; notify() is a silent no-op on whichever the host did
/// not subscribe. Runs until disconnect.
async fn serve_connection(
    server: &Server<'_>,
    conn: &GattConnection<'_, '_, DefaultPacketPool>,
    stack: &'static Stack<'static, Controller, DefaultPacketPool>,
) {
    CONNECTED.store(true, Ordering::Relaxed);
    loop {
        match select3(conn.next(), REPORTS.receive(), BOND_CLEAR.wait()).await {
            Either3::Third(()) => {
                // AT+BLE=clear: purge and drop the now-bondless link.
                handle_bond_clear(stack);
                conn.raw().disconnect();
                break;
            }
            Either3::First(GattConnectionEvent::Disconnected { reason }) => {
                esp_println::println!(
                    "ble: disconnected {:?} (heap {} B)",
                    reason,
                    esp_alloc::HEAP.free()
                );
                break;
            }
            Either3::First(GattConnectionEvent::Gatt { event }) => {
                if let GattEvent::Write(w) = &event {
                    esp_println::println!("ble: write h=0x{:02x} {:?}", w.handle(), w.data());
                }
                match event.accept() {
                    Ok(reply) => reply.send().await,
                    Err(e) => esp_println::println!("ble gatt reply error: {:?}", e),
                };
            }
            Either3::First(GattConnectionEvent::PairingComplete {
                security_level,
                bond,
            }) => {
                esp_println::println!("ble: paired {:?}", security_level);
                if let Some(bond) = bond {
                    on_new_bond(&bond).await;
                }
            }
            Either3::First(GattConnectionEvent::PairingFailed(e)) => {
                esp_println::println!("ble: pairing failed {:?}", e);
            }
            Either3::First(_) => {}
            Either3::Second(rep) => {
                let bytes: [u8; 8] = [
                    rep.mods, 0, rep.keys[0], rep.keys[1], rep.keys[2], rep.keys[3],
                    rep.keys[4], rep.keys[5],
                ];
                if let Err(e) = server.hid.input_report.notify(conn, &bytes).await {
                    esp_println::println!("ble: input notify err {:?}", e);
                }
                if let Err(e) = server.hid.boot_keyboard_input.notify(conn, &bytes).await {
                    esp_println::println!("ble: boot notify err {:?}", e);
                }
            }
        }
    }
    CONNECTED.store(false, Ordering::Relaxed);
    // Drain reports queued against the dead link so the next connection
    // never replays stale keys.
    while REPORTS.try_receive().is_ok() {}
}
