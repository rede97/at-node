//! AT-Node rust-s3 — serial (UART0, 115200) AT console.
//!
//! Line discipline mirrors esp32/zephyr/src/at_serial.c (itself mirroring
//! the Arduino handle_serial()): printable chars echoed and accumulated
//! (300-char buffer), CR or LF executes (the LF of a CRLF pair is
//! swallowed), Ctrl-C cancels the line being typed, backspace edits,
//! overflow drops garbage. Responses go out as line + CRLF.

use embassy_executor::Spawner;
use esp_hal::Async;
use esp_hal::peripherals::{GPIO43, GPIO44, UART0};
use esp_hal::uart::{Config, Uart, UartTx};

use crate::at::{self, AtSink};

const LINE_MAX: usize = 300;

struct UartSink<'a> {
    tx: &'a mut UartTx<'static, Async>,
}

/// write_async may return short writes once the TX FIFO fills; loop until
/// the whole buffer is out (dropped tail truncated the KEYS json at ~FIFO).
async fn write_all(tx: &mut UartTx<'static, Async>, mut buf: &[u8]) {
    while !buf.is_empty() {
        match tx.write_async(buf).await {
            Ok(0) | Err(_) => break,
            Ok(n) => buf = &buf[n..],
        }
    }
}

impl AtSink for UartSink<'_> {
    async fn emit(&mut self, line: &str) {
        write_all(self.tx, line.as_bytes()).await;
        write_all(self.tx, b"\r\n").await;
    }
}

/// Configure UART0 and spawn the console task. Call once from main.
pub fn init(spawner: Spawner, uart: UART0<'static>, rx: GPIO44<'static>, tx: GPIO43<'static>) {
    spawner.spawn(serial_task(uart, rx, tx).expect("spawn serial task"));
}

#[embassy_executor::task]
async fn serial_task(uart: UART0<'static>, rx_pin: GPIO44<'static>, tx_pin: GPIO43<'static>) {
    let uart = Uart::new(uart, Config::default())
        .expect("uart0 config")
        .with_rx(rx_pin)
        .with_tx(tx_pin)
        .into_async();
    let (mut rx, mut tx) = uart.split();

    {
        let mut sink = UartSink { tx: &mut tx };
        sink.emit("AT-Node rust-s3 ready, AT+HELP for cmds").await;
    }

    let mut line = [0u8; LINE_MAX];
    let mut len = 0usize;
    let mut skip_lf = false;
    let mut ch = [0u8; 1];

    loop {
        if rx.read_async(&mut ch).await.is_err() {
            continue;
        }
        let c = ch[0];

        if c == 0x03 {
            // Ctrl-C: cancel the line being typed.
            len = 0;
            skip_lf = false;
            let mut sink = UartSink { tx: &mut tx };
            sink.emit("^C").await;
            continue;
        }

        if c == b'\r' || c == b'\n' {
            if c == b'\n' && skip_lf {
                // LF of a CRLF pair.
                skip_lf = false;
                continue;
            }
            skip_lf = c == b'\r';
            let _ = tx.write_async(b"\r\n").await;
            // Trim trailing spaces/tabs (Arduino line.trim()).
            while len > 0 && (line[len - 1] == b' ' || line[len - 1] == b'\t') {
                len -= 1;
            }
            if len > 0
                && let Ok(s) = core::str::from_utf8(&line[..len])
            {
                let mut sink = UartSink { tx: &mut tx };
                at::handle_line(s, &mut sink).await;
            }
            len = 0;
            continue;
        }
        skip_lf = false;

        if c == 0x08 || c == 0x7F {
            // Backspace / DEL.
            if len > 0 {
                len -= 1;
                let _ = tx.write_async(b"\x08 \x08").await;
            }
            continue;
        }

        if c < 0x20 {
            continue; // drop other control chars
        }

        if len < LINE_MAX {
            line[len] = c;
            len += 1;
            let _ = tx.write_async(&[c]).await; // echo
        } else {
            len = 0; // overflow: drop garbage (Arduino semantics)
        }
    }
}
