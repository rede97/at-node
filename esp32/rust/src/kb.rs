// The routing/engine internals are only read by a compiled-in backend;
// with neither kbd feature the surfaces still call kb::enabled().
#![cfg_attr(not(any(feature = "kbd-usb", feature = "kbd-ble")), allow(dead_code))]

//! AT-Node rust-s3 — keyboard routing layer (CH582 kb_* semantics).
//!
//! One command surface (AT/HTTP/MQTT) -> kb ops -> engine task -> backend
//! target. R5 lands the USB backend (kbd_usb); R6 adds BLE. The engine
//! owns all timing: tap press/release, text per-char pacing, KEY_SEQ
//! batch playback — backends only apply raw 8-byte boot reports.
//!
//! Injection discipline (CH582 FIELD-NOTES F18): prefer AT+TAP / KEY_STR /
//! KEY_SEQ (press/release paired automatically); bare AT+KEY holds the
//! report until an explicit AT+KEY=0,0 release.

use core::sync::atomic::{AtomicU8, Ordering};

use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::channel::Channel;
use embassy_time::{Duration, Timer};
use heapless::{String, Vec};

/// Any keyboard backend compiled in (kbd-usb now, kbd-ble in R6).
pub fn enabled() -> bool {
    cfg!(feature = "kbd-usb") || cfg!(feature = "kbd-ble")
}

// ------------------------------------------------------------- target ---

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Target {
    Usb = 0,
    Ble = 1,
}

// Boot target follows the compiled backend: USB when kbd-usb exists
// (dual-mode boards default to wired), BLE on kbd-ble-only builds —
// otherwise a reboot would silently swallow keys into a backend that
// was never compiled in.
static TARGET: AtomicU8 = AtomicU8::new(if cfg!(feature = "kbd-usb") {
    Target::Usb as u8
} else {
    Target::Ble as u8
});

pub fn target() -> Target {
    match TARGET.load(Ordering::Relaxed) {
        1 => Target::Ble,
        _ => Target::Usb,
    }
}

/// AT+DEV=USB|BLE. BLE is refused until R6 (mirrors CH582 reporting the
/// error instead of silently dropping keys).
pub fn set_target(t: Target) -> Result<(), ()> {
    match t {
        Target::Usb if cfg!(feature = "kbd-usb") => {
            TARGET.store(t as u8, Ordering::Relaxed);
            Ok(())
        }
        Target::Ble if cfg!(feature = "kbd-ble") => {
            TARGET.store(t as u8, Ordering::Relaxed);
            Ok(())
        }
        _ => Err(()),
    }
}

pub fn target_str() -> &'static str {
    match target() {
        Target::Usb => "USB",
        Target::Ble => "BLE",
    }
}

// ------------------------------------------------------------ key ops ---

/// One 8-byte boot report (modifier + up to 6 keycodes).
#[derive(Clone, Copy, Default)]
pub struct Report {
    pub mods: u8,
    pub keys: [u8; 6],
}

/// Max reports per AT+KEY_SEQ command (CH582 SEQ_MAX_REPORTS, bounded by
/// the AT line length).
pub const SEQ_MAX_REPORTS: usize = 8;

enum Op {
    /// Bare report; holds until the next report (AT+KEY).
    Hold(Report),
    /// Atomic press + release (AT+TAP).
    Tap { mods: u8, key: u8, ms: u32 },
    /// ASCII text, auto-paired press/release per char (AT+KEY_STR).
    Text { s: String<192>, ms: u32, gap: u32 },
    /// Batch report playback (AT+KEY_SEQ).
    Seq { reports: Vec<Report, SEQ_MAX_REPORTS>, delay_ms: u32 },
}

static OPS: Channel<CriticalSectionRawMutex, Op, 4> = Channel::new();

/// Queue is full / no backend. Callers turn this into "ERROR busy".
fn post(op: Op) -> bool {
    OPS.try_send(op).is_ok()
}

pub fn hold(r: Report) -> bool {
    post(Op::Hold(r))
}

pub fn tap(mods: u8, key: u8, ms: u32) -> bool {
    post(Op::Tap { mods, key, ms })
}

pub fn text(s: String<192>, ms: u32, gap: u32) -> bool {
    post(Op::Text { s, ms, gap })
}

pub fn seq(reports: Vec<Report, SEQ_MAX_REPORTS>, delay_ms: u32) -> bool {
    post(Op::Seq { reports, delay_ms })
}

// ------------------------------------------------------ engine task -----

async fn emit(r: Report) {
    match target() {
        Target::Usb => {
            #[cfg(feature = "kbd-usb")]
            crate::kbd_usb::send_report(r).await;
            #[cfg(not(feature = "kbd-usb"))]
            let _ = r;
        }
        Target::Ble => {
            #[cfg(feature = "kbd-ble")]
            crate::kbd_ble::send_report(r).await;
            #[cfg(not(feature = "kbd-ble"))]
            let _ = r;
        }
    }
}

const RELEASE: Report = Report {
    mods: 0,
    keys: [0; 6],
};

/// Sequence engine: owns all timing (CH582 TMOS playback equivalent).
/// Spawned once when any keyboard backend is compiled in.
pub async fn engine_task() -> ! {
    loop {
        match OPS.receive().await {
            Op::Hold(r) => emit(r).await,
            Op::Tap { mods, key, ms } => {
                emit(Report {
                    mods,
                    keys: [key, 0, 0, 0, 0, 0],
                })
                .await;
                Timer::after(Duration::from_millis(u64::from(ms))).await;
                emit(RELEASE).await;
            }
            Op::Text { s, ms, gap } => {
                for ch in s.chars() {
                    let Some((shift, kc)) = ascii_to_hid(ch) else {
                        continue;
                    };
                    emit(Report {
                        mods: shift,
                        keys: [kc, 0, 0, 0, 0, 0],
                    })
                    .await;
                    Timer::after(Duration::from_millis(u64::from(ms))).await;
                    emit(RELEASE).await;
                    Timer::after(Duration::from_millis(u64::from(gap))).await;
                }
            }
            Op::Seq { reports, delay_ms } => {
                for r in reports {
                    emit(r).await;
                    Timer::after(Duration::from_millis(u64::from(delay_ms))).await;
                }
            }
        }
    }
}

// ------------------------------------------------------- ASCII -> HID ---

/// US-layout mapping: char -> (modifier, keycode). Modifier 2 = LSHIFT.
/// Unmapped chars return None (caller skips, send_key.py parity).
pub fn ascii_to_hid(ch: char) -> Option<(u8, u8)> {
    let (m, k) = match ch {
        'a'..='z' => (0, ch as u8 - b'a' + 0x04),
        'A'..='Z' => (2, ch as u8 - b'A' + 0x04),
        '1' => (0, 0x1E),
        '2' => (0, 0x1F),
        '3' => (0, 0x20),
        '4' => (0, 0x21),
        '5' => (0, 0x22),
        '6' => (0, 0x23),
        '7' => (0, 0x24),
        '8' => (0, 0x25),
        '9' => (0, 0x26),
        '0' => (0, 0x27),
        '!' => (2, 0x1E),
        '@' => (2, 0x1F),
        '#' => (2, 0x20),
        '$' => (2, 0x21),
        '%' => (2, 0x22),
        '^' => (2, 0x23),
        '&' => (2, 0x24),
        '*' => (2, 0x25),
        '(' => (2, 0x26),
        ')' => (2, 0x27),
        ' ' => (0, 0x2C),
        '\n' => (0, 0x28),
        '\t' => (0, 0x2B),
        '-' => (0, 0x2D),
        '_' => (2, 0x2D),
        '=' => (0, 0x2E),
        '+' => (2, 0x2E),
        '[' => (0, 0x2F),
        '{' => (2, 0x2F),
        ']' => (0, 0x30),
        '}' => (2, 0x30),
        '\\' => (0, 0x31),
        '|' => (2, 0x31),
        ';' => (0, 0x33),
        ':' => (2, 0x33),
        '\'' => (0, 0x34),
        '"' => (2, 0x34),
        '`' => (0, 0x35),
        '~' => (2, 0x35),
        ',' => (0, 0x36),
        '<' => (2, 0x36),
        '.' => (0, 0x37),
        '>' => (2, 0x37),
        '/' => (0, 0x38),
        '?' => (2, 0x38),
        _ => return None,
    };
    Some((m, k))
}
