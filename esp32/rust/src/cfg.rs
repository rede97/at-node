//! AT-Node rust-s3 — persistent config registry.
//!
//! Semantics aligned with esp32/zephyr/src/cfg.c; key space aligned with
//! esp32/arduino/arduino.ino CFG_TABLE (mqtt.ca is replaced by an embedded
//! CA DER, device.hostname dropped together with the mDNS non-goal).
//!
//! Values are cached in RAM at load/set time so reads never touch flash.
//! Secret keys (wifi.pass, mqtt.pass) are write-only through the AT-facing
//! get/list surface; services use get_str() which returns the real value.
//! Persistence: sequential-storage map on the last 64 KiB of flash.

use core::fmt::Write as _;
use core::ops::Range;
use core::str;

use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::mutex::Mutex;
use embassy_sync::pubsub::{PubSubChannel, Subscriber};
use esp_hal::peripherals::FLASH;
use esp_storage::FlashStorage;
use heapless::String;
use log::warn;
use sequential_storage::cache::{Cache, Uncached};
use sequential_storage::map::{MapConfig, MapStorage};

/// Max value length (Zephyr CFG_VAL_MAX; covers SSID/pass/broker host).
pub const VAL_MAX: usize = 96;  // ble.bond persists as 80 hex chars (R6.3);

/// Storage region: last 64 KiB of the 8 MiB flash (app stays far below).
const STORAGE_RANGE: Range<u32> = 0x7F_0000..0x80_0000;

const F_WO: u8 = 1; // write-only secret (get -> WriteOnly, list masks value)
const F_BOOL: u8 = 2; // persisted normalized to "1"/"0"
const F_INT: u8 = 4; // mqtt.port: 1..=65535
const F_INT60: u8 = 8; // tunnel retry backoff: 1..=60 seconds

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    UnknownKey,
    WriteOnly,
    BadValue,
    Flash,
}

struct Entry {
    key: &'static str,
    flags: u8,
    is_set: bool,
    len: u8,
    val: [u8; VAL_MAX],
}

impl Entry {
    const fn new(key: &'static str, flags: u8) -> Self {
        Self {
            key,
            flags,
            is_set: false,
            len: 0,
            val: [0; VAL_MAX],
        }
    }

    fn value(&self) -> &str {
        str::from_utf8(&self.val[..self.len as usize]).unwrap_or("")
    }
}

/// Registry layout; index doubles as the sequential-storage map key.
/// Registry layout; index doubles as the sequential-storage map key.
/// Only ever APPEND — inserting shifts persisted values to the wrong key.
const TABLE_DEF: [(&str, u8); 23] = [
    ("device.name", 0),
    ("wifi.ssid", 0),
    ("wifi.pass", F_WO),
    ("mqtt.broker", 0),
    ("mqtt.port", F_INT),
    ("mqtt.user", 0),
    ("mqtt.pass", F_WO),
    ("mqtt.auto", F_BOOL),
    ("mqtt.enable", F_BOOL),
    ("http.auto", F_BOOL),
    ("http.enable", F_BOOL),
    ("ble.auto", F_BOOL),
    ("ble.enable", F_BOOL),
    ("device.hostname", 0),
    ("rathole.enable", F_BOOL),
    ("tunnel.1.server", 0),
    ("tunnel.1.token", F_WO),
    ("tunnel.1.service", 0),
    ("tunnel.1.local", 0),
    ("tunnel.1.auto", F_BOOL),
    ("tunnel.1.retry", F_INT60),
    ("tunnel.1.enable", F_BOOL),
    ("ble.bond", F_WO), // R6.3: hex-serialized BLE bond (CH582 single-bond)
];

/// esp-storage implements the sync embedded-storage 0.3 traits while
/// sequential-storage wants the async 0.4 ones; the underlying flash ops
/// are blocking ROM calls either way, so delegate directly.
struct EspFlash(FlashStorage<'static>);

impl embedded_storage_async::nor_flash::ErrorType for EspFlash {
    type Error = esp_storage::FlashStorageError;
}

impl embedded_storage_async::nor_flash::ReadNorFlash for EspFlash {
    const READ_SIZE: usize = 1;

    async fn read(&mut self, offset: u32, bytes: &mut [u8]) -> Result<(), Self::Error> {
        embedded_storage::nor_flash::ReadNorFlash::read(&mut self.0, offset, bytes)
    }

    fn capacity(&self) -> usize {
        embedded_storage::nor_flash::ReadNorFlash::capacity(&self.0)
    }
}

impl embedded_storage_async::nor_flash::NorFlash for EspFlash {
    const WRITE_SIZE: usize = FlashStorage::WORD_SIZE as usize;
    const ERASE_SIZE: usize = FlashStorage::SECTOR_SIZE as usize;

    async fn write(&mut self, offset: u32, bytes: &[u8]) -> Result<(), Self::Error> {
        embedded_storage::nor_flash::NorFlash::write(&mut self.0, offset, bytes)
    }

    async fn erase(&mut self, from: u32, to: u32) -> Result<(), Self::Error> {
        embedded_storage::nor_flash::NorFlash::erase(&mut self.0, from, to)
    }
}

impl embedded_storage_async::nor_flash::MultiwriteNorFlash for EspFlash {}

type Storage = MapStorage<u8, EspFlash, Cache<Uncached, Uncached, Uncached, u8>>;

struct Cfg {
    table: [Entry; TABLE_DEF.len()],
    storage: Storage,
    buf: [u8; 96],
    /// Computed defaults derived from the efuse MAC.
    def_name: String<16>,
    def_hostname: String<16>,
}

static CFG: Mutex<CriticalSectionRawMutex, Option<Cfg>> = Mutex::new(None);

/// Config-change fan-out (Zephyr node_cfg_changed): every successful set()
/// publishes the key; services (wifi/mqtt/http) subscribe and re-read what
/// they care about.
static CHANGED: PubSubChannel<CriticalSectionRawMutex, &'static str, 4, 8, 1> =
    PubSubChannel::new();

#[cfg_attr(not(feature = "wifi"), allow(dead_code))]
pub type ChangedSub = Subscriber<'static, CriticalSectionRawMutex, &'static str, 4, 8, 1>;

/// Subscribe to config-change notifications.
#[cfg_attr(not(feature = "wifi"), allow(dead_code))]
pub fn changed() -> Result<ChangedSub, embassy_sync::pubsub::Error> {
    CHANGED.subscriber()
}

/// Registry default for a key ("" when none). Arduino semantics:
/// device.name = "AT-Node-ESP-XXXX", device.hostname = "atnodeesp-XXXX"
/// (XXXX = last 2 efuse MAC bytes).
fn default_for<'a>(cfg: &'a Cfg, key: &str) -> &'a str {
    match key {
        "device.name" => cfg.def_name.as_str(),
        "device.hostname" => cfg.def_hostname.as_str(),
        "mqtt.port" => "8883",
        "rathole.enable" | "tunnel.1.enable" | "tunnel.1.retry" => "1",
        _ => "",
    }
}

/// Validate + normalize a new value (Zephyr cfg_validate).
fn validate(entry: &Entry, raw: &str, out: &mut String<VAL_MAX>) -> Result<(), Error> {
    out.clear();
    if entry.flags & F_BOOL != 0 {
        match raw {
            "1" | "true" => out.push('1').ok(),
            "0" | "false" => out.push('0').ok(),
            _ => None,
        }
        .ok_or(Error::BadValue)?;
        return Ok(());
    }
    if entry.flags & F_INT != 0 {
        match raw.parse::<u32>() {
            Ok(v) if (1..=65535).contains(&v) => {
                write!(out, "{v}").map_err(|_| Error::BadValue)?;
                return Ok(());
            }
            _ => return Err(Error::BadValue),
        }
    }
    if entry.flags & F_INT60 != 0 {
        match raw.parse::<u32>() {
            Ok(v) if (1..=60).contains(&v) => {
                write!(out, "{v}").map_err(|_| Error::BadValue)?;
                return Ok(());
            }
            _ => return Err(Error::BadValue),
        }
    }
    if raw.len() >= VAL_MAX {
        return Err(Error::BadValue);
    }
    out.push_str(raw).map_err(|_| Error::BadValue)
}

/// Bring up flash storage and load every persisted key into the RAM cache.
pub async fn init(flash: FLASH<'static>) {
    let mac = esp_hal::efuse::base_mac_address();
    let mut def_name: String<16> = String::new();
    let _ = write!(
        def_name,
        "AT-Node-ESP-{:02X}{:02X}",
        mac.as_bytes()[4],
        mac.as_bytes()[5]
    );
    let mut def_hostname: String<16> = String::new();
    let _ = write!(
        def_hostname,
        "atnodeesp-{:02x}{:02x}",
        mac.as_bytes()[4],
        mac.as_bytes()[5]
    );

    let storage = FlashStorage::new(flash).multicore_auto_park();
    let storage: Storage = MapStorage::new(
        EspFlash(storage),
        const { MapConfig::new(STORAGE_RANGE) },
        Cache::new_uncached(),
    );

    let mut cfg = Cfg {
        table: TABLE_DEF.map(|(key, flags)| Entry::new(key, flags)),
        storage,
        buf: [0; 96],
        def_name,
        def_hostname,
    };

    for id in 0..cfg.table.len() {
        match cfg
            .storage
            .fetch_item::<&[u8]>(&mut cfg.buf, &(id as u8))
            .await
        {
            Ok(Some(bytes)) if bytes.len() < VAL_MAX => {
                let e = &mut cfg.table[id];
                e.val[..bytes.len()].copy_from_slice(bytes);
                e.len = bytes.len() as u8;
                e.is_set = true;
            }
            Ok(_) => {}
            Err(_) => warn!("cfg: fetch key {id} failed, using default"),
        }
    }

    *CFG.lock().await = Some(cfg);
}

/// AT-facing read: write-only secrets are rejected (Zephyr cfg_get -EACCES).
pub async fn get(key: &str) -> Result<String<VAL_MAX>, Error> {
    let guard = CFG.lock().await;
    let cfg = guard.as_ref().expect("cfg not initialized");
    let Some(e) = cfg.table.iter().find(|e| e.key == key) else {
        return Err(Error::UnknownKey);
    };
    if e.flags & F_WO != 0 {
        return Err(Error::WriteOnly);
    }
    let mut out = String::new();
    let v = if e.is_set {
        e.value()
    } else {
        default_for(cfg, key)
    };
    out.push_str(v).map_err(|_| Error::BadValue)?;
    Ok(out)
}

/// Service-facing read (WiFi/MQTT); returns the real value for secrets too.
#[allow(dead_code)] // R2/R3: wifi/mqtt service config reads
pub async fn get_str(key: &str) -> String<VAL_MAX> {
    let mut out = String::new();
    let guard = CFG.lock().await;
    let cfg = guard.as_ref().expect("cfg not initialized");
    if let Some(e) = cfg.table.iter().find(|e| e.key == key) {
        let v = if e.is_set {
            e.value()
        } else {
            default_for(cfg, key)
        };
        let _ = out.push_str(v);
    }
    out
}

/// Validate, persist, and update the RAM cache (Zephyr cfg_set).
pub async fn set(key: &str, val: &str) -> Result<(), Error> {
    let mut normalized: String<VAL_MAX> = String::new();
    let mut guard = CFG.lock().await;
    let cfg = guard.as_mut().expect("cfg not initialized");
    let Some(id) = cfg.table.iter().position(|e| e.key == key) else {
        return Err(Error::UnknownKey);
    };
    validate(&cfg.table[id], val, &mut normalized)?;

    cfg.storage
        .store_item(&mut cfg.buf, &(id as u8), &normalized.as_bytes())
        .await
        .map_err(|e| {
            warn!("cfg: store key {id} failed: {e:?}");
            Error::Flash
        })?;

    let e = &mut cfg.table[id];
    e.val[..normalized.len()].copy_from_slice(normalized.as_bytes());
    e.len = normalized.len() as u8;
    e.is_set = true;
    let key = e.key;
    drop(guard);
    let _ = CHANGED.immediate_publisher().try_publish(key);
    Ok(())
}

/// Arduino config_list_json(): secrets mask the value entirely.
pub async fn list_json(out: &mut String<1600>) {
    let guard = CFG.lock().await;
    let cfg = guard.as_ref().expect("cfg not initialized");
    out.clear();
    let _ = out.push('[');
    for (i, e) in cfg.table.iter().enumerate() {
        if i > 0 {
            let _ = out.push(',');
        }
        let _ = write!(out, "{{\"key\":\"{}\"", e.key);
        if e.flags & F_WO != 0 {
            let _ = out.push_str(",\"secret\":true");
        } else {
            let v = if e.is_set {
                e.value()
            } else {
                default_for(cfg, e.key)
            };
            let _ = write!(out, ",\"value\":\"{v}\"");
        }
        let _ = out.push('}');
    }
    let _ = out.push(']');
}

/// AT+NVS=clear: erase the storage region and drop the RAM cache.
pub async fn erase_all() -> Result<(), Error> {
    let mut guard = CFG.lock().await;
    let cfg = guard.as_mut().expect("cfg not initialized");
    cfg.storage
        .erase_all()
        .await
        .map_err(|_| Error::Flash)?;
    for e in &mut cfg.table {
        e.is_set = false;
        e.len = 0;
    }
    Ok(())
}
