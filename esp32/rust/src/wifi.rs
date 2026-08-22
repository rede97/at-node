//! AT-Node rust-s3 — WiFi STA + reconnect watchdog.
//!
//! Rhythm aligned with esp32/zephyr/src/wifi_sta.c (Arduino loop parity):
//! while creds exist and the link is down, (re)connect; failures retry on
//! a 15 s cadence. Connected: LED green + RSSI refresh every 2 s; link
//! loss -> slow-blink blue and back to the retry loop. Config changes
//! (wifi.ssid/wifi.pass via any channel) wake the watchdog immediately
//! through the cfg pubsub (Zephyr node_cfg_changed fan-out).
//!
//! Link-up events are observable by later stages (mqtt/http) through
//! stack()/link_up(); MIGRATION section 5.4's Signal fan-out lands with R3.

use embassy_executor::Spawner;
use embassy_futures::select::{Either, select};
use embassy_net::{Runner, Stack, StackResources};
use embassy_time::{Duration, Timer};
use esp_hal::peripherals::WIFI;
use esp_radio::wifi::{Config, ControllerConfig, Interface, WifiController, sta::StationConfig};
use log::{info, warn};
use static_cell::StaticCell;

use crate::{cfg, led};

/// Reconnect cadence when the link is down (Zephyr/Arduino 15 s).
const RETRY: Duration = Duration::from_secs(15);
/// RSSI refresh + link-loss poll cadence while connected.
const POLL: Duration = Duration::from_secs(2);

static RESOURCES: StaticCell<StackResources<16>> = StaticCell::new();

/// Cached link state for AT+STATUS; owned by the wifi task. Plain data so
/// it can sit behind a blocking mutex (updated at transitions + RSSI poll).
#[derive(Clone, Copy, Default)]
struct State {
    up: bool,
    ip: Option<embassy_net::Ipv4Address>,
    rssi: i32,
}

static STATE: critical_section::Mutex<core::cell::RefCell<State>> =
    critical_section::Mutex::new(core::cell::RefCell::new(State {
        up: false,
        ip: None,
        rssi: 0,
    }));

fn set_state(f: impl FnOnce(&mut State)) {
    critical_section::with(|cs| f(&mut STATE.borrow(cs).borrow_mut()));
}

/// Bring up the WiFi driver, the embassy-net stack (DHCP), and spawn the
/// watchdog task. Call once from main; returns the stack handle for later
/// network services (http R4, mqtt R3).
pub fn init(spawner: Spawner, wifi: WIFI<'static>, random_seed: u64) -> Stack<'static> {
    let (controller, interfaces) =
        esp_radio::wifi::new(wifi, ControllerConfig::default()).expect("wifi init");
    let (stack, runner) = embassy_net::new(
        interfaces.station,
        embassy_net::Config::dhcpv4(Default::default()),
        RESOURCES.init(StackResources::new()),
        random_seed,
    );
    spawner.spawn(net_task(runner).expect("spawn net task"));
    spawner.spawn(wifi_task(controller, stack).expect("spawn wifi task"));
    stack
}

/// Link up and DHCP done (AT+STATUS wifi=/ip= fields).
pub fn link_up() -> bool {
    critical_section::with(|cs| STATE.borrow(cs).borrow().up)
}

/// IPv4 address for AT+STATUS (None until DHCP finishes).
pub fn ipv4() -> Option<embassy_net::Ipv4Address> {
    critical_section::with(|cs| STATE.borrow(cs).borrow().ip)
}

/// Last RSSI sample (0 when never connected; refreshed every POLL while up).
pub fn rssi() -> i32 {
    critical_section::with(|cs| STATE.borrow(cs).borrow().rssi)
}

#[embassy_executor::task]
async fn net_task(mut runner: Runner<'static, Interface<'static>>) -> ! {
    runner.run().await
}

/// Wait for a cfg change or a timeout; true when a relevant key changed.
async fn wait_change(
    changed: &mut cfg::ChangedSub,
    timeout: Duration,
    relevant: fn(&str) -> bool,
) -> bool {
    match select(changed.next_message_pure(), Timer::after(timeout)).await {
        Either::First(key) => relevant(key),
        Either::Second(()) => false,
    }
}

#[embassy_executor::task]
async fn wifi_task(mut controller: WifiController<'static>, stack: Stack<'static>) {
    let mut changed = cfg::changed().expect("cfg change subscriber");
    let mut cur_ssid: heapless::String<64> = heapless::String::new();
    let mut cur_pass: heapless::String<64> = heapless::String::new();

    loop {
        let ssid = cfg::get_str("wifi.ssid").await;
        let pass = cfg::get_str("wifi.pass").await;
        if ssid.is_empty() {
            cur_ssid.clear();
            cur_pass.clear();
            // No creds: idle until they show up (any key may be the one).
            wait_change(&mut changed, RETRY, |_| true).await;
            continue;
        }

        if ssid == cur_ssid && pass == cur_pass && controller.is_connected() {
            // Creds unchanged and link up: nothing to do. Avoids a full
            // driver reconfig on redundant writes (which wedges the
            // driver when a TLS session is mid-flight).
            wait_change(&mut changed, RETRY, |k| k.starts_with("wifi.")).await;
            continue;
        }
        cur_ssid = ssid.clone();
        cur_pass = pass.clone();
        if controller.is_connected() {
            // Graceful link-down before reconfig (driver stop with a live
            // TLS session can hang the blob).
            let _ = controller.disconnect_async().await;
        }

        led::status(led::Status::WifiConnecting);
        let conf = Config::Station(
            StationConfig::default()
                .with_ssid(ssid.as_str())
                .with_password(pass.as_str().into()),
        );
        if let Err(e) = controller.set_config(&conf) {
            warn!("wifi: set_config failed: {e:?}");
            wait_change(&mut changed, RETRY, |k| k.starts_with("wifi.")).await;
            continue;
        }

        info!("wifi: connecting to {ssid}");
        match controller.connect_async().await {
            Ok(_) => {
                info!("wifi: associated, waiting for DHCP");
                stack.wait_config_up().await;
                set_state(|s| {
                    s.up = true;
                    s.ip = stack.config_v4().map(|c| c.address.address());
                });
                led::status(led::Status::Online);
                info!("wifi: up");

                // Connected: refresh RSSI, notice link loss / creds change.
                loop {
                    if wait_change(&mut changed, POLL, |k| k.starts_with("wifi.")).await {
                        break; // creds changed -> outer loop reconfigures
                    }
                    if !controller.is_connected() {
                        info!("wifi: link lost");
                        set_state(|s| {
                            s.up = false;
                            s.ip = None;
                        });
                        break;
                    }
                    if let Ok(r) = controller.rssi() {
                        set_state(|s| s.rssi = r);
                    }
                }
            }
            Err(e) => {
                warn!("wifi: connect failed: {e:?}");
                wait_change(&mut changed, RETRY, |k| k.starts_with("wifi.")).await;
            }
        }
    }
}
