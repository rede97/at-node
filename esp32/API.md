# AT-Node HTTP API Reference

> Version: v1.0
> Base URL: `http://<device-ip>/at-node` or `http://<hostname>.local/at-node`
> All endpoints return JSON unless noted.
> Device discovery: mDNS `<hostname>.local` (e.g., `atnodeesp-c842.local`)

---

## 1. Status & Discovery

### GET /at-node/status

Device status page (HTML).

**Response**: HTML page with embedded JSON data.

### GET /at-node/cmd/status

Device status (pure JSON).

**Response**:
```json
{
  "device": "AT-Node-ESP-5688",
  "hostname": "atnodeesp-5688",
  "connected": false,
  "ip": "192.168.1.27",
  "ble_addr": "88:56:a6:7b:c8:42",
  "typing": false,
  "mqtt": false,
  "ap": false,
  "http_enabled": true
}
```

### GET /at-node/help

API documentation page (HTML).

### Browser UI (single-page app)

`GET /` serves the whole web UI as ONE gzipped single-page app
(`Content-Encoding: gzip`, ~4.5KB from flash, built by `esp32/web/build.py`).
The page covers status dashboard, BLE pairing, MQTT config, rathole tunnel
config, WiFi config and this API catalog; all dynamic content is driven by
the JSON `/at-node/cmd/*` endpoints below — the HTML itself is never
re-requested. Legacy page URLs (`/at-node/status`, `/at-node/pair`,
`/at-node/mqtt`, `/at-node/tunnel`, `/at-node/help`) respond `302` → `/`.

### GET /at-node/help.json

Machine-readable API catalog (same data as MQTT `sys/info` services).

**Response**:
```json
{
  "ok": true,
  "services": {
    "keyboard/tap": {
      "d": "press+release one key",
      "p": {"mods": "modifier mask (0x01=Ctrl 0x02=Shift 0x04=Alt 0x08=GUI 0x10=LCtrl)", "k": "HID keycode (4=a 5=b ... 0x39=CapsLock)", "ms": "hold duration ms, default 100"}
    },
    "keyboard/text": {"d": "type ASCII string via BLE", "p": {"s": "ASCII text to type", "ms": "per-key hold ms, default 60", "gap": "inter-key gap ms, default 80"}},
    "gpio/write": {"d": "set GPIO output level", "p": {"pin": "GPIO number (0-10, 18, 19, 20, 21)", "level": "0=LOW 1=HIGH"}},
    "...": "17 services total"
  }
}
```

> Agent 可通过此端点或 MQTT `sys/info` RPC 自动发现全部 API 及参数。

---

## 2. Raw AT Command

### POST /at-node/at

Execute raw AT command.

**Content-Type**: `text/plain`

**Body**: `AT+<command>[=<args>]`

**Response**:
```json
{"ok": true, "response": "OK"}
```

**Supported commands** (the HTTP raw-AT endpoint supports this subset; the full
AT set — `AT+VER`, `AT+SET/GET/KEYS`, `AT+TUNNEL`, `AT+MOD`, `AT+KEY_SEQ`,
`AT+BT_*`, ... — is available on the USB serial port):
- `AT`
- `AT+TAP=<ms>,<mods>,<key>`
- `AT+KEY=<mods>,<k0>,<k1>,...,<k5>`
- `AT+TEXT=<string>`
- `AT+CONF=<key>=<value>`
- `AT+GPIO_W=<pin>,<level>`
- `AT+GPIO_R=<pin>`
- `AT+ADC=<ch>`
- `AT+I2C_SCAN`
- `AT+I2C_R=<addr>,<reg>,<len>`
- `AT+I2C_W=<addr>,<reg>,<data>`
- `AT+IR=<protocol>,<data>[,<bits>]`
- `AT+MQTT=<sub>,<value>`
- `AT+WIFI=<sub>,<value>`
- `AT+HTTP=<status|enable,<0|1>|clear|0|1>`
- `AT+PAIR=<1|0|status>`
- `AT+NVS=clear`
- `AT+AP=<0|1>`

---

## 3. Keyboard

### POST /at-node/cmd/keyboard/tap

Tap a single key (press + release).

**Params** (query string or form):
- `mods` (uint8): modifier keys bitmask (0=none, 1=LCtrl, 2=LShift, 4=LAlt, 8=LGUI)
- `k` (uint8): key code (USB HID)
- `ms` (int): press duration in milliseconds (default: 100)

**Response**:
```json
{"ok": true, "cmd": "keyboard/tap", "ms": 100}
```

### POST /at-node/cmd/keyboard/text

Type a string.

**Params**:
- `s` (string): text to type
- `ms` (int): per-key press duration (default: 40)
- `gap` (int): gap between characters (default: 30)

**Response**:
```json
{"ok": true, "cmd": "keyboard/text", "queued": true}
```

### POST /at-node/cmd/keyboard/key

Send raw HID report.

**Params**:
- `mods` (uint8): modifier keys
- `k0`..`k5` (uint8): key codes (0=none)

**Response**:
```json
{"ok": true, "cmd": "keyboard/key"}
```

---

## 4. Peripherals

### POST /at-node/cmd/gpio/write

Write GPIO output.

**Params**:
- `pin` (int): GPIO pin number (0-48)
- `level` (int): 0=low, 1=high

**Response**:
```json
{"ok": true, "cmd": "gpio/write", "pin": 2, "level": 1}
```

### POST /at-node/cmd/gpio/read

Read GPIO input.

**Params**:
- `pin` (int): GPIO pin number

**Response**:
```json
{"ok": true, "cmd": "gpio/read", "pin": 2, "level": 1}
```

### POST /at-node/cmd/adc/read

Read ADC value.

**Params**:
- `ch` (int): ADC channel (0-7)

**Response**:
```json
{"ok": true, "cmd": "adc/read", "ch": 0, "mv": 592}
```

### POST /at-node/cmd/i2c/scan

Scan I2C bus.

**Response**:
```json
{"ok": true, "cmd": "i2c/scan", "devices": ["0x50", "0x68"]}
```

### POST /at-node/cmd/i2c/read

Read I2C device register.

**Params**:
- `addr` (hex string): 7-bit device address (e.g., "0x50")
- `reg` (hex string): register address (e.g., "0x00")
- `len` (int): bytes to read (1-32)

**Response**:
```json
{"ok": true, "cmd": "i2c/read", "addr": "0x50", "reg": "0x0", "data": "A5"}
```

### POST /at-node/cmd/i2c/write

Write I2C device register.

**Params**:
- `addr` (hex string): device address
- `reg` (hex string): register address
- `data` (hex string): bytes to write (e.g., "A5B6")

**Response**:
```json
{"ok": true, "cmd": "i2c/write", "addr": "0x50", "reg": "0x0"}
```

### POST /at-node/cmd/ir/send

Send IR signal.

**Params**:
- `protocol` (string): NEC, SIRC, or RAW
- `data` (string): protocol data (hex for NEC/SIRC, comma-separated timings for RAW)
- `bits` (int): bit count for SIRC (default: 32)

**Response**:
```json
{"ok": true, "cmd": "ir/send", "protocol": "NEC"}
```

---

## 5. WiFi Configuration

### POST /at-node/cmd/wifi/config

Configure WiFi credentials.

**Params**:
- `ssid` (string): WiFi SSID
- `pass` (string): WiFi password

**Response**:
```json
{"ok": true, "cmd": "wifi/config", "ssid": "MyNetwork"}
```

**Note**: Changes persist in NVS. Reboot to apply (or use AT+RST).

---

## 6. MQTT

### GET /at-node/cmd/mqtt/status

MQTT connection status.

**Response**:
```json
{
  "connected": true,
  "broker": "122.51.226.5",
  "port": 8883,
  "client_id": "atnode-atnodeesp-5688",
  "ca_fp": "E1:82:7D:...:40:02:A9",
  "auto": true
}
```

Fields:
- `ca_fp`: SHA256 fingerprint used for TLS verification (empty if plain TCP)
- `auto`: whether auto-reconnect on boot is enabled (NVS `mqtt_auto`)

### POST /at-node/cmd/mqtt/config

Configure MQTT broker.

**Params**:
- `broker` (string): broker hostname/IP
- `port` (int): broker port (1883=plain, 8883=TLS)
- `user` (string): username (optional)
- `pass` (string): password (optional)
- `auto` (int): `1` = auto-connect on boot (NVS)

**Response**:
```json
{"ok": true, "cmd": "mqtt/config"}
```

### POST /at-node/cmd/mqtt/clear

Wipe all MQTT settings (NVS + runtime) and disconnect.

**Response**:
```json
{"ok": true, "cmd": "mqtt/clear"}
```

**Browser UI**: `GET /at-node/mqtt` — form for broker/credentials/fingerprint/auto.

### POST /at-node/cmd/mqtt/ca

Set SHA256 fingerprint for TLS certificate verification.

**Params**:
- `fp` (string): SHA256 fingerprint — 64 hex chars (colons optional)

**Response**:
```json
{"ok": true, "cmd": "mqtt/ca"}
```

**Notes**:
- Only SHA256 fingerprint is supported (no full CA/PEM).
- The device uses `setInsecure()` for the TLS handshake, then verifies the
  peer certificate's SHA256 hash against the stored fingerprint post-connect.
- Fingerprint is persisted in NVS (`mqtt_ca_fp`).
- Generate with: `openssl x509 -in server.crt -noout -fingerprint -sha256`

### POST /at-node/cmd/mqtt/connect

Connect to MQTT broker (queued, non-blocking).

**Response**:
```json
{"ok": true, "cmd": "mqtt/connect", "queued": true}
```

### POST /at-node/cmd/mqtt/publish

Publish MQTT message.

**Params**:
- `topic` (string): MQTT topic
- `msg` (string): message payload

**Response**:
```json
{"ok": true, "cmd": "mqtt/publish"}
```

### POST /at-node/cmd/mqtt/subscribe

Subscribe to MQTT topic.

**Params**:
- `topic` (string): MQTT topic

**Response**:
```json
{"ok": true, "cmd": "mqtt/subscribe"}
```

---

## 7. HTTP Configuration

> **Security policy**: The HTTP control plane has **no authentication** and is intended for
> **trusted local NAT networks only**. On untrusted networks, disable it with `AT+HTTP=0`
> (or `POST /at-node/cmd/http/config` with `enable=0`) and use the MQTT (TLS) control plane instead.

### GET /at-node/cmd/http/status

Read HTTP server state.

**Response**:
```json
{"ok": true, "cmd": "http/status", "enabled": true}
```

### POST /at-node/cmd/http/config

Enable or disable the HTTP control plane.

**Params**:
- `enable` (int): `1` to enable HTTP, `0` to disable

**Response**:
```json
{"ok": true, "cmd": "http/config", "enabled": false}
```

### POST /at-node/cmd/http/clear

Reset the HTTP setting to default (enabled) and remove it from NVS.

**Response**:
```json
{"ok": true, "cmd": "http/clear"}
```

**Notes**:
- Settings are persisted in NVS as `http_enable`.
- Disabling HTTP takes effect immediately; the listening socket is closed.
- You can also toggle it via raw AT:
  - `AT+HTTP=0` / `AT+HTTP=1` (shorthand)
  - `AT+HTTP=enable,0` / `AT+HTTP=enable,1`
  - `AT+CONF=http_enable=0` / `AT+CONF=http_enable=1`
- Read state via `AT+HTTP=status`.
- Reset to default via `AT+HTTP=clear`.
- Re-enable only via serial, AP portal, or MQTT (since the HTTP endpoint is no longer reachable once disabled).

---

## 8. BLE Advertising

Public BLE advertising is **off by default** for security. The device is not discoverable until you explicitly enter pairing mode.

### GET /at-node/cmd/ble/status

Returns BLE state including `advertising` (currently broadcasting publicly), `pairing_mode`, and `pair_timeout_ms` when public pairing mode is running.

### POST /at-node/cmd/ble/pair

Enter or exit **public pairing mode**.

**Params**:
- `enable` (int): `1` to enter pairing mode, `0` to exit

**Response**:
```json
{"ok": true, "cmd": "ble/pair", "advertising": true, "pairing_mode": true}
```

**Behavior**:
- Pairing mode is a **runtime state only**; it is not persisted to NVS.
- Public advertising automatically stops after **60 seconds** if no host connects/pairs.
- Once a host is bonded, the device advertises **privately/directed** to that bonded host after disconnect (or on boot). Only that host can reconnect; the device is not publicly discoverable.

**AT equivalents**:
- `AT+PAIR=1` — enter public pairing mode for 60s
- `AT+PAIR=0` — exit pairing mode immediately
- `AT+PAIR=status`

**MQTT equivalent**: `ble/pair?enable=1|0`

---

## 9. NVS / Factory Reset

### POST /at-node/cmd/nvs/clear

Erase all persisted settings in the `atnode` NVS namespace and restart the device.

**Response** (sent before restart):
```json
{"ok": true, "cmd": "nvs/clear", "restarting": true}
```

**AT equivalent**: `AT+NVS=clear`

**Warning**: This clears WiFi credentials, MQTT config, device name, hostname, HTTP setting, etc. The device will reboot with factory defaults (BLE bonding managed by NimBLE separately).

---

## 10. Unified Configuration

Every persistent setting lives behind one registry: `config_set(key, val)` /
`config_get(key)` / `config_list_json()`. Serial AT, HTTP and MQTT all delegate
to it, and legacy domain commands (`AT+WIFI=`, `AT+MQTT=`, `AT+HTTP=`,
`AT+CONF=`, `/at-node/cmd/wifi/config`, `/at-node/cmd/mqtt/config`, ...) are
thin aliases over the same registry. NVS keys are unchanged.

**Key space**:

| Key | Notes |
|-----|-------|
| `device.name` | BLE device name |
| `device.hostname` | mDNS hostname |
| `wifi.ssid` / `wifi.pass` | WiFi credentials (pass is write-only) |
| `mqtt.broker` / `mqtt.port` / `mqtt.user` / `mqtt.pass` / `mqtt.ca` / `mqtt.auto` | pass is write-only |
| `http.enable` | `1`/`0`, takes effect immediately |
| `rathole.enable` | rathole master switch, `1`/`0` |
| `tunnel.1.server` / `.token` / `.service` / `.local` / `.auto` / `.retry` / `.enable` | single tunnel only; token is write-only; retry = reconnect backoff base 1-60 s; enable = per-tunnel persisted switch |

### POST /at-node/cmd/config

Set a config value.

**Params**:
- `key` (string): config key from the table above
- `val` (string): value

**Response**:
```json
{"ok": true, "cmd": "config", "key": "tunnel.1.retry"}
```

### GET /at-node/cmd/config

Read a config value. **Params**: `key`. Secret keys return an empty value.

```json
{"ok": true, "key": "device.name", "value": "AT-Node-ESP-5688"}
```

### GET /at-node/cmd/config/list

List all config keys (secret keys marked, values omitted).

```json
{"ok": true, "keys": [{"key": "wifi.pass", "secret": true}, {"key": "mqtt.broker", "value": "122.51.226.5"}, ...]}
```

**Serial equivalents**: `AT+SET=<key>=<val>` / `AT+GET=<key>` / `AT+KEYS`.
**MQTT equivalents**: `config/set` (`key,val`), `config/get` (`key`), `config/list`.

---

## 11. rathole Tunnels

Reverse-tunnel client compatible with [rathole](https://github.com/rapiz1/rathole)
(protocol v1, **plain TCP transport** — tunnel only protocols that carry their
own encryption like SSH/HTTPS, or bind the service to `127.0.0.1` on the
rathole server). **Single tunnel** (id `1` — one SSH session can jump further;
less public exposure, less RAM), NVS-persisted, optional autostart.

### GET /at-node/cmd/tunnel/status

Tunnel state (one-element array).

```json
{"ok": true, "tunnels": [
  {"id": 1, "configured": true, "server": "192.168.1.7:2333", "service": "c3http",
   "local": "127.0.0.1:80", "auto": false, "retry": 5, "master": true,
   "enabled": true, "running": true, "connected": true, "pool": 1,
   "data_channels": 1, "free_heap": 24232, "last_error": ""}
]}
```

**Switch hierarchy**: `master` (global, `rathole.enable`) && per-tunnel `enabled`
&& `auto` together decide boot autostart; `master` or `enabled` off makes
`connect` fail immediately. `enabled` off stops a running tunnel right away.

### POST /at-node/cmd/tunnel/config

**Params**: `id` (always `1`), plus any of `server`, `token`, `service`, `local`,
`auto` (`1`=connect at boot), `retry` (reconnect backoff base, seconds, 1-60),
`enable` (`1|0` per-tunnel persisted switch).
Empty fields keep their current value. A running tunnel restarts on change.

### POST /at-node/cmd/tunnel/enable

Global master switch (NVS). **Params**: `enable=1|0`. When disabled all tunnels
stop and none auto-start at boot.

### POST /at-node/cmd/tunnel/connect | disconnect | clear

**Params**: `id`. `clear` also wipes the tunnel's NVS keys.

**Browser UI**: `GET /at-node/tunnel`.
**Serial**: `AT+TUNNEL=enable,<0|1>`, `AT+TUNNEL=<id>,server|token|service|local|auto|retry,<val>`,
`AT+TUNNEL=<id>,connect|disconnect|clear|status`, `AT+TUNNEL=status`.
**MQTT**: `tunnel/status`, `tunnel/config`, `tunnel/enable`, `tunnel/connect`,
`tunnel/disconnect`, `tunnel/clear`.

---

## 12. AP Portal

### POST /at-node/cmd/ap

Start/stop AP mode.

**Params**:
- `1`: start AP mode
- `0`: stop AP mode

**Response**:
```json
{"ok": true, "cmd": "ap"}
```

**AP details**:
- SSID: `AT-NODE-{device-name}`
- Password: `ATNODECFG`
- IP: `192.168.4.1`
- Portal: `http://192.168.4.1:8080`

---

## Error Responses

All endpoints return consistent error format:

```json
{"ok": false, "error": "error message"}
```

Common HTTP status codes:
- `200`: Success
- `400`: Bad request (missing/invalid params)
- `404`: Not found
- `409`: Conflict (e.g., BLE not connected, MQTT not connected)
- `423`: Locked (e.g., typing in progress)
- `500`: Internal error

---

## Examples

### Tap key 'a' (0x04)
```bash
curl -X POST "http://atnodeesp-c842.local/at-node/cmd/keyboard/tap?mods=0&k=4&ms=100"
```

### Type text
```bash
curl -X POST "http://atnodeesp-c842.local/at-node/cmd/keyboard/text?s=Hello&ms=60&gap=100"
```

### Raw AT command
```bash
curl -X POST -d "AT+TAP=100,0,4" http://atnodeesp-c842.local/at-node/at
```

### Get JSON status
```bash
curl http://atnodeesp-c842.local/at-node/cmd/status
```

### Configure WiFi
```bash
curl -X POST "http://atnodeesp-c842.local/at-node/cmd/wifi/config?ssid=MyNetwork&pass=MyPassword"
```

### Configure MQTT with fingerprint
```bash
curl -X POST "http://atnodeesp-c842.local/at-node/cmd/mqtt/ca?fp=e1827db813ffdbb6dea1d3da3c726271179b227293d2090c72beb02ea74002a9"
curl -X POST "http://atnodeesp-c842.local/at-node/cmd/mqtt/connect"
```
