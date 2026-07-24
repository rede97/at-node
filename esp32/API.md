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
  "ap": false
}
```

### GET /at-node/help

API documentation page (HTML).

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

**Supported commands**:
- `AT`
- `AT+STATUS`
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
  "broker": "192.168.1.7",
  "port": 8883,
  "client_id": "atnode-atnodeesp-5688",
  "ca_type": "fingerprint"
}
```

### POST /at-node/cmd/mqtt/config

Configure MQTT broker.

**Params**:
- `broker` (string): broker hostname/IP
- `port` (int): broker port (1883=plain, 8883=TLS)
- `user` (string): username (optional)
- `pass` (string): password (optional)

**Response**:
```json
{"ok": true, "cmd": "mqtt/config"}
```

### POST /at-node/cmd/mqtt/ca

Configure MQTT CA certificate or SHA256 fingerprint.

**Params**:
- `plain` (text): CA certificate PEM (starts with `-----BEGIN`) or SHA256 fingerprint (64 hex chars)
- `fp` (string): SHA256 fingerprint (alternative to plain)

**Response**:
```json
{"ok": true, "cmd": "mqtt/ca"}
```

**Auto-detection**:
- Starts with `-----BEGIN` → CA certificate (PEM)
- Otherwise → SHA256 fingerprint

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

## 7. Device Configuration

### POST /at-node/cmd/config/set

Set device configuration (NVS persistent).

**Params** (JSON body or query):
- `name`: BLE device name
- `hostname`: mDNS hostname
- `wifi_ssid`: WiFi SSID
- `wifi_pass`: WiFi password
- `mqtt_broker`: MQTT broker
- `mqtt_port`: MQTT port
- `mqtt_user`: MQTT username
- `mqtt_pass`: MQTT password

**Response**:
```json
{"ok": true, "cmd": "config/set"}
```

---

## 8. AP Portal

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
