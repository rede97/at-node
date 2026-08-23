//! AT-Node rust-s3 — USB HID keyboard backend (kbd-usb feature, R5).
//!
//! esp-hal otg_fs (Synopsys USB OTG FS @ GPIO19 D- / GPIO20 D+) +
//! usb-device + usbd-hid, boot-protocol 8-byte reports only. The task
//! polls the USB device and applies raw reports queued by the kb engine;
//! all key timing lives in kb.rs.

use embassy_futures::select::{Either, select};
use embassy_sync::blocking_mutex::raw::CriticalSectionRawMutex;
use embassy_sync::channel::Channel;
use embassy_time::{Duration, Timer};
use esp_hal::otg_fs::{Usb, UsbBus};
use esp_hal::peripherals::{GPIO19, GPIO20, USB0};
use static_cell::StaticCell;
use usb_device::prelude::*;
use usbd_hid::descriptor::{KeyboardReport, SerializedDescriptor as _};
use usbd_hid::hid_class::HIDClass;

use crate::kb::Report;

type Bus = UsbBus<Usb<'static>>;

static REPORTS: Channel<CriticalSectionRawMutex, Report, 8> = Channel::new();

/// Queue a raw report (kb engine only; backpressure = engine pacing).
pub async fn send_report(r: Report) {
    REPORTS.send(r).await;
}

/// Bring up the USB device and spawn the keyboard task. Call once from
/// main when the kbd-usb feature is enabled.
pub fn init(
    spawner: embassy_executor::Spawner,
    usb0: USB0<'static>,
    dp: GPIO20<'static>,
    dm: GPIO19<'static>,
) {
    static EP_MEMORY: StaticCell<[u32; 1024]> = StaticCell::new();
    static USB_BUS: StaticCell<UsbDeviceBus> = StaticCell::new();
    static STRINGS: StaticCell<[StringDescriptors<'static>; 1]> = StaticCell::new();

    let usb = Usb::new(usb0, dp, dm);
    let bus = USB_BUS.init(UsbBus::new(usb, EP_MEMORY.init([0; 1024])));
    let strings = STRINGS.init([StringDescriptors::default()
        .manufacturer("AT-Node")
        .product("AT-Node Keyboard")
        .serial_number("0001")]);

    let hid = HIDClass::new(bus, KeyboardReport::desc(), 10);
    let dev = UsbDeviceBuilder::new(bus, UsbVidPid(0x303A, 0x8201))
        .strings(strings)
        .expect("usb strings")
        .build();
    spawner.spawn(kbd_usb_task(dev, hid).expect("spawn kbd usb task"));
}

type UsbDeviceBus = usb_device::bus::UsbBusAllocator<Bus>;

#[embassy_executor::task]
async fn kbd_usb_task(
    mut dev: UsbDevice<'static, Bus>,
    hid: HIDClass<'static, Bus>,
) {
    let mut hid = hid;
    loop {
        // USB device state machine + HID IN reports. 5 ms poll cadence:
        // HID idle rate is 10 ms; faster polling only costs a wake.
        let _ = dev.poll(&mut [&mut hid]);
        if let Either::First(rep) =
            select(REPORTS.receive(), Timer::after(Duration::from_millis(5))).await
        {
            let _ = hid.push_input(&KeyboardReport {
                modifier: rep.mods,
                reserved: 0,
                leds: 0,
                keycodes: rep.keys,
            });
        }
    }
}
