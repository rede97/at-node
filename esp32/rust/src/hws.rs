//! AT-Node rust-s3 — hardware services (GPIO / ADC / I2C).
//!
//! Semantics aligned with esp32/zephyr/src/hws.c:
//! - GPIO: blacklist rejects strapping/USB/flash/PSRAM/console pins (H6);
//!   write = push-pull output with initial level, read = input + pull-up
//!   (Arduino pinMode/digitalRead parity).
//! - ADC: unit 1 channels 0..9 = GPIO1..10, 12 bit, 11 dB attenuation
//!   (esp-hal calibration converts to mV, matching Arduino
//!   analogReadMilliVolts full-scale).
//! - I2C0: SDA=GPIO8, SCL=GPIO9, 100 kHz. Scan probes with a 1-byte read
//!   (esp-hal rejects Zephyr's zero-length write probe with
//!   ZeroLengthInvalid; read probes ACK-check the address byte the same
//!   way. Caveat: a read probe may pop a register on some devices).
//!
//! Unsafe pin model: user-visible aliasing (GPIO vs ADC vs I2C on the same
//! physical pin) is allowed exactly like the Arduino/Zephyr variants —
//! mode clobber between commands is the documented behavior. `AnyPin::steal`
//! is confined to this module and guarded by the same blacklist; pin 48 is
//! additionally forbidden here (it drives the WS2812 via RMT).

use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::mutex::Mutex;
use esp_hal::analog::adc::{Adc, AdcConfig, AdcPin, Attenuation};
use esp_hal::gpio::{AnyPin, Flex, InputConfig, Level, OutputConfig, OutputSignal, Pull};
use esp_hal::i2c::master::I2c;
use esp_hal::peripherals;
use heapless::String;

const I2C_SCAN_FIRST: u8 = 0x08;
const I2C_SCAN_LAST: u8 = 0x77;

/// AT+I2C_R/AT+I2C_W data byte cap (Zephyr I2C_IO_MAX).
pub const I2C_IO_MAX: usize = 32;

/// Pins that must never be touched by user AT commands:
/// 0,3,45,46 strapping; 19,20 native USB; 26..32 SPI flash; 33..37 octal
/// PSRAM; 43,44 UART0 console. 48 is the WS2812 (RMT) on this firmware.
fn pin_forbidden(pin: u8) -> bool {
    if pin > 48 {
        return true;
    }
    if (26..=37).contains(&pin) {
        return true;
    }
    matches!(pin, 0 | 3 | 19 | 20 | 43 | 44 | 45 | 46 | 48)
}

// ---------------------------------------------------------------- GPIO ---

/// Lazily-created Flex per pin so an output level set by GPIO_W persists.
static PINS: critical_section::Mutex<core::cell::RefCell<[Option<Flex<'static>>; 49]>> =
    critical_section::Mutex::new(core::cell::RefCell::new([const { None }; 49]));

fn with_flex<R>(pin: u8, f: impl FnOnce(&mut Flex<'static>) -> R) -> R {
    critical_section::with(|cs| {
        let mut pins = PINS.borrow(cs).borrow_mut();
        let slot = &mut pins[pin as usize];
        if slot.is_none() {
            // SAFETY: pin passed the blacklist; hws is the sole GPIO owner of
            // every non-blacklisted pin. ADC pins 1-10 alias the ADC table by
            // design (Zephyr/Arduino parity).
            *slot = Some(Flex::new(unsafe { AnyPin::steal(pin) }));
        }
        f(slot.as_mut().expect("flex"))
    })
}

/// AT+GPIO_W: configure push-pull output with the requested initial level.
pub fn gpio_write(pin: u8, high: bool) -> Result<(), ()> {
    if pin_forbidden(pin) {
        return Err(());
    }
    with_flex(pin, |p| {
        p.set_level(if high { Level::High } else { Level::Low });
        p.set_output_enable(true);
    });
    Ok(())
}

/// AT+GPIO_R: configure input with pull-up (Arduino parity), return level.
pub fn gpio_read(pin: u8) -> Result<u8, ()> {
    if pin_forbidden(pin) {
        return Err(());
    }
    Ok(with_flex(pin, |p| {
        p.set_output_enable(false);
        p.apply_input_config(&InputConfig::default().with_pull(Pull::Up));
        p.set_input_enable(true);
        u8::from(p.is_high())
    }))
}

// ----------------------------------------------------------------- ADC ---

type Adc1 = Adc<'static, peripherals::ADC1<'static>, esp_hal::Blocking>;
type Pin<P> = AdcPin<P, peripherals::ADC1<'static>>;

/// ADC1 channel n sits on GPIO(n+1). All channels are enabled at 11 dB
/// during init (xtensa enables pins through AdcConfig, not Adc).
#[allow(clippy::type_complexity)]
enum Slot {
    G1(Pin<peripherals::GPIO1<'static>>),
    G2(Pin<peripherals::GPIO2<'static>>),
    G3(Pin<peripherals::GPIO3<'static>>),
    G4(Pin<peripherals::GPIO4<'static>>),
    G5(Pin<peripherals::GPIO5<'static>>),
    G6(Pin<peripherals::GPIO6<'static>>),
    G7(Pin<peripherals::GPIO7<'static>>),
    G10(Pin<peripherals::GPIO10<'static>>),
}

struct AdcState {
    adc: Adc1,
    slots: [Slot; 8],
}

static ADC: critical_section::Mutex<core::cell::RefCell<Option<AdcState>>> =
    critical_section::Mutex::new(core::cell::RefCell::new(None));



/// AT+ADC=<ch>: calibrated millivolts on ADC1. Channels 7/8 are
/// rejected: GPIO8/9 are the I2C bus (and they never read correctly
/// through esp-hal's analog path — touch pads; reads rail at 4095).
pub fn adc_read_mv(ch: u8) -> Result<u16, ()> {
    if ch > 9 || ch == 7 || ch == 8 {
        return Err(());
    }
    critical_section::with(|cs| {
        let mut guard = ADC.borrow(cs).borrow_mut();
        let st = guard.as_mut().expect("adc not initialized");
        let adc = &mut st.adc;
        // S3 curve-fitting calibration: read_blocking returns millivolts.
        let idx = if ch == 9 { 7 } else { ch as usize };
        let mv = match &mut st.slots[idx] {
            Slot::G1(p) => adc.read_blocking(p),
            Slot::G2(p) => adc.read_blocking(p),
            Slot::G3(p) => adc.read_blocking(p),
            Slot::G4(p) => adc.read_blocking(p),
            Slot::G5(p) => adc.read_blocking(p),
            Slot::G6(p) => adc.read_blocking(p),
            Slot::G7(p) => adc.read_blocking(p),
            Slot::G10(p) => adc.read_blocking(p),
        };
        Ok(mv)
    })
}

// ----------------------------------------------------------------- I2C ---

static I2C: Mutex<CriticalSectionRawMutex, Option<I2c<'static, esp_hal::Blocking>>> =
    Mutex::new(None);

/// Hardware I2C owns pins 8/9 for R/W transactions; the bit-bang scan
/// detaches its output routing for the scan duration and restores it
/// after (validated GPIO-first on 2026-08-22, then integrated).
const I2C_HW_ENABLED: bool = true;

/// Bring up ADC1 and I2C0. Call once from main. ADC takes ownership of
/// GPIO1..10 (I2C SDA/SCL on 8/9 go through AnyPin::steal so ADC channels
/// 7/8 stay reachable, matching Zephyr's register-level sharing).
#[allow(clippy::too_many_arguments)]
pub fn init(
    i2c0: peripherals::I2C0<'static>,
    adc1: peripherals::ADC1<'static>,
    g1: peripherals::GPIO1<'static>,
    g2: peripherals::GPIO2<'static>,
    g3: peripherals::GPIO3<'static>,
    g4: peripherals::GPIO4<'static>,
    g5: peripherals::GPIO5<'static>,
    g6: peripherals::GPIO6<'static>,
    g7: peripherals::GPIO7<'static>,
    g10: peripherals::GPIO10<'static>,
) {
    let mut adc_cfg = AdcConfig::new();
    let slots = [
        Slot::G1(adc_cfg.enable_pin(g1, Attenuation::_11dB)),
        Slot::G2(adc_cfg.enable_pin(g2, Attenuation::_11dB)),
        Slot::G3(adc_cfg.enable_pin(g3, Attenuation::_11dB)),
        Slot::G4(adc_cfg.enable_pin(g4, Attenuation::_11dB)),
        Slot::G5(adc_cfg.enable_pin(g5, Attenuation::_11dB)),
        Slot::G6(adc_cfg.enable_pin(g6, Attenuation::_11dB)),
        Slot::G7(adc_cfg.enable_pin(g7, Attenuation::_11dB)),
        Slot::G10(adc_cfg.enable_pin(g10, Attenuation::_11dB)),
    ];
    critical_section::with(|cs| {
        ADC.borrow(cs)
            .borrow_mut()
            .replace(AdcState {
                adc: Adc::new(adc1, adc_cfg),
                slots,
            });
    });

    // Note: enable_pin() puts the pads in analog mode (RTC mux), which
    // disables the digital input buffer. ADC and digital GPIO on the same
    // pin do NOT coexist; the bit-bang I2C scan flips pins 8/9 to digital
    // for its duration and restores analog afterwards.

    let i2c = I2c::new(i2c0, esp_hal::i2c::master::Config::default())
        .expect("i2c config");
    if I2C_HW_ENABLED {
        // SAFETY: GPIO8/9 are the designated I2C pins (Zephyr app.overlay);
        // hws owns them at register level.
        let sda = unsafe { AnyPin::steal(8) };
        let scl = unsafe { AnyPin::steal(9) };
        let i2c = i2c.with_sda(sda).with_scl(scl);
        let mut guard = I2C.try_lock().expect("i2c static locked during init");
        *guard = Some(i2c);
    } else {
        let mut guard = I2C.try_lock().expect("i2c static locked during init");
        *guard = Some(i2c);
    }
}

/// "+I2C: 0xXX 0xYY" or "+I2C: none" (Zephyr hws_i2c_scan).
///
/// Bit-banged scan (user decision 2026-08-21): the hardware I2C path pays
/// ~260 ms per absent address in esp-hal's NACK recovery (clear-bus +
/// FSM timeouts), so a bare-board scan took ~29 s and stalled the whole
/// executor. Bit-banging only START + address + ACK + STOP is ~100 µs per
/// address (~12 ms total), needs no recovery machinery, and — unlike the
/// 1-byte read probe — never enters the data phase, so it cannot pop a
/// device register. Real read/write transactions still use the hardware
/// I2C driver (proper FSM, clock stretching, speed).
pub async fn i2c_scan(out: &mut String<600>) {
    // Hold the I2C mutex so no hardware transaction races the bit-bang.
    let _guard = I2C.lock().await;
    let mut bus = BitbangBus::claim();
    out.clear();
    let _ = out.push_str("+I2C:");
    let mut found = false;
    for addr in I2C_SCAN_FIRST..=I2C_SCAN_LAST {
        if bus.probe(addr) {
            let _ = core::fmt::Write::write_fmt(out, format_args!(" 0x{addr:02X}"));
            found = true;
        }
    }
    if !found {
        let _ = out.push_str(" none");
    }
}

// ------------------------------------------------- bit-bang scan (GPIO) ---

const BB_SDA: u8 = 8;
const BB_SCL: u8 = 9;
/// Half-bit pacing: ~5 µs -> 100 kHz.
const BB_HALF_US: u32 = 5;

/// One bit-bang session on the I2C pins: pins are claimed once (output
/// routing detached, pads configured open-drain + pull-up) and restored
/// on drop. Total scan ≈ 112 probes × ~100 µs ≈ 15 ms.
struct BitbangBus {
    sda: Flex<'static>,
    scl: Flex<'static>,
}

impl BitbangBus {
    fn claim() -> Self {
        use esp_hal::gpio::interconnect::PeripheralOutput;

        // SAFETY: GPIO8/9 are hws-owned I2C pins; the hardware driver is
        // idle (I2C mutex held by caller). Output routing is detached for
        // the session and restored on drop.
        let sda_raw = unsafe { AnyPin::steal(BB_SDA) };
        let scl_raw = unsafe { AnyPin::steal(BB_SCL) };
        if I2C_HW_ENABLED {
            sda_raw.disconnect_from_peripheral_output();
            scl_raw.disconnect_from_peripheral_output();
        }
        let mut sda = Flex::new(sda_raw);
        let mut scl = Flex::new(scl_raw);
        for p in [&mut sda, &mut scl] {
            p.apply_output_config(
                &OutputConfig::default()
                    .with_drive_mode(esp_hal::gpio::DriveMode::OpenDrain)
                    .with_pull(Pull::Up),
            );
            p.set_output_enable(true);
            p.apply_input_config(&InputConfig::default().with_pull(Pull::Up));
            p.set_input_enable(true);
            p.set_level(Level::High); // released
        }
        Self { sda, scl }
    }

    fn delay(&self) {
        esp_hal::delay::Delay::new().delay_micros(BB_HALF_US);
    }

    /// Clock out one byte, return the ACK bit (true = ACK).
    fn byte(&mut self, byte: u8) -> bool {
        let mut b = byte;
        for _ in 0..8 {
            self.sda.set_level(if b & 0x80 != 0 {
                Level::High
            } else {
                Level::Low
            });
            b <<= 1;
            self.delay();
            self.scl.set_high();
            self.delay();
            self.scl.set_low();
        }
        self.sda.set_high();
        self.delay();
        self.scl.set_high();
        self.delay();
        let ack = !self.sda.is_high();
        self.scl.set_low();
        self.delay();
        ack
    }

    fn start(&mut self) {
        // Stuck-bus recovery: clock SDA out if a slave holds it low.
        if !self.sda.is_high() {
            for _ in 0..9 {
                self.scl.set_low();
                self.delay();
                self.scl.set_high();
                self.delay();
            }
        }
        self.sda.set_low();
        self.delay();
        self.scl.set_low();
        self.delay();
    }

    fn stop(&mut self) {
        self.sda.set_low();
        self.delay();
        self.scl.set_high();
        self.delay();
        self.sda.set_high();
        self.delay();
    }

    /// START + addr/W + ACK + STOP. True if the address ACKs.
    fn probe(&mut self, addr: u8) -> bool {
        self.start();
        let ack = self.byte(addr << 1);
        self.stop();
        ack
    }

}

impl Drop for BitbangBus {
    fn drop(&mut self) {
        if I2C_HW_ENABLED {
            use esp_hal::gpio::interconnect::PeripheralOutput;
            // Restore the I2C output routing (input routing never touched).
            // SAFETY: hws owns the pins; session over.
            let sda_raw = unsafe { AnyPin::steal(BB_SDA) };
            let scl_raw = unsafe { AnyPin::steal(BB_SCL) };
            sda_raw.connect_peripheral_to_output(OutputSignal::I2CEXT0_SDA);
            scl_raw.connect_peripheral_to_output(OutputSignal::I2CEXT0_SCL);
        }
    }
}

/// Register address encoding: reg <= 0xFF sends one address byte
/// (typical sensors), reg > 0xFF sends two (16-bit-address EEPROMs like
/// the AT24C256 — single-byte frames wedge those clones).
fn reg_bytes(reg: u32) -> ([u8; 2], usize) {
    if reg > 0xFF {
        ([(reg >> 8) as u8, (reg & 0xFF) as u8], 2)
    } else {
        ([reg as u8, 0], 1)
    }
}

/// Burst read: write register address, read len bytes (Zephyr hws_i2c_read).
pub async fn i2c_read(addr: u8, reg: u32, data: &mut [u8]) -> Result<(), ()> {
    if data.is_empty() || addr > 0x7F || reg > 0xFFFF || !I2C_HW_ENABLED {
        return Err(());
    }
    let (rb, rn) = reg_bytes(reg);
    let mut guard = I2C.lock().await;
    let i2c = guard.as_mut().expect("i2c not initialized");
    i2c.write_read(addr, &rb[..rn], data).map_err(|_| ())
}

/// Burst write: register address followed by data (Zephyr hws_i2c_write).
pub async fn i2c_write(addr: u8, reg: u32, data: &[u8]) -> Result<(), ()> {
    if addr > 0x7F || reg > 0xFFFF || data.len() > I2C_IO_MAX || !I2C_HW_ENABLED {
        return Err(());
    }
    let (rb, rn) = reg_bytes(reg);
    let mut buf = [0u8; I2C_IO_MAX + 2];
    buf[..rn].copy_from_slice(&rb[..rn]);
    buf[rn..rn + data.len()].copy_from_slice(data);
    let mut guard = I2C.lock().await;
    let i2c = guard.as_mut().expect("i2c not initialized");
    i2c.write(addr, &buf[..rn + data.len()]).map_err(|_| ())
}
