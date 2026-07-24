/*
 * esp32_at_node.ino
 *
 * ESP32-C3 AT Node — network-enabled BLE HID keyboard peripheral.
 * Compatible command semantics with CH582 AT Node, over WiFi HTTP.
 *
 * Features:
 *   - BLE HID keyboard (boot protocol, 8-byte reports)
 *   - WiFi HTTP control plane on /at-node/*
 *   - USB serial fallback with full AT command set
 *   - NVS-based device configuration
 *
 * HTTP endpoints (base path /at-node):
 *   GET  /at-node/status
 *   POST /at-node/at              (raw AT command, text/plain)
 *   POST /at-node/cmd/keyboard/tap
 *   POST /at-node/cmd/keyboard/text
 *   POST /at-node/cmd/keyboard/key
 *
 * See esp32/PLAN.md for full routing design.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ping/ping_sock.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Wire.h>
#include <PubSubClient.h>
#include <NimBLEDevice.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include "ap_portal.h"
#include <NimBLEServer.h>
#include <NimBLEHIDDevice.h>
#include <NimBLECharacteristic.h>
#include "wifi_config.h"

/* --- device configuration --------------------------------------------- */
#define DEFAULT_DEVICE_NAME "AT-Node-ESP"
#define DEFAULT_HOSTNAME    "atnodeesp"

Preferences prefs;
String g_device_name;
String g_hostname;

static String get_default_name(void)
{
    uint64_t mac = ESP.getEfuseMac();
    char suffix[8];
    sprintf(suffix, "%04X", (uint16_t)(mac & 0xFFFF));
    return String(DEFAULT_DEVICE_NAME) + "-" + String(suffix);
}

static String get_default_hostname(void)
{
    uint64_t mac = ESP.getEfuseMac();
    char suffix[8];
    sprintf(suffix, "%04X", (uint16_t)(mac & 0xFFFF));
    return String(DEFAULT_HOSTNAME) + "-" + String(suffix);
}

/* --- BLE globals ------------------------------------------------------ */
static NimBLEServer*        g_server        = nullptr;
static NimBLEHIDDevice*     g_hid           = nullptr;
static NimBLECharacteristic* g_inputReport  = nullptr;
static NimBLECharacteristic* g_outputReport = nullptr;

static struct {
    uint8_t mods;
    uint8_t keys[6];
} g_key_state;

/* --- HTTP globals ----------------------------------------------------- */
static WebServer g_http(80);
static bool      g_wifi_ready = false;

/* --- MQTT client ------------------------------------------------------- */
static WiFiClient       g_mqtt_wifi_plain;
static WiFiClientSecure g_mqtt_wifi_secure;
static PubSubClient     g_mqtt(g_mqtt_wifi_plain);
static bool             g_mqtt_connected = false;
static bool             g_mqtt_connect_pending = false;
static String           g_mqtt_broker;
static TaskHandle_t     g_mqtt_task = NULL;
static int              g_mqtt_port = 8883;
static String           g_mqtt_user;
static String           g_mqtt_pass;
static String           g_mqtt_client_id;
static String           g_mqtt_topic_prefix;
static String           g_mqtt_ca_cert;
static String           g_mqtt_ca_fp;
static String           g_wifi_ssid;
static String           g_wifi_pass;

/* --- typing queue (non-blocking) -------------------------------------- */
static String   g_type_text;
static size_t   g_type_idx  = 0;
static int      g_type_ms   = 40;
static int      g_type_gap  = 30;
static uint32_t g_type_next = 0;
static bool     g_type_busy = false;

/* --- keyboard report map (report protocol, Report IDs) ---------------- */
/* ID 1 = keyboard input (8 bytes: mods, reserved, 6 keys)                 */
/* ID 2 = LED output (1 byte)                                             */
static const uint8_t REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x85, 0x01,                     /* Report ID 1 */
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0x85, 0x02,                     /* Report ID 2 */
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0xC0
};

/* --- modifier table ---------------------------------------------------- */
static const uint8_t MOD_KEYS[8] = {
    0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7
};

/* Own BLE address as string (random static, derived from efuse MAC).
 * Set in ble_init(); used for status display because
 * NimBLEDevice::getAddress() always reports the public address.       */
static char g_ble_addr_str[18] = "";

/* Advertising restart is deferred out of the NimBLE host-task context:
 * calling startAdvertising() directly inside onDisconnect can fail with
 * BLE_HS_EBUSY when the disconnect was caused by unpair/bond-clear.    */
static volatile uint32_t g_adv_restart_at = 0;   /* millis() deadline, 0=none */

/* --- BLE callbacks ---------------------------------------------------- */
class AtNodeServerCallbacks : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        Serial.println("BLE connected");
    }
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo,
                       int reason) override {
        Serial.printf("BLE disconnected, reason=0x%02X bonded=%d\n",
                      reason, connInfo.isBonded());
        g_adv_restart_at = millis() + 500;
    }
    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        Serial.printf("BLE auth: bonded=%d encrypted=%d\n",
                      connInfo.isBonded(), connInfo.isEncrypted());
    }
};

class LedOutputCallbacks : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
        NimBLEAttValue v = pChar->getValue();
        if (v.size() > 0) {
            Serial.printf("LED state: 0x%02X\n", v.data()[0]);
        }
    }
};

static uint8_t parse_uint8(const String& s)
{
    if (s.length() == 0) return 0;
    return (uint8_t)strtoul(s.c_str(), NULL, 0);
}

/* --- URL query helpers (for MQTT cmd channel) ---------------------------- */
static String url_decode(const String& s)
{
    String out;
    out.reserve(s.length());
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '%' && i + 2 < s.length()) {
            char hex[3] = { s[i + 1], s[i + 2], 0 };
            out += (char)strtoul(hex, NULL, 16);
            i += 2;
        } else if (c == '+') {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

/* get (decoded) value of key from "k=v&k=v" query string */
static String query_get(const String& q, const char* key)
{
    String k = String(key) + "=";
    int i = q.indexOf(k);
    if (i < 0 || (i > 0 && q[i - 1] != '&')) {
        i = q.indexOf('&' + k);
        if (i < 0) return "";
        i += 1;
    }
    int start = i + k.length();
    int amp = q.indexOf('&', start);
    return url_decode((amp > 0) ? q.substring(start, amp) : q.substring(start));
}

static void send_report(void)
{
    uint8_t report[8];
    report[0] = g_key_state.mods;
    report[1] = 0;
    for (int i = 0; i < 6; i++) report[i + 2] = g_key_state.keys[i];
    if (g_inputReport) g_inputReport->setValue(report, sizeof(report));
    if (g_inputReport) g_inputReport->notify();
}

static void clear_keys(void)
{
    g_key_state.mods = 0;
    memset(g_key_state.keys, 0, sizeof(g_key_state.keys));
    send_report();
}

static void key_press(uint8_t k)
{
    for (int i = 0; i < 6; i++) {
        if (g_key_state.keys[i] == 0) {
            g_key_state.keys[i] = k;
            break;
        }
        if (g_key_state.keys[i] == k) return;
    }
    send_report();
}

static void key_release(uint8_t k)
{
    for (int i = 0; i < 6; i++) {
        if (g_key_state.keys[i] == k) {
            g_key_state.keys[i] = 0;
        }
    }
    send_report();
}

static void key_tap(uint8_t mods, uint8_t k, int ms)
{
    if (ms <= 0) ms = 100;

    /* save current state, press mods+key, then restore.
     * NOTE: clear_keys() must not be used here - it also zeroes mods. */
    uint8_t old_mods = g_key_state.mods;
    uint8_t old_keys[6];
    memcpy(old_keys, g_key_state.keys, sizeof(old_keys));

    memset(g_key_state.keys, 0, sizeof(g_key_state.keys));
    g_key_state.keys[0] = k;
    g_key_state.mods = old_mods | mods;
    send_report();
    delay(ms);

    memcpy(g_key_state.keys, old_keys, sizeof(old_keys));
    g_key_state.mods = old_mods;
    send_report();
}

static bool is_connected(void)
{
    return g_server && g_server->getConnectedCount() > 0;
}

/* --- configuration ----------------------------------------------------- */
static void load_config(void)
{
    prefs.begin("atnode", false);
    g_device_name = prefs.getString("name", get_default_name());
    g_hostname    = prefs.getString("hostname", get_default_hostname());
    g_wifi_ssid   = prefs.getString("wifi_ssid", WIFI_SSID);
    g_wifi_pass   = prefs.getString("wifi_pass", WIFI_PASSWORD);
    g_mqtt_broker = prefs.getString("mqtt_broker", "");
    /* stored as string via save_config(); parse as int (getInt would
     * fail the NVS type check and silently return the default).      */
    g_mqtt_port   = prefs.getString("mqtt_port", "8883").toInt();
    if (g_mqtt_port <= 0) g_mqtt_port = 8883;
    g_mqtt_user   = prefs.getString("mqtt_user", "");
    g_mqtt_pass   = prefs.getString("mqtt_pass", "");
    g_mqtt_ca_cert = prefs.getString("mqtt_ca_cert", "");
    g_mqtt_ca_fp   = prefs.getString("mqtt_ca_fp", "");
    prefs.end();
}

void save_config(const String& key, const String& value)
{
    prefs.begin("atnode", false);
    prefs.putString(key.c_str(), value);
    prefs.end();
}

/* Clear all MQTT settings (NVS + runtime) and disconnect. */
static void mqtt_clear_config(void)
{
    prefs.begin("atnode", false);
    prefs.remove("mqtt_broker");
    prefs.remove("mqtt_port");
    prefs.remove("mqtt_user");
    prefs.remove("mqtt_pass");
    prefs.remove("mqtt_ca_cert");
    prefs.remove("mqtt_ca_fp");
    prefs.end();
    if (g_mqtt_connected) {
        g_mqtt.disconnect();
        g_mqtt_connected = false;
    }
    g_mqtt_broker  = "";
    g_mqtt_port    = 8883;
    g_mqtt_user    = "";
    g_mqtt_pass    = "";
    g_mqtt_ca_cert = "";
    g_mqtt_ca_fp   = "";
}

/* --- typing queue ------------------------------------------------------ */
static void type_poll(void)
{
    if (!g_type_busy) return;

    uint32_t now = millis();
    if (now < g_type_next) return;

    if (g_type_idx >= g_type_text.length()) {
        g_type_busy = false;
        g_type_text = "";
        return;
    }

    uint8_t c = (uint8_t)g_type_text[g_type_idx];
    if (c >= 'a' && c <= 'z') {
        key_tap(0, 0x04 + (c - 'a'), g_type_ms);
    } else if (c >= 'A' && c <= 'Z') {
        key_tap(0x02, 0x04 + (c - 'A'), g_type_ms);
    } else if (c == ' ') {
        key_tap(0, 0x2C, g_type_ms);
    } else if (c >= '1' && c <= '9') {
        key_tap(0, 0x1E + (c - '1'), g_type_ms);   /* '1'..'9' = 0x1E..0x26 */
    } else if (c == '0') {
        key_tap(0, 0x27, g_type_ms);               /* '0' = 0x27 */
    } else if (c == '\n') {
        key_tap(0, 0x28, g_type_ms);
    } else {
        key_tap(0, c, g_type_ms);
    }
    g_type_idx++;
    g_type_next = now + g_type_ms + g_type_gap;
}

/* --- HTTP handlers ----------------------------------------------------- */
static void send_json(const String& json, int code = 200)
{
    g_http.sendHeader("Access-Control-Allow-Origin", "*");
    g_http.send(code, "application/json", json);
}

static void send_html(const String& html, int code = 200)
{
    g_http.sendHeader("Access-Control-Allow-Origin", "*");
    g_http.send(code, "text/html", html);
}

static void handle_root(void)
{
    g_http.sendHeader("Location", "/at-node/status");
    g_http.send(302, "text/plain", "");
}

static void handle_status_html(void)
{
    String ble_state = is_connected()
        ? "<span class='ok'>connected</span>"
        : "<span class='bad'>not connected</span>";

    String html = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>AT-Node Status</title>";
    html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
    html += "<style>body{font-family:monospace;padding:16px;max-width:560px;margin:auto;}"
            "table{border-collapse:collapse;width:100%;margin:8px 0;}"
            "th,td{border:1px solid #ddd;padding:6px 8px;text-align:left;}"
            "th{background:#f5f5f5;width:38%;}"
            ".ok{color:#0a0;}.bad{color:#d33;}"
            "a{color:#007aff;}</style></head><body>";
    html += "<h1>AT-Node Status</h1>";
    html += "<table>";
    html += "<tr><th>Device</th><td>" + g_device_name + "</td></tr>";
    html += "<tr><th>Hostname</th><td>" + g_hostname + ".local</td></tr>";
    html += "<tr><th>IP</th><td>" + WiFi.localIP().toString() + "</td></tr>";
    html += "<tr><th>BLE Address</th><td>" + String(g_ble_addr_str) + "</td></tr>";
    html += "<tr><th>BLE Host</th><td>" + ble_state + "</td></tr>";
    html += "<tr><th>Bonded Hosts</th><td>" + String(NimBLEDevice::getNumBonds()) + "</td></tr>";
    html += "<tr><th>Typing</th><td>" + String(g_type_busy ? "yes" : "no") + "</td></tr>";
    html += "<tr><th>MQTT</th><td>" + String(g_mqtt_connected ? "connected" : "disconnected") + "</td></tr>";
    html += "<tr><th>AP Mode</th><td>" + String(ap_portal_active() ? "active" : "off") + "</td></tr>";
    html += "</table>";
    html += "<p><a href=\"/at-node/pair\">BLE Pairing</a> | ";
    html += "<a href=\"/at-node/help\">API Help</a> | ";
    html += "<a href=\"/at-node/cmd/status\">JSON</a></p>";
    html += "</body></html>";
    send_html(html);
}

static const char* HELP_PAGE_HTML = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AT-Node Help</title>
<style>
  body{font-family:monospace;padding:16px;max-width:800px;margin:auto;}
  h1,h2{margin-top:1em;}
  code{background:#f5f5f5;padding:2px 4px;border-radius:4px;}
  table{border-collapse:collapse;width:100%;margin:8px 0;}
  th,td{border:1px solid #ddd;padding:6px;text-align:left;}
  th{background:#f5f5f5;}
  .note{background:#fff3cd;border-left:4px solid #ffc107;padding:8px 12px;margin:8px 0;}
</style>
</head>
<body>
<h1>AT-Node HTTP API</h1>
<p>This interface is designed for agents. All endpoints return JSON unless noted.</p>

<h2>Device Discovery</h2>
<div class="note">
  <strong>mDNS</strong>: This device advertises itself via mDNS as <code>&lt;hostname&gt;.local</code>.
  Agents can discover the device IP by resolving the mDNS hostname or by scanning the local network.
  The hostname is configurable via <code>AT+CONF=hostname=...</code> (raw AT, persisted to NVS).
  Default: <code>atnodeesp-&lt;chipid&gt;.local</code> (e.g., <code>atnodeesp-c842.local</code>).
</div>

<h2>Status</h2>
<table>
  <tr><th>Method</th><th>Path</th><th>Format</th><th>Description</th></tr>
  <tr><td>GET</td><td><code>/at-node/status</code></td><td>HTML</td><td>Device status page</td></tr>
  <tr><td>GET</td><td><code>/at-node/cmd/status</code></td><td>JSON</td><td>Device status (pure JSON)</td></tr>
  <tr><td>GET</td><td><code>/at-node/help</code></td><td>HTML</td><td>This help page</td></tr>
  <tr><td>GET</td><td><code>/at-node/pair</code></td><td>HTML</td><td>BLE pairing page (browser)</td></tr>
</table>

<h2>Raw AT Command</h2>
<table>
  <tr><th>Method</th><th>Path</th><th>Body</th><th>Description</th></tr>
  <tr><td>POST</td><td><code>/at-node/at</code></td><td><code>AT+...</code></td><td>Execute raw AT command (text/plain)</td></tr>
</table>
<p>Config keys via <code>AT+CONF=name=...</code> / <code>AT+CONF=hostname=...</code> (persisted to NVS).<br>
MQTT subcommands: <code>AT+MQTT=broker|port|ca,&lt;val&gt;</code> and <code>AT+MQTT=connect|status|clear</code> (no value) —
<code>clear</code> wipes all MQTT settings (NVS + runtime) and disconnects.</p>

<h2>BLE Keyboard / Pairing</h2>
<table>
  <tr><th>Method</th><th>Path</th><th>Params</th><th>Description</th></tr>
  <tr><td>GET</td><td><code>/at-node/cmd/ble/status</code></td><td></td><td>BLE name, address, connected peers, bonded host list</td></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/ble/advertise</code></td><td><code>start=1|0</code></td><td>Start / stop BLE advertising (make device discoverable for pairing)</td></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/ble/bonds/delete</code></td><td><code>idx</code></td><td>Remove one bonded host (idx from ble/status)</td></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/ble/bonds/clear</code></td><td></td><td>Remove ALL bonded hosts</td></tr>
</table>
<p>Browser UI: <a href="/at-node/pair">/at-node/pair</a> &mdash; shows connection state,
controls advertising, lists and removes bonded hosts.
Pairing flow: make sure the device is advertising, then select
<code>AT-Node-ESP-XXXX</code> in the host OS Bluetooth settings (Just Works, no PIN).
After a firmware update that changes GATT services, remove the device in the
host OS first (hosts cache the GATT table per MAC).</p>

<h2>Keyboard</h2>
<table>
  <tr><th>Method</th><th>Path</th><th>Params</th></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/keyboard/tap</code></td><td><code>mods,k,ms</code></td></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/keyboard/text</code></td><td><code>s,ms,gap</code></td></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/keyboard/key</code></td><td><code>mods,k0..k5</code></td></tr>
</table>

<h2>Peripherals</h2>
<table>
  <tr><th>Method</th><th>Path</th><th>Params</th></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/gpio/write</code></td><td><code>pin,level</code></td></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/gpio/read</code></td><td><code>pin</code></td></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/adc/read</code></td><td><code>ch</code></td></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/i2c/scan</code></td><td><code></code></td></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/i2c/read</code></td><td><code>addr,reg,len</code></td></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/i2c/write</code></td><td><code>addr,reg,data</code></td></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/ir/send</code></td><td><code>protocol,data,bits</code></td></tr>
</table>

<h2>Network</h2>
<table>
  <tr><th>Method</th><th>Path</th><th>Params</th><th>Description</th></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/net/wol</code></td><td><code>mac</code></td><td>Send Wake-on-LAN magic packet on the device LAN</td></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/net/ping</code></td><td><code>host,count</code></td><td>ICMP ping from the device LAN, returns avg RTT</td></tr>
</table>

<h2>Configuration</h2>
<table>
  <tr><th>Method</th><th>Path</th><th>Params</th></tr>
  <tr><td>GET</td><td><code>/at-node/cmd/mqtt/status</code></td><td><code></code></td></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/wifi/config</code></td><td><code>ssid,pass</code></td></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/mqtt/config</code></td><td><code>broker,port,user,pass</code></td></tr>
  <tr><td colspan="3"><small>Clear all MQTT settings via raw AT: <code>AT+MQTT=clear</code></small></td></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/mqtt/ca</code></td><td><code>plain (PEM) or fp</code></td></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/mqtt/connect</code></td><td><code></code></td></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/mqtt/publish</code></td><td><code>topic,msg</code></td></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/mqtt/subscribe</code></td><td><code>topic</code></td></tr>
  <tr><td>POST</td><td><code>/at-node/cmd/ap</code></td><td><code>1=start,0=stop</code></td></tr>
</table>

<h2>Examples</h2>
<pre>
# Tap key 'a' (0x04)
curl -X POST "http://atnodeesp-c842.local/at-node/cmd/keyboard/tap?mods=0&k=4&ms=100"

# Type text
curl -X POST "http://atnodeesp-c842.local/at-node/cmd/keyboard/text?s=Hello&ms=60&gap=100"

# Raw AT command
curl -X POST -d "AT+TAP=100,0,4" http://atnodeesp-c842.local/at-node/at

# Get JSON status
curl http://atnodeesp-c842.local/at-node/cmd/status

# BLE: check status, start advertising, clear bonds
curl http://atnodeesp-c842.local/at-node/cmd/ble/status
curl -X POST "http://atnodeesp-c842.local/at-node/cmd/ble/advertise?start=1"
curl -X POST "http://atnodeesp-c842.local/at-node/cmd/ble/bonds/clear"
</pre>

<p><a href="/at-node/status">Back to Status</a></p>
</body>
</html>
)HTML";

static void handle_help_html(void)
{
    send_html(HELP_PAGE_HTML);
}

/* --- BLE status / pairing ---------------------------------------------- */
static String build_ble_status_json(void)
{
    String json = "{";
    json += "\"name\":\"" + g_device_name + "\"";
    json += ",\"addr\":\"" + String(g_ble_addr_str) + "\"";
    json += ",\"connected\":" + String(is_connected() ? "true" : "false");
    json += ",\"advertising\":" + String(NimBLEDevice::getAdvertising()->isAdvertising() ? "true" : "false");
    json += ",\"peers\":[";
    uint8_t n = g_server ? g_server->getConnectedCount() : 0;
    for (uint8_t i = 0; i < n; i++) {
        NimBLEConnInfo info = g_server->getPeerInfo(i);
        if (i) json += ",";
        json += "{\"addr\":\"" + String(info.getAddress().toString().c_str()) + "\"";
        json += ",\"bonded\":" + String(info.isBonded() ? "true" : "false");
        json += ",\"encrypted\":" + String(info.isEncrypted() ? "true" : "false");
        json += "}";
    }
    json += "],\"bonds\":[";
    int nb = NimBLEDevice::getNumBonds();
    for (int i = 0; i < nb; i++) {
        NimBLEAddress a = NimBLEDevice::getBondedAddress(i);
        if (i) json += ",";
        json += "{\"idx\":" + String(i) + ",\"addr\":\"" + String(a.toString().c_str()) + "\"}";
    }
    json += "]}";
    return json;
}

static void handle_ble_status(void)
{
    send_json(build_ble_status_json());
}

/* System info manifest - also published to MQTT atnode/<id>/info.
 * The "services" list doubles as the remote API catalog.            */
static String build_sys_info_json(void)
{
    String json = "{";
    json += "\"device\":\"" + g_device_name + "\"";
    json += ",\"hostname\":\"" + g_hostname + "\"";
    json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
    json += ",\"ble_addr\":\"" + String(g_ble_addr_str) + "\"";
    json += ",\"ble_connected\":" + String(is_connected() ? "true" : "false");
    json += ",\"typing\":" + String(g_type_busy ? "true" : "false");
    json += ",\"mqtt\":" + String(g_mqtt_connected ? "true" : "false");
    json += ",\"services\":[\"keyboard/tap\",\"keyboard/text\",\"keyboard/key\",";
    json += "\"gpio/write\",\"gpio/read\",\"adc/read\",";
    json += "\"i2c/scan\",\"i2c/read\",\"i2c/write\",\"ir/send\",";
    json += "\"ble/status\",\"ble/advertise\",\"ble/bonds/delete\",\"ble/bonds/clear\",";
    json += "\"net/wol\",\"net/ping\",\"sys/info\"]";
    json += "}";
    return json;
}

static void handle_ble_advertise(void)
{
    String start = g_http.arg("start");
    if (start == "1" || start == "true") {
        NimBLEDevice::getAdvertising()->start();
    } else if (start == "0" || start == "false") {
        NimBLEDevice::getAdvertising()->stop();
    }
    send_json("{\"ok\":true,\"cmd\":\"ble/advertise\",\"advertising\":" +
              String(NimBLEDevice::getAdvertising()->isAdvertising() ? "true" : "false") + "}");
}

static void handle_ble_bonds_delete(void)
{
    String idxStr = g_http.arg("idx");
    if (idxStr.length() == 0) {
        send_json("{\"ok\":false,\"error\":\"missing idx\"}", 400);
        return;
    }
    int idx = idxStr.toInt();
    int nb  = NimBLEDevice::getNumBonds();
    if (idx < 0 || idx >= nb) {
        send_json("{\"ok\":false,\"error\":\"invalid idx\"}", 400);
        return;
    }
    NimBLEAddress addr = NimBLEDevice::getBondedAddress(idx);
    bool ok = NimBLEDevice::deleteBond(addr);
    Serial.printf("BLE bond delete idx=%d addr=%s ok=%d\n",
                  idx, addr.toString().c_str(), ok);
    if (!ok) {
        send_json("{\"ok\":false,\"error\":\"unpair failed\"}", 500);
        return;
    }
    send_json("{\"ok\":true,\"cmd\":\"ble/bonds/delete\",\"addr\":\"" +
              String(addr.toString().c_str()) + "\"}");
}

static void handle_ble_bonds_clear(void)
{
    bool ok = NimBLEDevice::deleteAllBonds();
    Serial.printf("BLE bonds cleared ok=%d\n", ok);
    send_json("{\"ok\":" + String(ok ? "true" : "false") + ",\"cmd\":\"ble/bonds/clear\"}");
}

static const char* PAIR_PAGE_HTML = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AT-Node BLE Pairing</title>
<style>
  body{font-family:monospace;padding:16px;max-width:640px;margin:auto;}
  h1{font-size:22px;} h2{font-size:16px;margin-top:1.4em;}
  table{border-collapse:collapse;width:100%;margin:8px 0;}
  th,td{border:1px solid #ddd;padding:6px 8px;text-align:left;font-size:14px;}
  th{background:#f5f5f5;width:38%;}
  button{padding:8px 14px;margin:4px 4px 4px 0;border:1px solid #007aff;
         border-radius:6px;background:#007aff;color:#fff;cursor:pointer;font-size:14px;}
  button.ghost{background:#fff;color:#007aff;}
  button.danger{border-color:#d33;background:#d33;}
  button:active{opacity:.8;}
  .ok{color:#0a0;} .bad{color:#d33;}
  a{color:#007aff;}
  #msg{margin-top:10px;color:#555;min-height:1.2em;}
</style>
</head>
<body>
<h1>AT-Node BLE Pairing</h1>

<h2>Device</h2>
<table>
  <tr><th>BLE Name</th><td id="name">-</td></tr>
  <tr><th>BLE Address</th><td id="addr">-</td></tr>
  <tr><th>Advertising</th><td id="adv">-</td></tr>
</table>
<button onclick="setAdv(1)">Start Advertising</button>
<button class="ghost" onclick="setAdv(0)">Stop Advertising</button>

<h2>Connected Host</h2>
<div id="peers">none</div>

<h2>Bonded Hosts</h2>
<div id="bonds">none</div>
<button class="danger" onclick="clearBonds()">Clear All Bonds</button>
<p><small>Removing a bond here does not remove it on the host &mdash; also remove
&quot;AT-Node-ESP&quot; in your OS Bluetooth settings, otherwise the host will
re-pair automatically on reconnect.</small></p>

<p id="msg"></p>
<p><a href="/at-node/status">Back to Status</a> | <a href="/at-node/help">API Help</a></p>

<script>
function msg(t){ document.getElementById('msg').textContent = t; }
function refresh(){
  fetch('/at-node/cmd/ble/status').then(r=>r.json()).then(s=>{
    document.getElementById('name').textContent = s.name;
    document.getElementById('addr').textContent = s.addr;
    document.getElementById('adv').innerHTML = s.advertising
      ? '<span class="ok">yes</span>' : '<span class="bad">no</span>';
    document.getElementById('peers').innerHTML = s.peers.length
      ? s.peers.map(p => p.addr + (p.bonded ? ' (bonded)' : ' (not bonded)') +
                       (p.encrypted ? ' [encrypted]' : '')).join('<br>')
      : 'none &mdash; pair from your host now';
    document.getElementById('bonds').innerHTML = s.bonds.length
      ? '<table><tr><th>Address</th><th></th></tr>' + s.bonds.map(b =>
          '<tr><td>'+b.addr+'</td><td><button class="danger" onclick="delBond('+b.idx+')">Remove</button></td></tr>'
        ).join('') + '</table>'
      : 'none';
  }).catch(()=>{ msg('refresh failed'); });
}
function setAdv(on){
  fetch('/at-node/cmd/ble/advertise?start='+(on?1:0), {method:'POST'})
    .then(r=>r.json()).then(()=>{ msg('advertising '+(on?'started':'stopped')); refresh(); });
}
function delBond(i){
  if(!confirm('Remove this bond?')) return;
  fetch('/at-node/cmd/ble/bonds/delete?idx='+i, {method:'POST'})
    .then(r=>r.json()).then(d=>{
      msg(d.ok ? 'bond removed &mdash; also remove the device in the host OS Bluetooth settings'
               : 'remove failed: '+(d.error||'unknown'));
      refresh();
    });
}
function clearBonds(){
  if(!confirm('Remove ALL bonded hosts?')) return;
  fetch('/at-node/cmd/ble/bonds/clear', {method:'POST'})
    .then(r=>r.json()).then(d=>{
      msg(d.ok ? 'all bonds cleared'
               : 'clear failed: '+(d.error||'unknown'));
      refresh();
    });
}
refresh();
setInterval(refresh, 2000);
</script>
</body>
</html>
)HTML";

static void handle_pair_html(void)
{
    send_html(PAIR_PAGE_HTML);
}

static void handle_cmd_status(void)
{
    String json = "{";
    json += "\"device\":\"" + g_device_name + "\"";
    json += ",\"hostname\":\"" + g_hostname + "\"";
    json += ",\"connected\":" + String(is_connected() ? "true" : "false");
    json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
    json += ",\"ble_addr\":\"" + String(g_ble_addr_str) + "\"";
    json += ",\"typing\":" + String(g_type_busy ? "true" : "false");
    json += ",\"mqtt\":" + String(g_mqtt_connected ? "true" : "false");
    json += ",\"ap\":" + String(ap_portal_active() ? "true" : "false");
    json += "}";
    send_json(json);
}

/* forward declarations for IR functions defined later */
static bool ir_send_raw(const uint16_t* timings, size_t count);
static bool ir_send_nec(uint32_t data);
static bool ir_send_sirc(uint32_t data, int bits);

static void handle_at(void)
{
    String cmd = g_http.arg("plain");
    cmd.trim();
    if (cmd.length() == 0) {
        send_json("{\"ok\":false,\"error\":\"empty command\"}", 400);
        return;
    }

    /* simple raw AT command parser */
    String resp;
    if (cmd == "AT") {
        resp = "OK";
    } else if (cmd.startsWith("AT+TAP=")) {
        String args = cmd.substring(7);
        int c1 = args.indexOf(',');
        int c2 = args.indexOf(',', c1 + 1);
        if (c1 > 0 && c2 > c1) {
            int ms   = args.substring(0, c1).toInt();
            int mods = args.substring(c1 + 1, c2).toInt();
            int key  = args.substring(c2 + 1).toInt();
            key_tap((uint8_t)mods, (uint8_t)key, ms);
            resp = "OK";
        } else {
            resp = "ERROR";
        }
    } else if (cmd.startsWith("AT+KEY=")) {
        String args = cmd.substring(7);
        g_key_state.mods = parse_uint8(args);
        for (int i = 0; i < 6; i++) {
            int comma = args.indexOf(',');
            String part = (comma > 0) ? args.substring(0, comma) : args;
            g_key_state.keys[i] = parse_uint8(part);
            if (comma > 0) args = args.substring(comma + 1);
        }
        send_report();
        resp = "OK";
    } else if (cmd.startsWith("AT+TEXT=")) {
        String text = cmd.substring(8);
        g_type_text = text;
        g_type_idx  = 0;
        g_type_busy = true;
        resp = "OK";
    } else if (cmd.startsWith("AT+CONF=")) {
        String kv = cmd.substring(8);
        int eq = kv.indexOf('=');
        if (eq > 0) {
            String key = kv.substring(0, eq);
            String val = kv.substring(eq + 1);
            save_config(key, val);
            if (key == "name") g_device_name = val;
            if (key == "hostname") g_hostname = val;
            resp = "OK";
        } else {
            resp = "ERROR";
        }
    } else if (cmd.startsWith("AT+GPIO_W=")) {
        String args = cmd.substring(10);
        int c1 = args.indexOf(',');
        if (c1 > 0) {
            int pin = args.substring(0, c1).toInt();
            int level = args.substring(c1 + 1).toInt();
            pinMode(pin, OUTPUT);
            digitalWrite(pin, level ? HIGH : LOW);
            resp = "OK";
        } else {
            resp = "ERROR";
        }
    } else if (cmd.startsWith("AT+GPIO_R=")) {
        int pin = cmd.substring(10).toInt();
        pinMode(pin, INPUT_PULLUP);
        int level = digitalRead(pin);
        resp = "+GPIO_R:" + String(level);
    } else if (cmd.startsWith("AT+ADC=")) {
        int ch = cmd.substring(7).toInt();
        int mv = analogReadMilliVolts(ch);
        resp = "+ADC:" + String(mv);
    } else if (cmd == "AT+I2C_SCAN") {
        for (uint8_t addr = 1; addr < 127; addr++) {
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) {
                resp += "+I2C_SCAN:0x" + String(addr, HEX) + " ";
            }
        }
        if (resp.length() == 0) resp = "+I2C_SCAN:none";
    } else if (cmd.startsWith("AT+I2C_R=")) {
        String args = cmd.substring(9);
        int c1 = args.indexOf(',');
        int c2 = args.indexOf(',', c1 + 1);
        if (c1 > 0 && c2 > c1) {
            int addr = strtoul(args.substring(0, c1).c_str(), NULL, 0);
            int reg  = strtoul(args.substring(c1 + 1, c2).c_str(), NULL, 0);
            int len  = args.substring(c2 + 1).toInt();
            Wire.beginTransmission(addr);
            Wire.write(reg);
            Wire.endTransmission(false);
            Wire.requestFrom(addr, len);
            resp = "+I2C_R:";
            while (Wire.available()) {
                uint8_t b = Wire.read();
                if (b < 0x10) resp += "0";
                resp += String(b, HEX);
                resp += " ";
            }
            resp.trim();
        } else {
            resp = "ERROR";
        }
    } else if (cmd.startsWith("AT+I2C_W=")) {
        String args = cmd.substring(9);
        int c1 = args.indexOf(',');
        int c2 = args.indexOf(',', c1 + 1);
        if (c1 > 0 && c2 > c1) {
            int addr = strtoul(args.substring(0, c1).c_str(), NULL, 0);
            int reg  = strtoul(args.substring(c1 + 1, c2).c_str(), NULL, 0);
            String hexData = args.substring(c2 + 1);
            hexData.replace(" ", "");
            Wire.beginTransmission(addr);
            Wire.write(reg);
            for (int i = 0; i < hexData.length(); i += 2) {
                String byteStr = hexData.substring(i, i + 2);
                uint8_t b = (uint8_t)strtoul(byteStr.c_str(), NULL, 16);
                Wire.write(b);
            }
            if (Wire.endTransmission() == 0) {
                resp = "OK";
            } else {
                resp = "ERROR";
            }
        } else {
            resp = "ERROR";
        }
    } else if (cmd.startsWith("AT+IR=")) {
        String args = cmd.substring(6);
        int c1 = args.indexOf(',');
        if (c1 > 0) {
            String proto = args.substring(0, c1);
            String data = args.substring(c1 + 1);
            bool ok = false;
            if (proto.equalsIgnoreCase("NEC")) {
                uint32_t d = strtoul(data.c_str(), NULL, 0);
                ok = ir_send_nec(d);
            } else if (proto.equalsIgnoreCase("SIRC")) {
                int c2 = data.indexOf(',');
                uint32_t d = strtoul(data.substring(0, c2).c_str(), NULL, 0);
                int bits = data.substring(c2 + 1).toInt();
                ok = ir_send_sirc(d, bits);
            } else if (proto.equalsIgnoreCase("RAW")) {
                uint16_t timings[256];
                int count = 0;
                int start = 0;
                while (count < 256) {
                    int comma = data.indexOf(',', start);
                    String part = (comma > 0) ? data.substring(start, comma) : data.substring(start);
                    timings[count++] = (uint16_t)part.toInt();
                    if (comma < 0) break;
                    start = comma + 1;
                }
                ok = ir_send_raw(timings, count);
            } else {
                resp = "ERROR";
            }
            if (ok) resp = "OK";
            else resp = "ERROR";
        } else {
            resp = "ERROR";
        }
    } else if (cmd.startsWith("AT+MQTT=")) {
        String args = cmd.substring(8);
        int c1 = args.indexOf(',');
        if (args.length() > 0) {   /* comma optional: sub-only commands (clear/status/connect) */
            String sub = (c1 > 0) ? args.substring(0, c1) : args;
            String val = (c1 > 0) ? args.substring(c1 + 1) : "";
            if (sub == "broker") {
                g_mqtt_broker = val;
                save_config("mqtt_broker", val);
                resp = "OK";
            } else if (sub == "port") {
                g_mqtt_port = val.toInt();
                save_config("mqtt_port", val);
                resp = "OK";
            } else if (sub == "connect") {
                bool ok = mqtt_connect();
                resp = ok ? "OK" : "ERROR";
            } else if (sub == "clear") {
                mqtt_clear_config();
                resp = "OK";
            } else if (sub == "status") {
                resp = "+MQTT:" + String(g_mqtt_connected ? "connected" : "disconnected");
            } else if (sub == "ca") {
                /* val should be the CA cert PEM or SHA256 fingerprint, or "status" */
                if (val == "status") {
                    if (g_mqtt_ca_fp.length() > 0) {
                        resp = "+MQTT_CA:fingerprint";
                    } else if (g_mqtt_ca_cert.length() > 0) {
                        resp = "+MQTT_CA:pem";
                    } else {
                        resp = "+MQTT_CA:none";
                    }
                } else if (val.startsWith("-----BEGIN")) {
                    g_mqtt_ca_cert = val;
                    save_config("mqtt_ca_cert", val);
                    resp = "OK";
                } else {
                    g_mqtt_ca_fp = val;
                    save_config("mqtt_ca_fp", val);
                    resp = "OK";
                }
            } else {
                resp = "ERROR";
            }
        } else {
            resp = "ERROR";
        }
    } else if (cmd.startsWith("AT+WIFI=")) {
        String args = cmd.substring(8);
        int c1 = args.indexOf(',');
        if (c1 > 0) {
            String sub = args.substring(0, c1);
            String val = args.substring(c1 + 1);
            if (sub == "ssid") {
                g_wifi_ssid = val;
                save_config("wifi_ssid", val);
                resp = "OK";
            } else if (sub == "pass") {
                g_wifi_pass = val;
                save_config("wifi_pass", val);
                resp = "OK";
            } else if (sub == "status") {
                resp = "+WIFI:" + g_wifi_ssid;
            } else {
                resp = "ERROR";
            }
        } else {
            resp = "ERROR";
        }
    } else if (cmd.startsWith("AT+AP=")) {
        int val = cmd.substring(6).toInt();
        if (val == 1) {
            ap_portal_start();
            resp = "OK";
        } else if (val == 0) {
            ap_portal_stop();
            resp = "OK";
        } else {
            resp = "ERROR";
        }
    } else {
        resp = "ERROR: unknown command";
    }

    String json = "{\"ok\":";
    json += (resp.indexOf("OK") == 0 || resp.indexOf("+") == 0) ? "true" : "false";
    json += ",\"response\":\"" + resp + "\"}";
    send_json(json);
}

static void handle_keyboard_tap(void)
{
    if (!is_connected()) {
        send_json("{\"ok\":false,\"error\":\"BLE not connected\"}", 409);
        return;
    }
    String body = g_http.arg("plain");
    uint8_t mods = parse_uint8(g_http.arg("mods"));
    uint8_t key  = parse_uint8(g_http.arg("k"));
    int ms       = g_http.arg("ms").toInt();
    if (ms <= 0) ms = 100;

    key_tap(mods, key, ms);
    send_json("{\"ok\":true,\"cmd\":\"keyboard/tap\",\"ms\":" + String(ms) + "}");
}

static void handle_keyboard_text(void)
{
    if (!is_connected()) {
        send_json("{\"ok\":false,\"error\":\"BLE not connected\"}", 409);
        return;
    }
    if (g_type_busy) {
        send_json("{\"ok\":false,\"error\":\"typing in progress\"}", 423);
        return;
    }
    String text = g_http.arg("s");
    if (text.length() == 0) {
        send_json("{\"ok\":false,\"error\":\"missing s\"}", 400);
        return;
    }
    int ms  = g_http.arg("ms").toInt();
    int gap = g_http.arg("gap").toInt();
    g_type_ms  = (ms > 0) ? ms : 40;
    g_type_gap = (gap > 0) ? gap : 30;
    g_type_text = text;
    g_type_idx  = 0;
    g_type_next = 0;
    g_type_busy = true;
    send_json("{\"ok\":true,\"cmd\":\"keyboard/text\",\"queued\":true}");
}

static void handle_keyboard_key(void)
{
    if (!is_connected()) {
        send_json("{\"ok\":false,\"error\":\"BLE not connected\"}", 409);
        return;
    }
    uint8_t mods = parse_uint8(g_http.arg("mods"));
    g_key_state.mods = mods;
    for (int i = 0; i < 6; i++) {
        String arg = g_http.arg("k" + String(i));
        g_key_state.keys[i] = parse_uint8(arg);
    }
    send_report();
    send_json("{\"ok\":true,\"cmd\":\"keyboard/key\"}");
}

/* --- GPIO / ADC --------------------------------------------------------- */
static void handle_gpio_write(void)
{
    int pin   = g_http.arg("pin").toInt();
    int level = g_http.arg("level").toInt();
    if (pin < 0 || pin > 48) {
        send_json("{\"ok\":false,\"error\":\"invalid pin\"}", 400);
        return;
    }
    pinMode(pin, OUTPUT);
    digitalWrite(pin, level ? HIGH : LOW);
    send_json("{\"ok\":true,\"cmd\":\"gpio/write\",\"pin\":" + String(pin) +
              ",\"level\":" + String(level) + "}");
}

static void handle_gpio_read(void)
{
    int pin = g_http.arg("pin").toInt();
    if (pin < 0 || pin > 48) {
        send_json("{\"ok\":false,\"error\":\"invalid pin\"}", 400);
        return;
    }
    pinMode(pin, INPUT_PULLUP);
    int level = digitalRead(pin);
    send_json("{\"ok\":true,\"cmd\":\"gpio/read\",\"pin\":" + String(pin) +
              ",\"level\":" + String(level) + "}");
}

static void handle_adc_read(void)
{
    int ch = g_http.arg("ch").toInt();
    if (ch < 0 || ch > 7) {
        send_json("{\"ok\":false,\"error\":\"invalid adc ch\"}", 400);
        return;
    }
    /* ESP32-C3 ADC1: GPIO0-4 = ch0-4, GPIO5-7 = ch5-7 */
    int pin = ch;
    if (ch >= 5 && ch <= 7) pin = ch;
    int mv = analogReadMilliVolts(pin);
    send_json("{\"ok\":true,\"cmd\":\"adc/read\",\"ch\":" + String(ch) +
              ",\"mv\":" + String(mv) + "}");
}

/* --- I2C --------------------------------------------------------------- */
static void handle_i2c_scan(void)
{
    String found = "[";
    bool first = true;
    for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            if (!first) found += ",";
            found += "\"0x" + String(addr, HEX) + "\"";
            first = false;
        }
    }
    found += "]";
    send_json("{\"ok\":true,\"cmd\":\"i2c/scan\",\"devices\":" + found + "}");
}

static void handle_i2c_read(void)
{
    int addr = strtoul(g_http.arg("addr").c_str(), NULL, 0);
    int reg  = strtoul(g_http.arg("reg").c_str(), NULL, 0);
    int len  = g_http.arg("len").toInt();
    if (len <= 0 || len > 32) {
        send_json("{\"ok\":false,\"error\":\"len must be 1-32\"}", 400);
        return;
    }
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) {
        send_json("{\"ok\":false,\"error\":\"i2c no ack\"}", 500);
        return;
    }
    Wire.requestFrom(addr, len);
    String data = "";
    while (Wire.available()) {
        uint8_t b = Wire.read();
        char hex[3];
        sprintf(hex, "%02X", b);
        data += hex;
        data += " ";
    }
    data.trim();
    send_json("{\"ok\":true,\"cmd\":\"i2c/read\",\"addr\":\"0x" + String(addr, HEX) +
              "\",\"reg\":\"0x" + String(reg, HEX) + "\",\"data\":\"" + data + "\"}");
}

static void handle_i2c_write(void)
{
    int addr = strtoul(g_http.arg("addr").c_str(), NULL, 0);
    int reg  = strtoul(g_http.arg("reg").c_str(), NULL, 0);
    String hexData = g_http.arg("data");
    hexData.replace(" ", "");
    if (hexData.length() == 0 || (hexData.length() % 2) != 0) {
        send_json("{\"ok\":false,\"error\":\"data must be hex pairs\"}", 400);
        return;
    }
    Wire.beginTransmission(addr);
    Wire.write(reg);
    for (int i = 0; i < hexData.length(); i += 2) {
        String byteStr = hexData.substring(i, i + 2);
        uint8_t b = (uint8_t)strtoul(byteStr.c_str(), NULL, 16);
        Wire.write(b);
    }
    if (Wire.endTransmission() != 0) {
        send_json("{\"ok\":false,\"error\":\"i2c no ack\"}", 500);
        return;
    }
    send_json("{\"ok\":true,\"cmd\":\"i2c/write\",\"addr\":\"0x" + String(addr, HEX) +
              "\",\"reg\":\"0x" + String(reg, HEX) + "\"}");
}

/* --- IR sender (RMT) --------------------------------------------------- */
#define IR_TX_PIN      4
#define IR_CARRIER_HZ  38000
#define RMT_FREQ_HZ    1000000     /* 1MHz, 1us per tick */

static bool ir_init(void)
{
    if (!rmtInit(IR_TX_PIN, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, RMT_FREQ_HZ)) {
        return false;
    }
    if (!rmtSetCarrier(IR_TX_PIN, true, true, IR_CARRIER_HZ, 0.33f)) {
        return false;
    }
    return true;
}

static bool ir_send_raw(const uint16_t* timings, size_t count)
{
    if (count == 0 || count > 256) return false;
    rmt_data_t items[256];
    for (size_t i = 0; i < count; i++) {
        uint16_t us = timings[i];
        items[i].level0    = (i % 2 == 0) ? 1 : 0;
        items[i].duration0 = us;
        items[i].level1    = 0;
        items[i].duration1 = 0;
    }
    return rmtWrite(IR_TX_PIN, items, count, RMT_WAIT_FOR_EVER);
}

static bool ir_send_nec(uint32_t data)
{
    /* NEC: 9000us mark, 4500us space, then 32 bits (560us mark + 560/1690us space) */
    uint16_t timings[68];
    int idx = 0;
    timings[idx++] = 9000;
    timings[idx++] = 4500;
    for (int i = 0; i < 32; i++) {
        timings[idx++] = 560;
        if (data & (1UL << i)) {
            timings[idx++] = 1690;
        } else {
            timings[idx++] = 560;
        }
    }
    timings[idx++] = 560;
    return ir_send_raw(timings, idx);
}

static bool ir_send_sirc(uint32_t data, int bits)
{
    /* SIRC: 2400us mark, 600us space, then bits (1200/600us mark + 600us space) */
    uint16_t timings[2 + 2 * 20];
    int idx = 0;
    timings[idx++] = 2400;
    timings[idx++] = 600;
    for (int i = 0; i < bits; i++) {
        if (data & (1UL << i)) {
            timings[idx++] = 1200;
        } else {
            timings[idx++] = 600;
        }
        timings[idx++] = 600;
    }
    return ir_send_raw(timings, idx);
}

static void handle_ir_send(void)
{
    String proto = g_http.arg("protocol");
    String data  = g_http.arg("data");
    String bitsStr = g_http.arg("bits");
    if (proto.length() == 0 || data.length() == 0) {
        send_json("{\"ok\":false,\"error\":\"missing protocol/data\"}", 400);
        return;
    }
    bool ok = false;
    uint32_t d = strtoul(data.c_str(), NULL, 0);
    int bits = bitsStr.toInt();
    if (bits <= 0) bits = 32;

    if (proto.equalsIgnoreCase("NEC")) {
        ok = ir_send_nec(d);
    } else if (proto.equalsIgnoreCase("SIRC")) {
        ok = ir_send_sirc(d, bits);
    } else if (proto.equalsIgnoreCase("RAW")) {
        /* data should be comma-separated us timings */
        uint16_t timings[256];
        int count = 0;
        int start = 0;
        while (count < 256) {
            int comma = data.indexOf(',', start);
            String part = (comma > 0) ? data.substring(start, comma) : data.substring(start);
            timings[count++] = (uint16_t)part.toInt();
            if (comma < 0) break;
            start = comma + 1;
        }
        ok = ir_send_raw(timings, count);
    } else {
        send_json("{\"ok\":false,\"error\":\"unknown protocol\"}", 400);
        return;
    }
    if (ok) {
        send_json("{\"ok\":true,\"cmd\":\"ir/send\",\"protocol\":\"" + proto + "\"}");
    } else {
        send_json("{\"ok\":false,\"error\":\"ir send failed\"}", 500);
    }
}

/* --- MQTT client ------------------------------------------------------- */
static const char* MQTT_CA_CERT = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDFTCCAf2gAwIBAgIUeR4LwVptVWTNmEFJT+rG6+1MGm4wDQYJKoZIhvcNAQEL
BQAwGjEYMBYGA1UEAwwPYXRub2RlLWxvY2FsLWNhMB4XDTI2MDcyNDAyMzYxOFoX
DTM2MDcyMTAyMzYxOFowGjEYMBYGA1UEAwwPYXRub2RlLWxvY2FsLWNhMIIBIjAN
BgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA7FZZ0kDppbeuzBIHdmeqFOreWxX7
/cRlk6xdh80E1LAwEYs2iby/JxQNauqLZ1BsFOf86wzTkuEmSy/qcsR9yyNwEz2+
RFQRwEF3FGI/y02TCEgl1RCkKcM9eaGJp2DdtI5+Rib8IISiszM9JP9WfVxvMkrG
qtccp67H2GpKVtNFt08QbEeCObEma26VFsnFMRDEU6zewb3GOXpKFjTXkf0UbkYM
cBaRn3rCYtP3dF3YnNXNnRJsDFNcO2DSQtWT0wlz2uQYrcJtKiDN+gzY+6ulCRlk
G4iwAFnOQVSRw7lMHfJPb85Nxo5/U+zsbrY6bE60ERIYDqgAnwcptyHVdQIDAQAB
o1MwUTAdBgNVHQ4EFgQUu7bfD4HnVOGCIqoAzl5/L1Tf8mowHwYDVR0jBBgwFoAU
u7bfD4HnVOGCIqoAzl5/L1Tf8mowDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0B
AQsFAAOCAQEADrl5eLSenj2Zkh4PbimN2eNAmQDZrE3t6jdqjF5Q8VIhFwJVPHad
zSHuvWa6ZN9W2rgkoe+/XP1SwxUfPJJQBaQoESvYiajZ9A2nqRDqGR4qk5J8G79c
IwlkYviYJLnIqDq+apb3LC/6bdvUAuwILerIc7CqancynFrZva1S9Ggn0RQ00Rhv
+SKierZMW+Xk9ED5J60yzl9qcydKAG+XVTUGO8oC7aNVuArMfbTQ5WlxEMaUueys
cZzQ5YIW0qBqzAp812DkzAvqzIOzI4C2zOpq1LxzxlxVmoIY8gEIN6a9XBNnLTiC
dcHklz6t6u/6dLL/gDCbE4sAFO2opEBnPw==
-----END CERTIFICATE-----
)EOF";

/* forward declaration - defined after the network helpers below */
static String mqtt_exec(const String& method, const String& query);

/* --- Wake-on-LAN --------------------------------------------------------- */
static bool wol_send(const String& macStr)
{
    unsigned int mac[6];
    if (sscanf(macStr.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
               &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
        return false;
    }
    uint8_t pkt[102];
    memset(pkt, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 6; j++) pkt[6 + i * 6 + j] = (uint8_t)mac[j];
    }
    WiFiUDP udp;
    udp.begin(0);
    bool ok = false;
    if (udp.beginPacket(IPAddress(255, 255, 255, 255), 9) == 1) {
        udp.write(pkt, sizeof(pkt));
        ok = udp.endPacket() == 1;
    }
    if (udp.beginPacket(WiFi.broadcastIP(), 9) == 1) {
        udp.write(pkt, sizeof(pkt));
        ok = (udp.endPacket() == 1) || ok;
    }
    udp.stop();
    return ok;
}

/* --- ICMP ping (esp-idf ping component) ----------------------------------- */
struct PingCtx {
    volatile int      recv;
    volatile uint32_t total_ms;
    volatile bool     done;
};

static void ping_on_success(esp_ping_handle_t hdl, void* args)
{
    PingCtx* ctx = (PingCtx*)args;
    uint32_t elapsed = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    ctx->recv++;
    ctx->total_ms += elapsed;
}

static void ping_on_end(esp_ping_handle_t hdl, void* args)
{
    ((PingCtx*)args)->done = true;
}

/* Blocking ping, up to ~count*1.5s. Returns avg RTT ms, or -1 on failure. */
static float ping_host(IPAddress ip, int count, int* out_recv)
{
    if (count < 1) count = 1;
    if (count > 10) count = 10;

    PingCtx ctx = { 0, 0, false };

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    ip_addr_t target;
    if (!ipaddr_aton(ip.toString().c_str(), &target)) return -1;
    cfg.target_addr  = target;
    cfg.count        = count;
    cfg.interval_ms  = 500;
    cfg.timeout_ms   = 1000;

    esp_ping_callbacks_t cbs = {};
    cbs.on_ping_success = ping_on_success;
    cbs.on_ping_end     = ping_on_end;
    cbs.cb_args         = &ctx;

    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) return -1;
    esp_ping_start(ping);

    uint32_t deadline = millis() + (uint32_t)count * 1500 + 2000;
    while (!ctx.done && (int32_t)(millis() - deadline) < 0) delay(10);
    if (!ctx.done) esp_ping_stop(ping);
    esp_ping_delete_session(ping);

    *out_recv = ctx.recv;
    if (ctx.recv == 0) return -1;
    return (float)ctx.total_ms / ctx.recv;
}

static void mqtt_callback(char* topic, byte* payload, unsigned int length)
{
    String t(topic);
    String body;
    body.reserve(length + 1);
    for (unsigned int i = 0; i < length; i++) body += (char)payload[i];

    /* command channel: atnode/<id>/cmd, payload "<reqid> <method> <query>" */
    if (t == g_mqtt_topic_prefix + "/cmd") {
        int sp1 = body.indexOf(' ');
        if (sp1 < 0) return;
        String reqid  = body.substring(0, sp1);
        int sp2 = body.indexOf(' ', sp1 + 1);
        String method = (sp2 > 0) ? body.substring(sp1 + 1, sp2) : body.substring(sp1 + 1);
        String query  = (sp2 > 0) ? body.substring(sp2 + 1) : "";
        String inner  = mqtt_exec(method, query);
        String resp   = "{\"id\":\"" + reqid + "\"," + inner + "}";
        g_mqtt.publish((g_mqtt_topic_prefix + "/resp").c_str(), resp.c_str());
        return;
    }
    Serial.printf("MQTT [%s] %s\n", topic, body.c_str());
}

static bool verify_fingerprint(const mbedtls_x509_crt* cert, const String& fp_hex)
{
    if (!cert) return false;
    uint8_t hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, cert->raw.p, cert->raw.len);
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", hash[i]);

    String fp = fp_hex;
    fp.replace(":", "");
    fp.toLowerCase();
    return fp.equals(hex);
}

static bool mqtt_connect(void)
{
    if (g_mqtt_broker.length() == 0) return false;
    Serial.printf("MQTT connect to %s:%d ...\n", g_mqtt_broker.c_str(), g_mqtt_port);
    g_mqtt_client_id = "atnode-" + g_hostname;
    g_mqtt_topic_prefix = "atnode/" + g_hostname;

    if (g_mqtt_port == 8883) {
        /* TLS mode */
        g_mqtt.setClient(g_mqtt_wifi_secure);
        if (g_mqtt_ca_fp.length() > 0) {
            g_mqtt_wifi_secure.setInsecure();
        } else if (g_mqtt_ca_cert.length() > 0) {
            g_mqtt_wifi_secure.setCACert(g_mqtt_ca_cert.c_str());
        } else {
            g_mqtt_wifi_secure.setInsecure();
        }
        g_mqtt_wifi_secure.setTimeout(15);   /* seconds (ESP32 Arduino) */
    } else {
        /* Plain TCP mode */
        g_mqtt.setClient(g_mqtt_wifi_plain);
        g_mqtt_wifi_plain.setTimeout(5);     /* seconds */
    }
    g_mqtt.setServer(g_mqtt_broker.c_str(), g_mqtt_port);
    g_mqtt.setCallback(mqtt_callback);
    g_mqtt.setBufferSize(1024);   /* sys/info manifest ~400B */

    String willTopic = g_mqtt_topic_prefix + "/state";
    bool ok;
    if (g_mqtt_user.length() > 0) {
        ok = g_mqtt.connect(g_mqtt_client_id.c_str(), g_mqtt_user.c_str(), g_mqtt_pass.c_str(),
                            willTopic.c_str(), 0, true, "offline");
    } else {
        ok = g_mqtt.connect(g_mqtt_client_id.c_str(),
                            willTopic.c_str(), 0, true, "offline");
    }

    /* verify fingerprint if configured (TLS only) */
    if (ok && g_mqtt_port == 8883 && g_mqtt_ca_fp.length() > 0) {
        if (!verify_fingerprint(g_mqtt_wifi_secure.getPeerCertificate(), g_mqtt_ca_fp)) {
            Serial.println("MQTT fingerprint mismatch, disconnecting");
            g_mqtt.disconnect();
            ok = false;
        }
    }

    if (ok) {
        /* presence + manifest + command subscription */
        g_mqtt.subscribe((g_mqtt_topic_prefix + "/cmd").c_str());
        g_mqtt.publish(willTopic.c_str(), "online", true);
        String info = build_sys_info_json();
        g_mqtt.publish((g_mqtt_topic_prefix + "/info").c_str(), info.c_str(), true);
    }

    g_mqtt_connected = ok;
    Serial.printf("MQTT connect %s\n", ok ? "OK" : "FAILED");
    return ok;
}

static void mqtt_task_func(void* arg)
{
    /* MQTT background task disabled — using main loop with short timeout */
    for (;;) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

static void mqtt_poll(void)
{
    static uint32_t last_attempt = 0;
    /* Auto-(re)connect whenever a broker is configured and WiFi is up.
     * Manual connect (mqtt/connect endpoint) retries fast (1s);
     * unattended reconnect backs off to 10s. mqtt_connect() republishes
     * state/info and resubscribes, so the broker registry self-heals.  */
    bool want = (g_mqtt_broker.length() > 0) && g_wifi_ready;
    if (want && !g_mqtt_connected) {
        uint32_t now = millis();
        uint32_t interval = g_mqtt_connect_pending ? 1000 : 10000;
        if (now - last_attempt > interval) {
            last_attempt = now;
            g_mqtt_connect_pending = false;
            mqtt_connect();
        }
        return;
    }
    if (!g_mqtt_connected) return;
    if (!g_mqtt.loop()) {
        g_mqtt_connected = false;
    }
}

static String mqtt_exec(const String& method, const String& query)
{
    auto err = [](const char* e) { return String("\"ok\":false,\"error\":\"") + e + "\""; };

    if (method == "keyboard/tap") {
        if (!is_connected()) return err("BLE not connected");
        uint8_t mods = parse_uint8(query_get(query, "mods"));
        uint8_t key  = parse_uint8(query_get(query, "k"));
        int ms       = query_get(query, "ms").toInt();
        key_tap(mods, key, ms > 0 ? ms : 100);
        return "\"ok\":true";
    }
    if (method == "keyboard/text") {
        if (!is_connected()) return err("BLE not connected");
        if (g_type_busy)     return err("typing in progress");
        String text = query_get(query, "s");
        if (text.length() == 0) return err("missing s");
        int ms  = query_get(query, "ms").toInt();
        int gap = query_get(query, "gap").toInt();
        g_type_ms   = (ms > 0) ? ms : 40;
        g_type_gap  = (gap > 0) ? gap : 30;
        g_type_text = text;
        g_type_idx  = 0;
        g_type_next = 0;
        g_type_busy = true;
        return "\"ok\":true,\"queued\":true";
    }
    if (method == "keyboard/key") {
        if (!is_connected()) return err("BLE not connected");
        g_key_state.mods = parse_uint8(query_get(query, "mods"));
        for (int i = 0; i < 6; i++) {
            g_key_state.keys[i] = parse_uint8(query_get(query, ("k" + String(i)).c_str()));
        }
        send_report();
        return "\"ok\":true";
    }
    if (method == "gpio/write") {
        int pin   = query_get(query, "pin").toInt();
        int level = query_get(query, "level").toInt();
        pinMode(pin, OUTPUT);
        digitalWrite(pin, level ? HIGH : LOW);
        return "\"ok\":true";
    }
    if (method == "gpio/read") {
        int pin = query_get(query, "pin").toInt();
        pinMode(pin, INPUT_PULLUP);
        return String("\"ok\":true,\"level\":") + digitalRead(pin);
    }
    if (method == "adc/read") {
        int ch = query_get(query, "ch").toInt();
        int mv = analogReadMilliVolts(ch);
        return String("\"ok\":true,\"mv\":") + mv;
    }
    if (method == "ble/status") {
        return String("\"ok\":true,\"ble\":") + build_ble_status_json();
    }
    if (method == "sys/info") {
        return String("\"ok\":true,\"info\":") + build_sys_info_json();
    }
    if (method == "net/wol") {
        String mac = query_get(query, "mac");
        if (mac.length() == 0) return err("missing mac");
        return wol_send(mac) ? "\"ok\":true" : err("wol send failed");
    }
    if (method == "net/ping") {
        String host = query_get(query, "host");
        if (host.length() == 0) return err("missing host");
        int count = query_get(query, "count").toInt();
        IPAddress ip;
        if (WiFi.hostByName(host.c_str(), ip) != 1) return err("dns failed");
        int recv = 0;
        float avg = ping_host(ip, count > 0 ? count : 4, &recv);
        if (avg < 0) {
            return String("\"ok\":false,\"error\":\"no reply\",\"ip\":\"") + ip.toString() +
                   "\",\"recv\":" + recv;
        }
        return String("\"ok\":true,\"ip\":\"") + ip.toString() + "\",\"recv\":" + recv +
               ",\"avg_ms\":" + String(avg, 1);
    }
    return err("unknown method");
}

static void handle_mqtt_status(void)
{
    String json = "{";
    json += "\"connected\":";
    json += g_mqtt_connected ? "true" : "false";
    json += ",\"broker\":\"" + g_mqtt_broker + "\"";
    json += ",\"port\":" + String(g_mqtt_port);
    json += ",\"client_id\":\"" + g_mqtt_client_id + "\"";
    json += ",\"ca_type\":\"";
    if (g_mqtt_ca_fp.length() > 0) {
        json += "fingerprint";
    } else if (g_mqtt_ca_cert.length() > 0) {
        json += "pem";
    } else {
        json += "none";
    }
    json += "\"";
    json += "}";
    send_json(json);
}

static void handle_mqtt_config(void)
{
    String broker = g_http.arg("broker");
    String port   = g_http.arg("port");
    String user   = g_http.arg("user");
    String pass   = g_http.arg("pass");
    if (broker.length() > 0) {
        g_mqtt_broker = broker;
        save_config("mqtt_broker", broker);
    }
    if (port.length() > 0) {
        g_mqtt_port = port.toInt();
        save_config("mqtt_port", port);
    }
    if (user.length() > 0) {
        g_mqtt_user = user;
        save_config("mqtt_user", user);
    }
    if (pass.length() > 0) {
        g_mqtt_pass = pass;
        save_config("mqtt_pass", pass);
    }
    send_json("{\"ok\":true,\"cmd\":\"mqtt/config\"}");
}

static void handle_mqtt_ca(void)
{
    String ca_cert = g_http.arg("plain");
    String ca_fp   = g_http.arg("fp");
    if (ca_cert.length() > 0) {
        g_mqtt_ca_cert = ca_cert;
        save_config("mqtt_ca_cert", ca_cert);
    }
    if (ca_fp.length() > 0) {
        g_mqtt_ca_fp = ca_fp;
        save_config("mqtt_ca_fp", ca_fp);
    }
    send_json("{\"ok\":true,\"cmd\":\"mqtt/ca\"}");
}

static void handle_wifi_config(void)
{
    String ssid = g_http.arg("ssid");
    String pass = g_http.arg("pass");
    if (ssid.length() > 0) {
        g_wifi_ssid = ssid;
        save_config("wifi_ssid", ssid);
    }
    if (pass.length() > 0) {
        g_wifi_pass = pass;
        save_config("wifi_pass", pass);
    }
    send_json("{\"ok\":true,\"cmd\":\"wifi/config\",\"ssid\":\"" + g_wifi_ssid + "\"}");
}

static void handle_mqtt_connect(void)
{
    g_mqtt_connect_pending = true;
    send_json("{\"ok\":true,\"cmd\":\"mqtt/connect\",\"queued\":true}");
}

static void handle_mqtt_publish(void)
{
    if (!g_mqtt_connected) {
        send_json("{\"ok\":false,\"error\":\"mqtt not connected\"}", 409);
        return;
    }
    String topic = g_http.arg("topic");
    String msg   = g_http.arg("msg");
    if (topic.length() == 0) {
        send_json("{\"ok\":false,\"error\":\"missing topic\"}", 400);
        return;
    }
    bool ok = g_mqtt.publish(topic.c_str(), msg.c_str());
    send_json("{\"ok\":" + String(ok ? "true" : "false") +
              ",\"cmd\":\"mqtt/publish\"}");
}

static void handle_mqtt_subscribe(void)
{
    if (!g_mqtt_connected) {
        send_json("{\"ok\":false,\"error\":\"mqtt not connected\"}", 409);
        return;
    }
    String topic = g_http.arg("topic");
    if (topic.length() == 0) {
        send_json("{\"ok\":false,\"error\":\"missing topic\"}", 400);
        return;
    }
    bool ok = g_mqtt.subscribe(topic.c_str());
    send_json("{\"ok\":" + String(ok ? "true" : "false") +
              ",\"cmd\":\"mqtt/subscribe\"}");
}

static void handle_ap(void)
{
    String val = g_http.arg("plain");
    if (val.length() == 0) val = g_http.arg("v");
    val.trim();
    if (val == "1") {
        ap_portal_start();
        send_json("{\"ok\":true,\"cmd\":\"ap\",\"active\":true}");
    } else if (val == "0") {
        ap_portal_stop();
        send_json("{\"ok\":true,\"cmd\":\"ap\",\"active\":false}");
    } else {
        send_json("{\"ok\":false,\"error\":\"expected 1=start,0=stop\"}", 400);
    }
}

static void handle_net_wol(void)
{
    String mac = g_http.arg("mac");
    if (mac.length() == 0) {
        send_json("{\"ok\":false,\"error\":\"missing mac\"}", 400);
        return;
    }
    bool ok = wol_send(mac);
    send_json("{\"ok\":" + String(ok ? "true" : "false") +
              ",\"cmd\":\"net/wol\",\"mac\":\"" + mac + "\"}");
}

static void handle_net_ping(void)
{
    String host = g_http.arg("host");
    if (host.length() == 0) {
        send_json("{\"ok\":false,\"error\":\"missing host\"}", 400);
        return;
    }
    int count = g_http.arg("count").toInt();
    IPAddress ip;
    if (WiFi.hostByName(host.c_str(), ip) != 1) {
        send_json("{\"ok\":false,\"error\":\"dns failed\"}", 500);
        return;
    }
    int recv = 0;
    float avg = ping_host(ip, count > 0 ? count : 4, &recv);
    String json = "{\"ok\":" + String(avg >= 0 ? "true" : "false");
    json += ",\"cmd\":\"net/ping\",\"ip\":\"" + ip.toString() + "\"";
    json += ",\"sent\":" + String(count > 0 ? count : 4);
    json += ",\"recv\":" + String(recv);
    if (avg >= 0) json += ",\"avg_ms\":" + String(avg, 1);
    json += "}";
    send_json(json, avg >= 0 ? 200 : 500);
}

static void handle_not_found(void)
{
    send_json("{\"ok\":false,\"error\":\"not found\"}", 404);
}

/* --- BLE init ---------------------------------------------------------- */
static bool ble_init(void)
{
    NimBLEDevice::init(g_device_name.c_str());
    String pub = NimBLEDevice::getAddress().toString().c_str();
    strncpy(g_ble_addr_str, pub.c_str(), sizeof(g_ble_addr_str) - 1);
    /* Bonding always allowed (same as the verified demo): bonding,
     * no MITM, no secure connections -> Just Works pairing.           */
    NimBLEDevice::setSecurityAuth(true, false, false);

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(new AtNodeServerCallbacks());
    g_server->advertiseOnDisconnect(true);

    g_hid = new NimBLEHIDDevice(g_server);
    g_hid->setManufacturer("AT-Node");
    g_hid->setPnp(0x02, 0xE502, 0xA111, 0x0210);
    g_hid->setHidInfo(0x00, 0x03);
    g_hid->setBatteryLevel(100);
    g_hid->setReportMap(const_cast<uint8_t*>(REPORT_MAP), sizeof(REPORT_MAP));


    /* Report characteristics (0x2A4D) with Report Reference (0x2908)
     * descriptors - required by Windows HID-over-GATT driver.
     * getInputReport/getOutputReport create them automatically. */
    g_inputReport = g_hid->getInputReport(1);
    uint8_t empty_report[8] = {0};
    g_inputReport->setValue(empty_report, sizeof(empty_report));

    g_outputReport = g_hid->getOutputReport(2);
    g_outputReport->setCallbacks(new LedOutputCallbacks());


    g_hid->startServices();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setName(g_device_name.c_str());
    adv->setAppearance(HID_KEYBOARD);
    adv->addServiceUUID(g_hid->getHidService()->getUUID());
    adv->enableScanResponse(false);
    adv->start();

    Serial.println("BLE keyboard started: " + g_device_name);
    return true;
}

/* --- serial AT parser --------------------------------------------------- */
static void serial_exec(const String& line)
{
    if (line == "AT") {
        Serial.println("OK");
    } else if (line == "AT+HELP") {
        Serial.println("AT-Node ESP32 commands:");
        Serial.println("  AT / AT+STATUS / AT+VER / AT+HELP");
        Serial.println("  AT+TAP=<ms>,<mods>,<key>     press+release");
        Serial.println("  AT+KEY=<mods>,<k1>..<k6>     raw HID report");
        Serial.println("  AT+MOD=<mask>                modifiers only");
        Serial.println("  AT+KEY_SEQ=<ms>,<mods>,<k1..6>,... batch reports");
        Serial.println("  AT+TEXT=<text>               type ASCII text");
        Serial.println("  AT+CONF=<key>=<val>          name/hostname (NVS)");
        Serial.println("  AT+BT_LIST / AT+BT_DISC / AT+BT_PAIR");
        Serial.println("  AT+GPIO_W=<pin>,<level> / AT+GPIO_R=<pin>");
        Serial.println("  AT+ADC=<ch> / AT+I2C_SCAN / AT+I2C_R / AT+I2C_W");
        Serial.println("  AT+IR=<NEC|SIRC|RAW>,...");
        Serial.println("  AT+WIFI=ssid|pass|status,<val>");
        Serial.println("  AT+MQTT=broker|port,<val> connect|status|clear");
        Serial.println("  AT+AP=<1|0>                  provisioning AP");
    } else if (line == "AT+VER") {
        Serial.println("AT-Node v1.0 [esp32]");
        Serial.println("OK");
    } else if (line == "AT+STATUS") {
        Serial.print("role=esp32_at_node connected=");
        Serial.print(is_connected() ? "1" : "0");
        Serial.print(" ip=");
        Serial.println(WiFi.localIP().toString());
    } else if (line.startsWith("AT+TAP=")) {
        String args = line.substring(7);
        int c1 = args.indexOf(',');
        int c2 = args.indexOf(',', c1 + 1);
        if (c1 > 0 && c2 > c1) {
            int ms   = args.substring(0, c1).toInt();
            int mods = args.substring(c1 + 1, c2).toInt();
            int key  = args.substring(c2 + 1).toInt();
            key_tap((uint8_t)mods, (uint8_t)key, ms);
            Serial.println("OK");
        } else {
            Serial.println("ERROR");
        }
    } else if (line.startsWith("AT+KEY=")) {
        String args = line.substring(7);
        g_key_state.mods = parse_uint8(args);
        for (int i = 0; i < 6; i++) {
            int comma = args.indexOf(',');
            String part = (comma > 0) ? args.substring(0, comma) : args;
            g_key_state.keys[i] = parse_uint8(part);
            if (comma > 0) args = args.substring(comma + 1);
        }
        send_report();
        Serial.println("OK");
    } else if (line.startsWith("AT+MOD=")) {
        g_key_state.mods = parse_uint8(line.substring(7));
        send_report();
        Serial.println("OK");
    } else if (line.startsWith("AT+KEY_SEQ=")) {
        /* AT+KEY_SEQ=<delay_ms>,<mods>,<k1>..<k6>,... (groups of 7) */
        String args = line.substring(11);
        int vals[128];
        int n = 0;
        int start = 0;
        while (n < 128) {
            int comma = args.indexOf(',', start);
            String part = (comma > 0) ? args.substring(start, comma) : args.substring(start);
            vals[n++] = (int)parse_uint8(part);
            if (comma < 0) break;
            start = comma + 1;
        }
        if (n < 8 || ((n - 1) % 7) != 0) {
            Serial.println("ERROR: usage AT+KEY_SEQ=<ms>,<mods>,<k1..6>,...");
        } else {
            int d = vals[0];
            if (d < 1) d = 1;
            if (d > 200) d = 200;
            int reports = (n - 1) / 7;
            for (int r = 0; r < reports; r++) {
                g_key_state.mods = (uint8_t)vals[1 + r * 7];
                for (int i = 0; i < 6; i++)
                    g_key_state.keys[i] = (uint8_t)vals[1 + r * 7 + 1 + i];
                send_report();
                delay(d);
            }
            Serial.printf("%d reports sent\n", reports);
            Serial.println("OK");
        }
    } else if (line == "AT+BT_LIST") {
        int nb = NimBLEDevice::getNumBonds();
        if (nb == 0) {
            Serial.println("+BT_LIST:none");
        }
        for (int i = 0; i < nb; i++) {
            Serial.printf("+BT_LIST:%d %s\n", i,
                          NimBLEDevice::getBondedAddress(i).toString().c_str());
        }
        Serial.println("OK");
    } else if (line == "AT+BT_DISC") {
        if (is_connected()) {
            std::vector<uint16_t> peers = g_server->getPeerDevices();
            for (uint16_t h : peers) g_server->disconnect(h);
            Serial.println("OK");
        } else {
            Serial.println("ERROR: not connected");
        }
    } else if (line == "AT+BT_PAIR") {
        if (is_connected()) {
            std::vector<uint16_t> peers = g_server->getPeerDevices();
            for (uint16_t h : peers) g_server->disconnect(h);
        }
        bool ok = NimBLEDevice::deleteAllBonds();
        Serial.printf("bonds erased: %d\n", ok);
        Serial.println(ok ? "OK" : "ERROR");
    } else if (line.startsWith("AT+TEXT=")) {
        String text = line.substring(8);
        g_type_text = text;
        g_type_idx  = 0;
        g_type_busy = true;
        Serial.println("OK");
    } else if (line.startsWith("AT+CONF=")) {
        String kv = line.substring(8);
        int eq = kv.indexOf('=');
        if (eq > 0) {
            String key = kv.substring(0, eq);
            String val = kv.substring(eq + 1);
            save_config(key, val);
            if (key == "name") g_device_name = val;
            if (key == "hostname") g_hostname = val;
            Serial.println("OK");
        } else {
            Serial.println("ERROR");
        }
    } else if (line.startsWith("AT+GPIO_W=")) {
        String args = line.substring(10);
        int c1 = args.indexOf(',');
        if (c1 > 0) {
            int pin = args.substring(0, c1).toInt();
            int level = args.substring(c1 + 1).toInt();
            pinMode(pin, OUTPUT);
            digitalWrite(pin, level ? HIGH : LOW);
            Serial.println("OK");
        } else {
            Serial.println("ERROR");
        }
    } else if (line.startsWith("AT+GPIO_R=")) {
        int pin = line.substring(10).toInt();
        pinMode(pin, INPUT_PULLUP);
        int level = digitalRead(pin);
        Serial.print("+GPIO_R:");
        Serial.println(level);
        Serial.println("OK");
    } else if (line.startsWith("AT+ADC=")) {
        int ch = line.substring(7).toInt();
        int mv = analogReadMilliVolts(ch);
        Serial.print("+ADC:");
        Serial.println(mv);
        Serial.println("OK");
    } else if (line == "AT+I2C_SCAN") {
        for (uint8_t addr = 1; addr < 127; addr++) {
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) {
                Serial.print("+I2C_SCAN:0x");
                Serial.println(addr, HEX);
            }
        }
        Serial.println("OK");
    } else if (line.startsWith("AT+I2C_R=")) {
        String args = line.substring(9);
        int c1 = args.indexOf(',');
        int c2 = args.indexOf(',', c1 + 1);
        if (c1 > 0 && c2 > c1) {
            int addr = strtoul(args.substring(0, c1).c_str(), NULL, 0);
            int reg  = strtoul(args.substring(c1 + 1, c2).c_str(), NULL, 0);
            int len  = args.substring(c2 + 1).toInt();
            Wire.beginTransmission(addr);
            Wire.write(reg);
            Wire.endTransmission(false);
            Wire.requestFrom(addr, len);
            Serial.print("+I2C_R:");
            while (Wire.available()) {
                uint8_t b = Wire.read();
                if (b < 0x10) Serial.print("0");
                Serial.print(b, HEX);
                Serial.print(" ");
            }
            Serial.println();
            Serial.println("OK");
        } else {
            Serial.println("ERROR");
        }
    } else if (line.startsWith("AT+I2C_W=")) {
        String args = line.substring(9);
        int c1 = args.indexOf(',');
        int c2 = args.indexOf(',', c1 + 1);
        if (c1 > 0 && c2 > c1) {
            int addr = strtoul(args.substring(0, c1).c_str(), NULL, 0);
            int reg  = strtoul(args.substring(c1 + 1, c2).c_str(), NULL, 0);
            String hexData = args.substring(c2 + 1);
            hexData.replace(" ", "");
            Wire.beginTransmission(addr);
            Wire.write(reg);
            for (int i = 0; i < hexData.length(); i += 2) {
                String byteStr = hexData.substring(i, i + 2);
                uint8_t b = (uint8_t)strtoul(byteStr.c_str(), NULL, 16);
                Wire.write(b);
            }
            if (Wire.endTransmission() == 0) {
                Serial.println("OK");
            } else {
                Serial.println("ERROR");
            }
        } else {
            Serial.println("ERROR");
        }
    } else if (line.startsWith("AT+IR=")) {
        String args = line.substring(6);
        int c1 = args.indexOf(',');
        if (c1 > 0) {
            String proto = args.substring(0, c1);
            String data = args.substring(c1 + 1);
            bool ok = false;
            if (proto.equalsIgnoreCase("NEC")) {
                uint32_t d = strtoul(data.c_str(), NULL, 0);
                ok = ir_send_nec(d);
            } else if (proto.equalsIgnoreCase("SIRC")) {
                int c2 = data.indexOf(',');
                uint32_t d = strtoul(data.substring(0, c2).c_str(), NULL, 0);
                int bits = data.substring(c2 + 1).toInt();
                ok = ir_send_sirc(d, bits);
            } else if (proto.equalsIgnoreCase("RAW")) {
                uint16_t timings[256];
                int count = 0;
                int start = 0;
                while (count < 256) {
                    int comma = data.indexOf(',', start);
                    String part = (comma > 0) ? data.substring(start, comma) : data.substring(start);
                    timings[count++] = (uint16_t)part.toInt();
                    if (comma < 0) break;
                    start = comma + 1;
                }
                ok = ir_send_raw(timings, count);
            } else {
                Serial.println("ERROR");
                return;
            }
            if (ok) Serial.println("OK");
            else Serial.println("ERROR");
        } else {
            Serial.println("ERROR");
        }
    } else if (line.startsWith("AT+MQTT=")) {
        String args = line.substring(8);
        int c1 = args.indexOf(',');
        if (args.length() > 0) {   /* comma optional: sub-only commands (clear/status/connect) */
            String sub = (c1 > 0) ? args.substring(0, c1) : args;
            String val = (c1 > 0) ? args.substring(c1 + 1) : "";
            if (sub == "broker") {
                g_mqtt_broker = val;
                save_config("mqtt_broker", val);
                Serial.println("OK");
            } else if (sub == "clear") {
                mqtt_clear_config();
                Serial.println("OK");
            } else if (sub == "port") {
                g_mqtt_port = val.toInt();
                save_config("mqtt_port", val);
                Serial.println("OK");
            } else if (sub == "connect") {
                bool ok = mqtt_connect();
                Serial.println(ok ? "OK" : "ERROR");
            } else if (sub == "status") {
                Serial.print("+MQTT:");
                Serial.println(g_mqtt_connected ? "connected" : "disconnected");
                Serial.println("OK");
            } else if (sub == "ca") {
                /* val should be the CA cert PEM or SHA256 fingerprint, or "status" */
                if (val == "status") {
                    if (g_mqtt_ca_fp.length() > 0) {
                        Serial.println("+MQTT_CA:fingerprint");
                    } else if (g_mqtt_ca_cert.length() > 0) {
                        Serial.println("+MQTT_CA:pem");
                    } else {
                        Serial.println("+MQTT_CA:none");
                    }
                } else if (val.startsWith("-----BEGIN")) {
                    g_mqtt_ca_cert = val;
                    save_config("mqtt_ca_cert", val);
                    Serial.println("OK");
                } else {
                    g_mqtt_ca_fp = val;
                    save_config("mqtt_ca_fp", val);
                    Serial.println("OK");
                }
            } else {
                Serial.println("ERROR");
            }
        } else {
            Serial.println("ERROR");
        }
    } else if (line.startsWith("AT+WIFI=")) {
        String args = line.substring(8);
        int c1 = args.indexOf(',');
        if (c1 > 0) {
            String sub = args.substring(0, c1);
            String val = args.substring(c1 + 1);
            if (sub == "ssid") {
                g_wifi_ssid = val;
                save_config("wifi_ssid", val);
                Serial.println("OK");
            } else if (sub == "pass") {
                g_wifi_pass = val;
                save_config("wifi_pass", val);
                Serial.println("OK");
            } else if (sub == "status") {
                Serial.print("+WIFI:");
                Serial.println(g_wifi_ssid);
                Serial.println("OK");
            } else {
                Serial.println("ERROR");
            }
        } else {
            Serial.println("ERROR");
        }
    } else if (line.startsWith("AT+AP=")) {
        int val = line.substring(6).toInt();
        if (val == 1) {
            ap_portal_start();
            Serial.println("OK");
        } else if (val == 0) {
            ap_portal_stop();
            Serial.println("OK");
        } else {
            Serial.println("ERROR");
        }
    } else {
        Serial.println("ERROR");
    }
}

/* Non-blocking serial reader: accumulates chars, executes on CR or LF.
 * (readStringUntil has a 1000ms default timeout - any command sent
 * without LF, e.g. CR-only terminals, stalled 1s per command.)
 * CRLF terminals: CR executes, the following LF is an empty line and
 * is skipped.                                                    */
static void handle_serial(void)
{
    static String line;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r' || c == '\n') {
            line.trim();
            if (line.length() > 0) serial_exec(line);
            line = "";
        } else if (line.length() < 255) {
            line += c;
        } else {
            line = "";   /* overflow: drop garbage */
        }
    }
}

/* --- setup / loop ------------------------------------------------------ */
void setup(void)
{
    Serial.begin(115200);
    delay(500);
    Serial.println("\r\nesp32_at_node start");

    load_config();

    /* Check AP trigger button (GPIO10) */
    bool ap_triggered = ap_portal_check_button();

    WiFi.mode(WIFI_STA);
    WiFi.begin(g_wifi_ssid.c_str(), g_wifi_pass.c_str());

    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 60) {
        delay(500);
        Serial.print(".");
        retry++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        g_wifi_ready = true;
        Serial.println();
        Serial.print("WiFi connected, IP=");
        Serial.println(WiFi.localIP());

        if (MDNS.begin(g_hostname.c_str())) {
            Serial.println("mDNS: " + g_hostname + ".local");
        } else {
            Serial.println("mDNS init failed");
        }

        g_http.on("/", HTTP_GET, handle_root);
        g_http.on("/at-node/status", HTTP_GET, handle_status_html);
        g_http.on("/at-node/cmd/status", HTTP_GET, handle_cmd_status);
        g_http.on("/at-node/help", HTTP_GET, handle_help_html);
        g_http.on("/at-node/pair", HTTP_GET, handle_pair_html);
        g_http.on("/at-node/at", HTTP_POST, handle_at);
        g_http.on("/at-node/cmd/keyboard/tap", HTTP_POST, handle_keyboard_tap);
        g_http.on("/at-node/cmd/keyboard/text", HTTP_POST, handle_keyboard_text);
        g_http.on("/at-node/cmd/keyboard/key", HTTP_POST, handle_keyboard_key);
        g_http.on("/at-node/cmd/ble/status", HTTP_GET, handle_ble_status);
        g_http.on("/at-node/cmd/ble/advertise", HTTP_POST, handle_ble_advertise);
        g_http.on("/at-node/cmd/ble/bonds/delete", HTTP_POST, handle_ble_bonds_delete);
        g_http.on("/at-node/cmd/ble/bonds/clear", HTTP_POST, handle_ble_bonds_clear);
        g_http.on("/at-node/cmd/gpio/write", HTTP_POST, handle_gpio_write);
        g_http.on("/at-node/cmd/gpio/read", HTTP_POST, handle_gpio_read);
        g_http.on("/at-node/cmd/adc/read", HTTP_POST, handle_adc_read);
        g_http.on("/at-node/cmd/i2c/scan", HTTP_POST, handle_i2c_scan);
        g_http.on("/at-node/cmd/i2c/read", HTTP_POST, handle_i2c_read);
        g_http.on("/at-node/cmd/i2c/write", HTTP_POST, handle_i2c_write);
        g_http.on("/at-node/cmd/ir/send", HTTP_POST, handle_ir_send);
        g_http.on("/at-node/cmd/mqtt/status", HTTP_GET, handle_mqtt_status);
        g_http.on("/at-node/cmd/mqtt/config", HTTP_POST, handle_mqtt_config);
        g_http.on("/at-node/cmd/mqtt/ca", HTTP_POST, handle_mqtt_ca);
        g_http.on("/at-node/cmd/mqtt/connect", HTTP_POST, handle_mqtt_connect);
        g_http.on("/at-node/cmd/mqtt/publish", HTTP_POST, handle_mqtt_publish);
        g_http.on("/at-node/cmd/mqtt/subscribe", HTTP_POST, handle_mqtt_subscribe);
        g_http.on("/at-node/cmd/wifi/config", HTTP_POST, handle_wifi_config);
        g_http.on("/at-node/cmd/ap", HTTP_POST, handle_ap);
        g_http.on("/at-node/cmd/net/wol", HTTP_POST, handle_net_wol);
        g_http.on("/at-node/cmd/net/ping", HTTP_POST, handle_net_ping);
        g_http.onNotFound(handle_not_found);
        g_http.begin();
        Serial.println("HTTP server on port 80");
    } else {
        Serial.println("\r\nWiFi connection failed, HTTP disabled");
    }

    /* I2C: SDA=GPIO8, SCL=GPIO9 (ESP32-C3 default) */
    Wire.begin(8, 9);
    Serial.println("I2C initialized (SDA=8, SCL=9)");

    /* IR: RMT on GPIO4 */
    if (ir_init()) {
        Serial.println("IR initialized (GPIO4, 38kHz carrier)");
    } else {
        Serial.println("IR init failed");
    }

    ble_init();

    /* MQTT background task disabled — using main loop with short timeout */
}

void loop(void)
{
    if (g_wifi_ready) g_http.handleClient();
    handle_serial();
    type_poll();
    mqtt_poll();
    ap_portal_poll();
    if (g_adv_restart_at && (int32_t)(millis() - g_adv_restart_at) >= 0) {
        g_adv_restart_at = 0;
        if (!is_connected()) {
            NimBLEDevice::getAdvertising()->stop();   /* clear stale state */
            NimBLEDevice::startAdvertising();
            Serial.println("advertising restarted");
        }
    }
    delay(2);
}
