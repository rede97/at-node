//! AT-Node rust-s3 — SSDP/UPnP discovery (ssdp feature; requires http).
//!
//! Wire behavior mirrored from the esp32_matrix reference sketch: join
//! 239.255.255.250:1900, NOTIFY ssdp:alive ×3 at startup + renewal every
//! MAX_AGE/2, unicast reply to M-SEARCH (ssdp:all / upnp:rootdevice /
//! Basic:1). The UPnP device description is served by httpd at
//! /description.xml; its presentationURL points at the SPA, which is what
//! Windows opens via "View device webpage" in Explorer's Network view.
//!
//! Runtime gate: runs only while the HTTP service is enabled AND WiFi is
//! up (httpd::running() is the master switch; disabling HTTP stops SSDP).

use core::fmt::Write as _;

use heapless::String;

use crate::{cfg, wifi};

pub fn enabled() -> bool {
    cfg!(feature = "ssdp")
}

#[cfg(feature = "ssdp")]
const MCAST: embassy_net::Ipv4Address = embassy_net::Ipv4Address::new(239, 255, 255, 250);
#[cfg(feature = "ssdp")]
const PORT: u16 = 1900;
/// max-age=1800; alive renewal at half that (reference sketch: 900 s).
#[cfg(feature = "ssdp")]
const ALIVE_INTERVAL: embassy_time::Duration = embassy_time::Duration::from_secs(900);

/// Stable UUID from the efuse MAC (reference sketch format).
#[cfg_attr(not(feature = "ssdp"), allow(dead_code))]
fn uuid() -> String<48> {
    let m = esp_hal::efuse::base_mac_address();
    let b = m.as_bytes();
    let mut u: String<48> = String::new();
    let _ = write!(
        u,
        "uuid:38323636-4558-4dda-9188-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        b[0], b[1], b[2], b[3], b[4], b[5]
    );
    u
}

/// UPnP device description (served at /description.xml by httpd).
#[cfg_attr(not(feature = "ssdp"), allow(dead_code))]
pub async fn description_xml() -> String<768> {
    let name = cfg::get_str("device.name").await;
    let uuid = uuid();
    let mut ip_s: String<16> = String::new();
    if let Some(ip) = wifi::ipv4() {
        let _ = write!(ip_s, "{ip}");
    }
    let mut x: String<768> = String::new();
    let _ = write!(
        x,
        "<?xml version=\"1.0\"?>\r\n\
<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\r\n\
\u{20} <specVersion><major>1</major><minor>0</minor></specVersion>\r\n\
\u{20} <device>\r\n\
\u{20}   <deviceType>urn:schemas-upnp-org:device:Basic:1</deviceType>\r\n\
\u{20}   <friendlyName>{name}</friendlyName>\r\n\
\u{20}   <manufacturer>AT-Node</manufacturer>\r\n\
\u{20}   <modelName>atnode-s3</modelName>\r\n\
\u{20}   <modelNumber>1.0</modelNumber>\r\n\
\u{20}   <UDN>{uuid}</UDN>\r\n\
\u{20}   <presentationURL>http://{ip_s}/</presentationURL>\r\n\
\u{20} </device>\r\n\
</root>\r\n"
    );
    x
}

#[cfg(feature = "ssdp")]
pub use driver::task;

#[cfg(feature = "ssdp")]
mod driver {
    use super::*;

    use embassy_futures::select::{Either3, select3};
    use embassy_net::udp::{PacketMetadata, UdpSocket};
    use embassy_net::{IpAddress, IpEndpoint, Stack};
    use embassy_time::{Duration, Instant, Timer};
    use log::{info, warn};

    async fn notify_alive(sock: &mut UdpSocket<'_>, ip_s: &str, uuid: &str) {
        let mut msg: String<512> = String::new();
        let _ = write!(
            msg,
            "NOTIFY * HTTP/1.1\r\n\
HOST: {MCAST}:{PORT}\r\n\
CACHE-CONTROL: max-age=1800\r\n\
LOCATION: http://{ip_s}:80/description.xml\r\n\
NT: upnp:rootdevice\r\n\
NTS: ssdp:alive\r\n\
SERVER: rust-s3/1.0 UPnP/1.1 atnode-s3/1.0\r\n\
USN: {uuid}::upnp:rootdevice\r\n\
\r\n"
        );
        let _ = sock
            .send_to(msg.as_bytes(), IpEndpoint::new(IpAddress::Ipv4(MCAST), PORT))
            .await;
    }

    async fn answer_msearch(
        sock: &mut UdpSocket<'_>,
        buf: &[u8],
        from: embassy_net::udp::UdpMetadata,
        ip_s: &str,
        uuid: &str,
    ) {
        if !buf.starts_with(b"M-SEARCH") {
            return;
        }
        let m = buf.windows(8).any(|w| w == b"ssdp:all")
            || buf.windows(15).any(|w| w == b"upnp:rootdevice")
            || buf.windows(7).any(|w| w == b"Basic:1");
        if !m {
            return;
        }
        let mut resp: String<512> = String::new();
        let _ = write!(
            resp,
            "HTTP/1.1 200 OK\r\n\
CACHE-CONTROL: max-age=1800\r\n\
EXT:\r\n\
LOCATION: http://{ip_s}:80/description.xml\r\n\
SERVER: rust-s3/1.0 UPnP/1.1 atnode-s3/1.0\r\n\
ST: upnp:rootdevice\r\n\
USN: {uuid}::upnp:rootdevice\r\n\
\r\n"
        );
        if sock.send_to(resp.as_bytes(), from).await.is_ok() {
            info!("ssdp: answered M-SEARCH from {from}");
        }
    }

    /// SSDP service task: active only while httpd::running() && WiFi up;
    /// drops the socket and leaves the group when either goes away.
    pub async fn task(stack: Stack<'static>) -> ! {
        static mut RX_META: [PacketMetadata; 4] = [PacketMetadata::EMPTY; 4];
        static mut RX_BUF: [u8; 768] = [0; 768];
        static mut TX_META: [PacketMetadata; 4] = [PacketMetadata::EMPTY; 4];
        static mut TX_BUF: [u8; 768] = [0; 768];

        loop {
            if !crate::httpd::running() || !wifi::link_up() {
                Timer::after(Duration::from_secs(2)).await;
                continue;
            }
            let Some(ip) = wifi::ipv4() else {
                Timer::after(Duration::from_secs(2)).await;
                continue;
            };
            let mut ip_s: String<16> = String::new();
            let _ = write!(ip_s, "{ip}");
            let uuid = uuid();

            if stack.join_multicast_group(MCAST).is_err() {
                warn!("ssdp: join multicast group failed");
                Timer::after(Duration::from_secs(5)).await;
                continue;
            }
            let mut sock = UdpSocket::new(
                stack,
                unsafe { &mut *core::ptr::addr_of_mut!(RX_META) },
                unsafe { &mut *core::ptr::addr_of_mut!(RX_BUF) },
                unsafe { &mut *core::ptr::addr_of_mut!(TX_META) },
                unsafe { &mut *core::ptr::addr_of_mut!(TX_BUF) },
            );
            if sock.bind(PORT).is_err() {
                warn!("ssdp: bind :{PORT} failed");
                Timer::after(Duration::from_secs(5)).await;
                let _ = stack.leave_multicast_group(MCAST);
                continue;
            }
            info!("ssdp: up, uuid={uuid}");
            for _ in 0..3 {
                notify_alive(&mut sock, &ip_s, &uuid).await;
                Timer::after(Duration::from_millis(100)).await;
            }

            let mut buf = [0u8; 512];
            let mut last_alive = Instant::now();
            loop {
                let remaining = ALIVE_INTERVAL
                    .checked_sub(last_alive.elapsed())
                    .unwrap_or(Duration::from_ticks(0));
                match select3(
                    sock.recv_from(&mut buf),
                    Timer::after(remaining),
                    Timer::after(Duration::from_secs(2)),
                )
                .await
                {
                    Either3::First(Ok((n, from))) => {
                        info!("ssdp: rx {n} bytes from {from}");
                        answer_msearch(&mut sock, &buf[..n], from, &ip_s, &uuid).await;
                    }
                    Either3::First(Err(_)) => {}
                    Either3::Second(()) => {
                        notify_alive(&mut sock, &ip_s, &uuid).await;
                        last_alive = Instant::now();
                    }
                    Either3::Third(()) => {
                        if !crate::httpd::running() || !wifi::link_up() {
                            break;
                        }
                    }
                }
            }
            info!("ssdp: down (http/wifi)");
            drop(sock);
            let _ = stack.leave_multicast_group(MCAST);
        }
    }
}
