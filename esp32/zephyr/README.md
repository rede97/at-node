# esp32/zephyr/ — ESP32-S3 Zephyr variant (AT-Node)

> Zephyr-based AT Node for high-performance ESP32 chips. Primary target:
> **MuseLab nanoESP32-S3** (ESP32-S3-WROOM-1-N8R8, 8MB flash + 8MB octal PSRAM).
> Replaces Arduino-ESP32 for S3 — root cause: [../COMPAT_REPORT.md](../COMPAT_REPORT.md) §D2.
> AT semantics shared with [../arduino/](../arduino/) (reference) and wchble/mr2.

## Status: ✅ Active (build-verified; hardware smoke pending)

| Subsystem | State |
|---|---|
| WiFi STA + 15s reconnect watchdog | ✅ (wifi_sta.c, LED blue-blink/green) |
| HTTP control plane `/at-node/*` + shared gzip SPA | ✅ (httpd.c, SPA from ../arduino/web_page.h unmodified) |
| MQTT over TLS (8883, CA strong-verify) / plain (1883) | ✅ (mqttc.c, LWT + retained info/state) |
| BLE HID boot keyboard (Zephyr native host + ESP32 BT controller) | ✅ build-verified, hardware test pending |
| USB HID keyboard (DWC2 OTG, usb_device_next) | ✅ build-verified, **hardware test pending** (user) |
| AT core over UART0 + HTTP `/at-node/at` + MQTT `cmd` topic | ✅ |
| Config registry (settings/NVS): `AT+SET/GET/KEYS`, config endpoints | ✅ (cfg.c, keys below) |
| GPIO / ADC (ch0-9=GPIO1-10) / I2C (SDA=8, SCL=9) | ✅ (hws.c) |
| WS2812 status LED @ GPIO48 | ✅ presets + free color via `AT+LED=r,g,b` |
| IR / rathole / AP portal / mDNS | ❌ not ported (see Deltas) |

## Build / flash

```bash
esp32/zephyr/tools/build.sh            # build -> build_zephyr/
esp32/zephyr/tools/build.sh flash      # build + flash (PORT=/dev/ttyACMx)
```

Requires the Zephyr workspace (`~/zephyrproject/.venv` on PATH) and
`ZEPHYR_SDK_INSTALL_DIR`; the wrapper sets both. Board target:
`nano_esp32s3/esp32s3/procpu` (board definition in `boards/`).

MQTT TLS CA: `tools/gen_certs.sh <broker-LAN-IP>` (copied from the nano demo)
generates `src/ca_cert.h` (gitignored). Without it the build still succeeds but
TLS falls back to `TLS_PEER_VERIFY_NONE` with a boot-time warning.

## Architecture

```
AT line ──► at_core (at_handle_line, mutex-serialized, emit per line)
   ▲  ▲  ▲            │  final line "OK" | "ERROR <reason>"
   │  │  │            ▼
   │  │  └─ mqttc (atnode/<name>/cmd → resp, TLS, reconnect thread)
   │  └──── httpd (POST /at-node/at, /at-node/cmd/*, GET / gzip SPA)
   └─────── at_serial (UART0 console, echo, 300-char line buf)

kbd.c routing: AT+DEV=USB|BLE|ALL bitmask ──► kbd_usb_send / kbd_ble_send
kbd_tap / kbd_type_text run on a sequence thread (atomic press+release, F18).

cfg.c: settings (NVS, storage partition) registry; cfg_set persists at once;
node_cfg_changed() in main.c fans key changes out to wifi/mqtt/http/ble.
```

Init order (main.c): LED → cfg → kbd seq thread → hws → WiFi watchdog →
USB HID → BLE (ble.auto) → HTTP (http.auto) → MQTT (mqtt.auto && mqtt.enable)
→ serial AT.

## Config keys (cfg registry)

`device.name` (default `AT-Node-S3-XXXX`, XXXX = efuse MAC tail),
`wifi.ssid`, `wifi.pass`(WO), `mqtt.broker|port|user|pass(WO)|auto|enable`,
`http.auto|enable`, `ble.auto|enable`.

- AT: `AT+SET=k=v` / `AT+GET=k` / `AT+KEYS`; HTTP: `/at-node/cmd/config[/list]`;
  write-only keys read back as `-EACCES`/`***`.
- Runtime enable/auto semantics follow the Arduino variant (§统一配置层).

## AT commands (UART0 115200, HTTP /at-node/at, MQTT cmd topic — identical)

`AT` `AT+VER` `AT+HELP` `AT+STATUS` `AT+ABILITY` ·
`AT+TAP=ms,mods,key` `AT+KEY=mods,k1..k6` (raw press — pair with `AT+KEY=0,0`)
`AT+KEY_STR=text` `AT+KEY_SEQ=...` · `AT+DEV=USB|BLE|ALL` ·
`AT+GPIO_W/R` `AT+ADC=ch` `AT+I2C_SCAN/R/W` ·
`AT+WIFI=ssid|pass|status` `AT+MQTT=connect|disconnect|status|enable,|auto,`
`AT+HTTP=status|enable,|auto,` · `AT+PAIR=1` (60s window) `AT+PAIR=0|status`
`AT+UNPAIR` · `AT+LED=r,g,b|off|auto` · `AT+SET/GET/KEYS` `AT+NVS=clear` `AT+RST`

## BLE pairing policy (Arduino parity)

Default: **no open advertising**. `AT+PAIR=1` / `POST /at-node/cmd/ble/pair?enable=1`
opens a 60s bondable window (JustWorks). With bonds on record and no window,
advertising uses the filter accept list — only bonded hosts can reconnect.
Bonds persist in flash (BT_SETTINGS). `AT+UNPAIR` clears all bonds.

## USB HID keyboard

Standard 8-byte boot-keyboard report on the USB OTG (DWC2, GPIO19/20) port;
enumerates as "AT-Node Keyboard" (VID 0x2FE3 / PID 0x0007). Routed by `AT+DEV`.
The console stays on UART0/ESPLink — no conflict with the native USB-JTAG.
Notes from the port: ESP32-S3 DWC2 defaults to internal-DMA mode
("Experimental DMA enabled"); if unstable set `CONFIG_UDC_DWC2_DMA=n`.
**Enumeration on a real host is untested — pending user hardware test.**

## HTTP API

Route table identical to the Arduino variant (../arduino/API.md): status,
ability, help.json, at, keyboard/{tap,text,key}, gpio/{write,read}, adc/read,
i2c/{scan,read,write}, config[/list], ble/{pair,status,bonds/delete,bonds/clear},
mqtt/{status,config,connect,clear,ca}, wifi/config, nvs/clear.
`mqtt/ca` accepts and ignores the fingerprint (`{"ok":true,"note":...}`) —
Zephyr verifies against the embedded CA cert instead.
Response envelope `{"ok":bool,...}`; unknown path → 404 `{"ok":false,...}`.

## MQTT topics

`atnode/<device.name>/cmd` (sub, raw AT line) · `.../resp` (AT response) ·
`.../state` (retained online/offline, LWT) · `.../info` (retained manifest).

## Deltas vs the Arduino variant (intentional)

- BLE host is Zephyr's native stack (not NimBLE) — Z1.3 目标改用原生 host。
- MQTT TLS uses an **embedded CA cert** (`src/ca_cert.h`), not SHA256 fingerprint.
- **IR 暂不实现**（决定，2026-08-20）：Zephyr esp32 port 无 RMT 驱动
  （硬件存在，缺的是驱动——上游通用 `pulse_io` 子系统仍在 RFC 阶段，
  zephyrproject-rtos/zephyr#109586；WS2812 走 SPI 与 RMT 无关）。
  等 pulse_io 落地或确有需求时再评估（LEDC PWM 38kHz 载波为备选路径）。
- No rathole tunnel, no AP captive portal (WiFi creds via AT+SET/HTTP config), no mDNS.
- `AT+LED=r,g,b|off|auto` free-color WS2812 control (user requirement).
- Status JSON omits heap/temp_c (SPA guards both as optional).

## Memory budget (nanoESP32-S3)

FLASH 977KB/8MB (11.7%), DRAM 396KB/399KB (99.2% — tight by design:
90KB k_malloc heap + 60KB mbedTLS heap + BT/WiFi blobs), IRAM 97KB/415KB.
PSRAM (8MB octal, 40MHz) available via shared_multi_heap for future big
assets (Z1.4). Raise `CONFIG_HEAP_MEM_POOL_SIZE`/`MBEDTLS_HEAP_SIZE` with
care — DRAM is nearly full.

## Debugging

Serial open resets the board (ESPLink DTR/RTS) — use a persistent console
(`tools/at_console.py` here, or the demo's serial_console.py).
Full JTAG/WiFi/TLS/PSRAM pit list: nano_esp32s3_demo `docs/DEBUGGING.md`.

JTAG note: while the firmware owns the USB OTG PHY, the ROM USB-Serial-JTAG
(303a:1001) loses the PHY — for openocd debugging build with
`-DEXTRA_DTC_OVERLAY_FILE=triage_no_usb.overlay` (kbd_usb.c compiles to stubs).

## Known issues (as of 2026-08-20)

- **MQTT reconnect leaks net connection contexts**: repeated broker
  connection failures eventually exhaust CONFIG_NET_MAX_CONN
  ("Not enough connection contexts"); mqttc retry path must close the
  socket of a failed mqtt_connect (mqtt_abort/close) before retrying.
- MQTT cmd->resp roundtrip not yet hardware-verified (TLS connect itself OK).
- BLE pairing/typing and USB HID typing verified at enumeration level only
  (SET_IDLE + LED output report observed); host-side typing untested.

## Bugs hit during bring-up (root causes, for future reference)

| Bug | Root cause | Fix |
|---|---|---|
| WiFi PSK connect -> k_panic ~1s later, crash in innocent esp_timer thread | LED blink did SPI (led_strip_update_rgb) from a k_timer expiry (ISR context), corrupting scheduler interrupt accounting (nested leak); z_pend_curr ISR guard panicked later in an unrelated thread | Blink deferred to system workqueue (led.c). NEVER do SPI/I2C/UART from k_timer/ISR context |
| MQTT TLS handshake always -0x7F80 (HW_ACCEL_FAILED) | BT_ECC implies MBEDTLS_PSA_P256M_DRIVER_ENABLED; ECDHE routed to p256m breaks the handshake on S3 | CONFIG_MBEDTLS_PSA_P256M_DRIVER_ENABLED=n |
| DRAM segment overflow ~32KB | WiFi+BT blobs + mbedTLS 60K + k_heap 131K exceed 399KB DRAM | k_heap 90K, MAIN_STACK 4K, HTTP clients 2 |
| Link: missing _http_resource_desc_*_list_start | Per-service HTTP resource iterable section must be app-declared | sections-rom.ld + ITERABLE_SECTION_ROM |
| hal bt.c compile errors (Kconfig symbols undefined) | Board dts (from demo, no BT) left esp32_bt_hci disabled -> BT_ESP32 unselected | app.overlay enables the node |
| "Unrecognized USB device" on host / native port vanishes | S3 has ONE USB PHY shared by USB-Serial-JTAG and USB OTG; firmware OTG takes it; after a crash the dead stack fails enumeration | Documented; debug via triage_no_usb.overlay |
| esp_cache_msync null errors | DWC2 experimental DMA mode | CONFIG_UDC_DWC2_DMA=n (slave FIFO) |
| esptool "chip stopped responding" | Persistent serial console holding the port races esptool | Stop console before flashing |
