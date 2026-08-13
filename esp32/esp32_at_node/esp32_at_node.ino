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
#include <esp_heap_caps.h>
#include "ap_portal.h"
#include "rathole_client.h"
#include <NimBLEServer.h>
#include <NimBLEHIDDevice.h>
#include <NimBLECharacteristic.h>
#include "wifi_config.h"
#include "web_page.h"   /* gzipped single-page web UI (esp32/web/build.py) */

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
/* Security policy: the unauthenticated HTTP control plane is intended for
 * trusted local NAT networks only. On untrusted networks the operator should
 * disable it (AT+HTTP=0, persisted to NVS) and keep only the MQTT (TLS) plane. */
static bool      g_http_enabled = true;
static bool      g_pairing_mode = false;   /* default off for security */
static uint32_t  g_pair_timeout_at = 0;    /* auto-stop advertising deadline */

/* deferred restart (used by NVS clear / factory reset) */
static uint32_t  g_restart_at = 0;

/* --- MQTT client ------------------------------------------------------- */
static WiFiClient       g_mqtt_wifi_plain;
static WiFiClientSecure g_mqtt_wifi_secure;
static PubSubClient     g_mqtt(g_mqtt_wifi_plain);
static bool             g_mqtt_connected = false;
static bool             g_mqtt_connect_pending = false;
static bool             g_mqtt_auto = false;   /* auto-reconnect on boot (NVS) */
static String           g_mqtt_broker;
static TaskHandle_t     g_mqtt_task = NULL;
static SemaphoreHandle_t g_mqtt_sem = NULL;
static int              g_mqtt_port = 8883;
static String           g_mqtt_user;
static String           g_mqtt_pass;
static String           g_mqtt_client_id;
static String           g_mqtt_topic_prefix;
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
static bool              g_directed_adv_pending = false;
static NimBLEAddress     g_directed_adv_addr;

/* --- BLE callbacks ---------------------------------------------------- */
class AtNodeServerCallbacks : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        Serial.println("BLE connected");
        /* Public pairing mode ends once a host connects. */
        g_pairing_mode = false;
        g_pair_timeout_at = 0;
        g_adv_restart_at = 0;
        g_directed_adv_pending = false;
    }
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo,
                       int reason) override {
        Serial.printf("BLE disconnected, reason=0x%02X bonded=%d\n",
                      reason, connInfo.isBonded());
        /* After disconnect: public pairing mode is over.
         * If the peer is bonded, advertise privately/directed to it so only
         * that host can reconnect. Otherwise stop advertising. */
        if (connInfo.isBonded()) {
            g_directed_adv_addr = connInfo.getAddress();
            g_directed_adv_pending = true;
            g_adv_restart_at = millis() + 500;
        } else {
            g_directed_adv_pending = false;
            NimBLEDevice::getAdvertising()->stop();
        }
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
    g_mqtt_ca_fp   = prefs.getString("mqtt_ca_fp", "");
    g_mqtt_auto    = prefs.getString("mqtt_auto", "0").toInt() != 0;
    g_http_enabled = prefs.getString("http_enable", "1").toInt() != 0;
    prefs.end();
}

void save_config(const String& key, const String& value)
{
    prefs.begin("atnode", false);
    prefs.putString(key.c_str(), value);
    prefs.end();
}

/* Accessor for rathole_client.cpp (g_mqtt_connected is file-static). */
bool mqtt_is_connected(void)
{
    return g_mqtt_connected;
}

/* Enable/disable the HTTP control plane (persisted to NVS as http_enable).
 * Security policy: HTTP is unauthenticated and intended for trusted local
 * NAT networks only; disable it (enable=false) on untrusted networks and
 * rely on the MQTT (TLS) control plane instead. */
static void set_http_enabled(bool enable, bool persist = true)
{
    g_http_enabled = enable;
    if (persist) save_config("http_enable", enable ? "1" : "0");
    if (!g_wifi_ready) return;
    if (enable) {
        g_http.begin();
        Serial.println("HTTP server started (trusted local NAT only; AT+HTTP=0 to disable)");
    } else {
        g_http.stop();
        Serial.println("HTTP server stopped");
    }
}

/* Start/stop public pairing-mode advertising.
 * This is a runtime state only; it is NOT persisted to NVS.
 * When enabled, public advertising runs for 60s and then stops automatically
 * if no host connects. After a connection/disconnection, the device falls
 * back to directed advertising to the bonded peer (or stops if unpaired). */
static void set_ble_adv_enabled(bool enable)
{
    g_pairing_mode = enable;
    g_adv_restart_at = 0;
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (enable) {
        adv->stop();
        adv->start();
        g_pair_timeout_at = millis() + 60000;
        Serial.println("BLE public advertising started (60s pairing timeout)");
    } else {
        adv->stop();
        g_pair_timeout_at = 0;
        Serial.println("BLE advertising stopped");
    }
}

static void ble_adv_poll(void)
{
    if (!g_pairing_mode) return;
    if (is_connected()) {
        g_pair_timeout_at = 0;
        return;
    }
    if (g_pair_timeout_at && (int32_t)(millis() - g_pair_timeout_at) >= 0) {
        g_pair_timeout_at = 0;
        Serial.println("BLE pairing timeout, stopping public advertising");
        g_pairing_mode = false;
        NimBLEDevice::getAdvertising()->stop();
    }
}

/* Clear all keys in the atnode NVS namespace and reset runtime HTTP state. */
static void nvs_clear_config(void)
{
    prefs.begin("atnode", false);
    prefs.clear();
    prefs.end();
    g_http_enabled = true;
}

static void schedule_restart(uint32_t delay_ms)
{
    g_restart_at = millis() + delay_ms;
}

/* Clear all MQTT settings (NVS + runtime) and disconnect. */
static void mqtt_clear_config(void)
{
    prefs.begin("atnode", false);
    prefs.remove("mqtt_broker");
    prefs.remove("mqtt_port");
    prefs.remove("mqtt_user");
    prefs.remove("mqtt_pass");
    prefs.remove("mqtt_ca_fp");
    prefs.remove("mqtt_auto");
    prefs.end();
    if (g_mqtt_connected) {
        g_mqtt.disconnect();
        g_mqtt_connected = false;
    }
    g_mqtt_broker  = "";
    g_mqtt_port    = 8883;
    g_mqtt_user    = "";
    g_mqtt_pass    = "";
    g_mqtt_ca_fp   = "";
    g_mqtt_auto    = false;
}

/* --- unified configuration layer ----------------------------------------
 * THE single entry point for every persistent setting. AT (AT+SET/GET/KEYS),
 * HTTP (/at-node/cmd/config) and MQTT (config/* methods) all delegate here;
 * legacy commands/endpoints are thin aliases over the same registry.
 * NVS keys are unchanged, so previously stored values stay valid.       */
static bool cfg_truthy(const String& v) { return v == "1" || v == "true"; }

static bool cfg_s_name(const String& v)  { g_device_name = v; save_config("name", v); return true; }
static bool cfg_s_hostn(const String& v) { g_hostname = v;    save_config("hostname", v); return true; }
static bool cfg_s_wssid(const String& v) { g_wifi_ssid = v;   save_config("wifi_ssid", v); return true; }
static bool cfg_s_wpass(const String& v) { g_wifi_pass = v;   save_config("wifi_pass", v); return true; }
static bool cfg_s_mbroker(const String& v) { g_mqtt_broker = v; save_config("mqtt_broker", v); return true; }
static bool cfg_s_mport(const String& v) {
    int p = v.toInt();
    if (p <= 0 || p > 65535) return false;
    g_mqtt_port = p;
    save_config("mqtt_port", String(p));
    return true;
}
static bool cfg_s_muser(const String& v) { g_mqtt_user = v; save_config("mqtt_user", v); return true; }
static bool cfg_s_mpass(const String& v) { g_mqtt_pass = v; save_config("mqtt_pass", v); return true; }
static bool cfg_s_mca(const String& v)   { g_mqtt_ca_fp = v; save_config("mqtt_ca_fp", v); return true; }
static bool cfg_s_mauto(const String& v) { g_mqtt_auto = cfg_truthy(v); save_config("mqtt_auto", g_mqtt_auto ? "1" : "0"); return true; }
static bool cfg_s_httpen(const String& v) { set_http_enabled(cfg_truthy(v)); return true; }
static bool cfg_s_tunen(const String& v)  { rathole_set_enabled(cfg_truthy(v)); return true; }

struct CfgEntry {
    const char* key;
    bool        secret;             /* write-only; get/list mask the value */
    bool   (*set)(const String&);
    String (*get)(void);
};

static const CfgEntry CFG_TABLE[] = {
    {"device.name",     false, cfg_s_name,    [](void){ return g_device_name; }},
    {"device.hostname", false, cfg_s_hostn,   [](void){ return g_hostname; }},
    {"wifi.ssid",       false, cfg_s_wssid,   [](void){ return g_wifi_ssid; }},
    {"wifi.pass",       true,  cfg_s_wpass,   [](void){ return String(); }},
    {"mqtt.broker",     false, cfg_s_mbroker, [](void){ return g_mqtt_broker; }},
    {"mqtt.port",       false, cfg_s_mport,   [](void){ return String(g_mqtt_port); }},
    {"mqtt.user",       false, cfg_s_muser,   [](void){ return g_mqtt_user; }},
    {"mqtt.pass",       true,  cfg_s_mpass,   [](void){ return String(); }},
    {"mqtt.ca",         false, cfg_s_mca,     [](void){ return g_mqtt_ca_fp; }},
    {"mqtt.auto",       false, cfg_s_mauto,   [](void){ return String(g_mqtt_auto ? 1 : 0); }},
    {"http.enable",     false, cfg_s_httpen,  [](void){ return String(g_http_enabled ? 1 : 0); }},
    {"rathole.enable",  false, cfg_s_tunen,   [](void){ return String(rathole_is_enabled() ? 1 : 0); }},
};
#define CFG_TABLE_COUNT (sizeof(CFG_TABLE) / sizeof(CFG_TABLE[0]))

/* tunnel.1.<server|token|service|local|auto|retry> — delegated to the
 * rathole module, which owns its NVS keys and restart semantics.       */
static bool config_tunnel_key(const String& key, int* idx, String* field)
{
    if (!key.startsWith("tunnel.")) return false;
    int dot = key.indexOf('.', 7);
    if (dot < 0) return false;
    int id = key.substring(7, dot).toInt();
    if (id < 1 || id > RATHOLE_MAX_TUNNELS) return false;
    *idx = id - 1;
    *field = key.substring(dot + 1);
    return true;
}

static bool config_set(const String& key, const String& val)
{
    int idx;
    String field;
    if (config_tunnel_key(key, &idx, &field)) return rathole_set(idx, field, val);
    for (uint8_t i = 0; i < CFG_TABLE_COUNT; i++) {
        if (key == CFG_TABLE[i].key) return CFG_TABLE[i].set(val);
    }
    return false;
}

static String config_get(const String& key)
{
    int idx;
    String field;
    if (config_tunnel_key(key, &idx, &field)) {
        return (field == "token") ? String() : rathole_get(idx, field);
    }
    for (uint8_t i = 0; i < CFG_TABLE_COUNT; i++) {
        if (key == CFG_TABLE[i].key) return CFG_TABLE[i].get();
    }
    return String();
}

static bool config_known(const String& key)
{
    int idx;
    String field;
    if (config_tunnel_key(key, &idx, &field)) {
        return field == "server" || field == "token" || field == "service" ||
               field == "local" || field == "auto" || field == "retry" ||
               field == "enable";
    }
    for (uint8_t i = 0; i < CFG_TABLE_COUNT; i++) {
        if (key == CFG_TABLE[i].key) return true;
    }
    return false;
}

static String config_list_json(void)
{
    String j = "[";
    for (uint8_t i = 0; i < CFG_TABLE_COUNT; i++) {
        if (i) j += ",";
        j += "{\"key\":\"" + String(CFG_TABLE[i].key) + "\"";
        if (CFG_TABLE[i].secret) {
            j += ",\"secret\":true";
        } else {
            j += ",\"value\":\"" + CFG_TABLE[i].get() + "\"";
        }
        j += "}";
    }
    static const char* tfields[] = {"server", "token", "service", "local", "auto", "retry", "enable"};
    for (int t = 0; t < RATHOLE_MAX_TUNNELS; t++) {
        for (uint8_t f = 0; f < 7; f++) {
            j += ",{\"key\":\"tunnel." + String(t + 1) + "." + tfields[f] + "\"";
            if (String(tfields[f]) == "token") {
                j += ",\"secret\":true";
            } else {
                j += ",\"value\":\"" + rathole_get(t, tfields[f]) + "\"";
            }
            j += "}";
        }
    }
    j += "]";
    return j;
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

/* The whole web UI is one gzipped single-page app (esp32/web/, built into
 * web_page.h). Served from flash in ONE response with Content-Encoding:
 * gzip — no heap copy, no repeated HTML requests; the page then drives
 * everything through the JSON /at-node/cmd/* endpoints. */
static void handle_web_app(void)
{
    g_http.sendHeader("Access-Control-Allow-Origin", "*");
    g_http.sendHeader("Content-Encoding", "gzip");
    g_http.sendHeader("Cache-Control", "no-cache");
    g_http.send_P(200, "text/html", (PGM_P)WEB_PAGE_GZ, WEB_PAGE_GZ_LEN);
}

/* Pre-SPA page URLs: the single-page app replaces them all. */
static void handle_legacy_page(void)
{
    g_http.sendHeader("Location", "/");
    g_http.send(302, "text/plain", "");
}


static void handle_help_json(void)
{
    send_json("{\"ok\":true,\"services\":" + build_services_json() + "}");
}

/* --- BLE status / pairing ---------------------------------------------- */
static String build_ble_status_json(void)
{
    String json = "{";
    json += "\"name\":\"" + g_device_name + "\"";
    json += ",\"addr\":\"" + String(g_ble_addr_str) + "\"";
    json += ",\"connected\":" + String(is_connected() ? "true" : "false");
    json += ",\"advertising\":" + String(NimBLEDevice::getAdvertising()->isAdvertising() ? "true" : "false");
    json += ",\"pairing_mode\":" + String(g_pairing_mode ? "true" : "false");
    if (g_pairing_mode && !is_connected() && g_pair_timeout_at) {
        int32_t remaining = (int32_t)(g_pair_timeout_at - millis());
        if (remaining < 0) remaining = 0;
        json += ",\"pair_timeout_ms\":" + String(remaining);
    }
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

/* --- API catalog (single source of truth for MQTT info / help.json) ---- */
struct ApiParam { const char* name; const char* desc; };
struct ApiEntry { const char* method; const ApiParam* params; uint8_t n; const char* desc; };

static const ApiParam P_TAP[] = {
    {"mods", "modifier mask (0x01=Ctrl 0x02=Shift 0x04=Alt 0x08=GUI 0x10=LCtrl)"},
    {"k",    "HID keycode (4=a 5=b ... 0x39=CapsLock)"},
    {"ms",   "hold duration ms, default 100"},
};
static const ApiParam P_TEXT[] = {
    {"s",   "ASCII text to type"},
    {"ms",  "per-key hold ms, default 60"},
    {"gap", "inter-key gap ms, default 80"},
};
static const ApiParam P_KEY[] = {
    {"mods",   "modifier mask"},
    {"k0..k5", "up to 6 simultaneous HID keycodes (0=none)"},
};
static const ApiParam P_GW[] = {
    {"pin",   "GPIO number (0-10, 18, 19, 20, 21)"},
    {"level", "0=LOW 1=HIGH"},
};
static const ApiParam P_GR[] = {
    {"pin", "GPIO number (input pullup)"},
};
static const ApiParam P_ADC[] = {
    {"ch", "ADC channel (0=GPIO0 1=GPIO1 2=GPIO2 3=GPIO3 4=GPIO4)"},
};
static const ApiParam P_I2CR[] = {
    {"addr", "I2C device address (hex ok)"},
    {"reg",  "register address"},
    {"len",  "bytes to read, default 1"},
};
static const ApiParam P_I2CW[] = {
    {"addr", "I2C device address (hex ok)"},
    {"reg",  "register address"},
    {"data", "hex bytes to write (e.g. FF01)"},
};
static const ApiParam P_IR[] = {
    {"protocol", "NEC | SIRC | RAW"},
    {"data",     "NEC/SIRC: hex code; RAW: comma-separated us timings"},
    {"bits",     "NEC/SIRC bit count (default 32/12)"},
};
static const ApiParam P_PAIR[] = {
    {"enable", "1=start 60s public advertising, 0=stop"},
};
static const ApiParam P_BD[] = {
    {"idx", "bond index (from ble/status list)"},
};
static const ApiParam P_WOL[] = {
    {"mac", "target MAC address (AA:BB:CC:DD:EE:FF)"},
};
static const ApiParam P_PING[] = {
    {"host",  "hostname or IP to ping"},
    {"count", "echo count, default 4"},
};
static const ApiParam P_TUNCFG[] = {
    {"id",      "tunnel id (always 1)"},
    {"server",  "rathole server host:port"},
    {"token",   "service token"},
    {"service", "service name (must match server)"},
    {"local",   "local host:port to forward to"},
    {"auto",    "1=connect at boot"},
    {"retry",   "reconnect backoff base, seconds 1-60"},
};
static const ApiParam P_TUNID[] = {
    {"id", "tunnel id (always 1)"},
};
static const ApiParam P_TUNEN[] = {
    {"enable", "1|0 rathole master switch (NVS)"},
};
static const ApiParam P_CFGSET[] = {
    {"key", "config key (see config/list)"},
    {"val", "value"},
};
static const ApiParam P_CFGGET[] = {
    {"key", "config key"},
};

static const ApiEntry API_CATALOG[] = {
    {"keyboard/tap",      P_TAP,  3, "press+release one key"},
    {"keyboard/text",     P_TEXT, 3, "type ASCII string via BLE"},
    {"keyboard/key",      P_KEY,  2, "raw 6KRO report (hold until released)"},
    {"gpio/write",        P_GW,   2, "set GPIO output level"},
    {"gpio/read",         P_GR,   1, "read GPIO input (pullup)"},
    {"adc/read",          P_ADC,  1, "read ADC millivolts"},
    {"i2c/scan",          NULL,   0, "scan I2C bus for devices"},
    {"i2c/read",          P_I2CR, 3, "read I2C register"},
    {"i2c/write",         P_I2CW, 3, "write I2C register"},
    {"ir/send",           P_IR,   3, "send IR via RMT (NEC/SIRC/RAW)"},
    {"ble/status",        NULL,   0, "BLE name, addr, connections, bonds"},
    {"ble/pair",          P_PAIR, 1, "start/stop public pairing mode"},
    {"ble/bonds/delete",  P_BD,   1, "delete one bonded host by index"},
    {"ble/bonds/clear",   NULL,   0, "delete all bonded hosts"},
    {"net/wol",           P_WOL,   1, "send Wake-on-LAN magic packet"},
    {"net/ping",          P_PING,  2, "ICMP ping from device LAN"},
    {"tunnel/status",     NULL,    0, "rathole tunnel states"},
    {"tunnel/config",     P_TUNCFG,7, "configure rathole tunnel (NVS)"},
    {"tunnel/connect",    P_TUNID, 1, "start tunnel control channel"},
    {"tunnel/disconnect", P_TUNID, 1, "stop tunnel"},
    {"tunnel/clear",      P_TUNID, 1, "wipe tunnel config (NVS)"},
    {"tunnel/enable",     P_TUNEN, 1, "rathole master switch (NVS)"},
    {"config/set",        P_CFGSET,2, "unified config: set key=val (NVS)"},
    {"config/get",        P_CFGGET,1, "unified config: read key"},
    {"config/list",       NULL,    0, "unified config: list all keys"},
    {"sys/info",          NULL,    0, "device manifest + this API catalog"},
};
#define API_COUNT (sizeof(API_CATALOG) / sizeof(API_CATALOG[0]))

/* Build the "services" JSON object from API_CATALOG. */
static String build_services_json(void)
{
    String s = "{";
    for (uint8_t i = 0; i < API_COUNT; i++) {
        const ApiEntry& e = API_CATALOG[i];
        if (i > 0) s += ",";
        s += "\"";
        s += e.method;
        s += "\":{\"d\":\"";
        s += e.desc;
        s += "\",\"p\":{";
        for (uint8_t j = 0; j < e.n; j++) {
            if (j > 0) s += ",";
            s += "\"";
            s += e.params[j].name;
            s += "\":\"";
            s += e.params[j].desc;
            s += "\"";
        }
        s += "}}";
    }
    s += "}";
    return s;
}

/* System info manifest - also published to MQTT atnode/<id>/info.
 * The "services" object is the full remote API catalog.              */
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
    json += ",\"services\":" + build_services_json();
    json += "}";
    return json;
}

static void handle_ble_pair(void)
{
    String val = g_http.arg("enable");
    if (val.length() == 0) val = g_http.arg("start");
    if (val == "1" || val == "true") {
        set_ble_adv_enabled(true);
    } else if (val == "0" || val == "false") {
        set_ble_adv_enabled(false);
    }
    String json = "{\"ok\":true,\"cmd\":\"ble/pair\"";
    json += ",\"advertising\":" + String(NimBLEDevice::getAdvertising()->isAdvertising() ? "true" : "false");
    json += ",\"pairing_mode\":" + String(g_pairing_mode ? "true" : "false");
    json += "}";
    send_json(json);
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
    json += ",\"http_enabled\":" + String(g_http_enabled ? "true" : "false");
    json += "}";
    send_json(json);
}

/* --- rathole tunnel handlers -------------------------------------------- */
/* Tunnel id from query/form arg "id" (1-based); -1 on invalid. */
static int tunnel_id_arg(void)
{
    int id = g_http.arg("id").toInt();
    if (id < 1 || id > RATHOLE_MAX_TUNNELS) return -1;
    return id - 1;
}

static void handle_tunnel_status(void)
{
    String json = "{\"ok\":true,\"tunnels\":[";
    for (int i = 0; i < RATHOLE_MAX_TUNNELS; i++) {
        if (i) json += ",";
        json += rathole_status_json(i);
    }
    json += "]}";
    send_json(json);
}

static void handle_tunnel_config(void)
{
    int idx = tunnel_id_arg();
    if (idx < 0) {
        send_json("{\"ok\":false,\"error\":\"invalid id\"}", 400);
        return;
    }
    static const char* keys[] = {"server", "token", "service", "local", "retry"};
    for (uint8_t i = 0; i < 5; i++) {
        String v = g_http.arg(keys[i]);
        if (v.length() > 0) rathole_set(idx, keys[i], v);
    }
    if (g_http.hasArg("auto")) rathole_set(idx, "auto", g_http.arg("auto"));
    if (g_http.hasArg("enable")) rathole_set(idx, "enable", g_http.arg("enable"));
    send_json("{\"ok\":true,\"cmd\":\"tunnel/config\",\"tunnel\":" +
              rathole_status_json(idx) + "}");
}

/* Global rathole master switch: POST /at-node/cmd/tunnel/enable?enable=1|0 */
static void handle_tunnel_enable(void)
{
    String val = g_http.arg("enable");
    if (val.length() == 0) val = g_http.arg("plain");
    val.trim();
    if (val == "1" || val == "true") {
        rathole_set_enabled(true);
    } else if (val == "0" || val == "false") {
        rathole_set_enabled(false);
    }
    send_json("{\"ok\":true,\"cmd\":\"tunnel/enable\",\"enabled\":" +
              String(rathole_is_enabled() ? "true" : "false") + "}");
}

static void handle_tunnel_connect(void)
{
    int idx = tunnel_id_arg();
    if (idx < 0) {
        send_json("{\"ok\":false,\"error\":\"invalid id\"}", 400);
        return;
    }
    bool ok = rathole_start(idx);
    send_json(String("{\"ok\":") + (ok ? "true" : "false") +
              ",\"cmd\":\"tunnel/connect\",\"tunnel\":" + rathole_status_json(idx) + "}",
              ok ? 200 : 400);
}

static void handle_tunnel_disconnect(void)
{
    int idx = tunnel_id_arg();
    if (idx < 0) {
        send_json("{\"ok\":false,\"error\":\"invalid id\"}", 400);
        return;
    }
    rathole_stop(idx);
    send_json("{\"ok\":true,\"cmd\":\"tunnel/disconnect\"}");
}

static void handle_tunnel_clear(void)
{
    int idx = tunnel_id_arg();
    if (idx < 0) {
        send_json("{\"ok\":false,\"error\":\"invalid id\"}", 400);
        return;
    }
    rathole_clear(idx);
    send_json("{\"ok\":true,\"cmd\":\"tunnel/clear\"}");
}


/* --- MQTT browser config page -------------------------------------------- */

static void handle_mqtt_clear(void)
{
    mqtt_clear_config();
    send_json("{\"ok\":true,\"cmd\":\"mqtt/clear\"}");
}

/* --- unified config endpoints (thin HTTP wrapper over config_set/get) --- */
static void handle_config_get(void)
{
    String key = g_http.arg("key");
    if (!config_known(key)) {
        send_json("{\"ok\":false,\"error\":\"unknown key\"}", 400);
        return;
    }
    send_json("{\"ok\":true,\"key\":\"" + key + "\",\"value\":\"" + config_get(key) + "\"}");
}

static void handle_config_set(void)
{
    if (!g_http.hasArg("key")) {
        send_json("{\"ok\":false,\"error\":\"missing key\"}", 400);
        return;
    }
    String key = g_http.arg("key");
    String val = g_http.hasArg("val") ? g_http.arg("val") : g_http.arg("value");
    if (!config_set(key, val)) {
        send_json("{\"ok\":false,\"error\":\"unknown key or invalid value\"}", 400);
        return;
    }
    send_json("{\"ok\":true,\"cmd\":\"config\",\"key\":\"" + key + "\"}");
}

static void handle_config_list(void)
{
    send_json("{\"ok\":true,\"keys\":" + config_list_json() + "}");
}

/* forward declarations for IR functions defined later */
static bool ir_send_raw(const uint16_t* timings, size_t count);
static bool ir_send_nec(uint32_t data);
static bool ir_send_sirc(uint32_t data, int bits);

/* Write hex-encoded bytes to I2C (e.g. "FF01" -> 0xFF, 0x01). */
static void i2c_write_hex(const String& hexData)
{
    for (int i = 0; i < hexData.length(); i += 2) {
        uint8_t b = (uint8_t)strtoul(hexData.substring(i, i + 2).c_str(), NULL, 16);
        Wire.write(b);
    }
}

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
            if (key == "name") {
                g_device_name = val;
                save_config(key, val);
            } else if (key == "hostname") {
                g_hostname = val;
                save_config(key, val);
            } else if (key == "http_enable") {
                set_http_enabled(val.toInt() != 0);
            } else {
                save_config(key, val);
            }
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
            i2c_write_hex(hexData);
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
                g_mqtt_connect_pending = true;
                if (g_mqtt_sem) xSemaphoreGive(g_mqtt_sem);
                resp = "OK";
            } else if (sub == "clear") {
                mqtt_clear_config();
                resp = "OK";
            } else if (sub == "status") {
                resp = "+MQTT:" + String(g_mqtt_connected ? "connected" : "disconnected") +
                                     ",auto=" + String(g_mqtt_auto ? "1" : "0");
            } else if (sub == "auto") {
                g_mqtt_auto = (val == "1");
                save_config("mqtt_auto", g_mqtt_auto ? "1" : "0");
                resp = "OK";
            } else if (sub == "ca") {
                /* val = SHA256 fingerprint or "status" */
                if (val == "status") {
                    if (g_mqtt_ca_fp.length() > 0) {
                        resp = "+MQTT_CA:" + g_mqtt_ca_fp;
                    } else {
                        resp = "+MQTT_CA:none";
                    }
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
    } else if (cmd.startsWith("AT+HTTP=") || cmd == "AT+HTTP") {
        String args = (cmd.length() > 8) ? cmd.substring(8) : String("status");
        if (args == "status") {
            resp = "+HTTP:" + String(g_http_enabled ? "enabled" : "disabled");
        } else if (args == "clear") {
            if (!g_http_enabled) set_http_enabled(true, false);
            prefs.begin("atnode", false);
            prefs.remove("http_enable");
            prefs.end();
            resp = "OK";
        } else if (args.startsWith("enable,")) {
            int val = args.substring(7).toInt();
            set_http_enabled(val != 0);
            resp = "OK";
        } else if (args == "1" || args == "0") {
            set_http_enabled(args == "1");
            resp = "OK";
        } else {
            resp = "ERROR";
        }
    } else if (cmd.startsWith("AT+PAIR=") || cmd == "AT+PAIR") {
        String args = (cmd.length() > 8) ? cmd.substring(8) : String("status");
        if (args == "status") {
            resp = "+PAIR:" + String(g_pairing_mode ? "enabled" : "disabled");
        } else if (args == "1" || args == "0") {
            set_ble_adv_enabled(args == "1");
            resp = "OK";
        } else {
            resp = "ERROR";
        }
    } else if (cmd.startsWith("AT+NVS=")) {
        String sub = cmd.substring(7);
        if (sub == "clear") {
            nvs_clear_config();
            schedule_restart(500);
            resp = "OK";
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
    i2c_write_hex(hexData);
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
/* TLS: setInsecure() + post-connect SHA256 fingerprint verification.
 * No embedded CA cert needed — fingerprint stored in NVS (mqtt_ca_fp). */

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
        g_mqtt_wifi_secure.setInsecure();  /* skip chain; fingerprint verified post-connect */
        g_mqtt_wifi_secure.setTimeout(15);   /* seconds (ESP32 Arduino) */
    } else {
        /* Plain TCP mode */
        g_mqtt.setClient(g_mqtt_wifi_plain);
        g_mqtt_wifi_plain.setTimeout(5);     /* seconds */
    }
    g_mqtt.setServer(g_mqtt_broker.c_str(), g_mqtt_port);
    g_mqtt.setCallback(mqtt_callback);
    g_mqtt.setBufferSize(4096);   /* sys/info with full API catalog ~3KB */

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
    static int alloc_fail_count = 0;
    if (ok && !g_mqtt_auto) {
        /* First successful connect: enable auto-reconnect for future boots */
        g_mqtt_auto = true;
        save_config("mqtt_auto", "1");
    }
    if (ok) {
        alloc_fail_count = 0;
        Serial.println("MQTT connect OK");
        return true;
    }
    {
        char ssl_err[128] = {0};
        if (g_mqtt_port == 8883) {
            g_mqtt_wifi_secure.lastError(ssl_err, sizeof(ssl_err));
        }
        Serial.printf("MQTT connect FAILED (state=%d heap=%u maxblk=%u ssl=%s)\n",
                      g_mqtt.state(), (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                      ssl_err);
        /* mbedTLS handshake needs large contiguous blocks; once the heap is
         * fragmented past that, every retry fails forever. A reboot restores
         * a clean heap, so escalate after repeated alloc failures.        */
        if (strstr(ssl_err, "Memory allocation") && ++alloc_fail_count >= 5) {
            Serial.println("MQTT TLS alloc failures: restarting to defragment heap");
            schedule_restart(1000);
        }
        return false;
    }
}

static void mqtt_task_func(void* arg)
{
    /* All PubSubClient operations run here (single owner, thread-safe).
     * Semaphore: given by mqtt_poll() on manual connect request;
     * 1s timeout tick handles auto-reconnect + g_mqtt.loop(). */
    uint32_t last_attempt = 0;
    for (;;) {
        xSemaphoreTake(g_mqtt_sem, 1000 / portTICK_PERIOD_MS);

        bool want = (g_mqtt_broker.length() > 0) && g_wifi_ready;
        if (want && !g_mqtt_connected) {
            /* Auto-reconnect only when mqtt_auto enabled (set after first
             * successful connect); manual pending always tries. */
            bool allowed = g_mqtt_connect_pending || g_mqtt_auto;
            if (allowed) {
                uint32_t now = millis();
                uint32_t interval = g_mqtt_connect_pending ? 1000 : 10000;
                if (now - last_attempt > interval) {
                    last_attempt = now;
                    g_mqtt_connect_pending = false;
                    mqtt_connect();
                }
            }
        }
        if (g_mqtt_connected) {
            if (!g_mqtt.loop()) {
                g_mqtt_connected = false;
            }
        }
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
    if (method == "ble/pair") {
        String val = query_get(query, "enable");
        if (val.length() == 0) val = query_get(query, "start");
        if (val == "1" || val == "true") {
            set_ble_adv_enabled(true);
            return "\"ok\":true,\"advertising\":" + String(NimBLEDevice::getAdvertising()->isAdvertising() ? "true" : "false") +
                   ",\"pairing_mode\":true";
        }
        if (val == "0" || val == "false") {
            set_ble_adv_enabled(false);
            return "\"ok\":true,\"advertising\":false,\"pairing_mode\":false";
        }
        return err("missing enable");
    }
    if (method == "sys/info") {
        return String("\"ok\":true,\"info\":") + build_sys_info_json();
    }
    if (method == "tunnel/status") {
        String t = "\"ok\":true,\"tunnels\":[";
        for (int i = 0; i < RATHOLE_MAX_TUNNELS; i++) {
            if (i) t += ",";
            t += rathole_status_json(i);
        }
        return t + "]";
    }
    if (method == "tunnel/enable") {
        String val = query_get(query, "enable");
        if (val == "1" || val == "true")      rathole_set_enabled(true);
        else if (val == "0" || val == "false") rathole_set_enabled(false);
        else return err("missing enable");
        return String("\"ok\":true,\"enabled\":") + (rathole_is_enabled() ? "true" : "false");
    }
    if (method.startsWith("tunnel/")) {
        int id = query_get(query, "id").toInt();
        if (id < 1 || id > RATHOLE_MAX_TUNNELS) return err("invalid id");
        int idx = id - 1;
        if (method == "tunnel/connect") {
            return rathole_start(idx) ? "\"ok\":true" : err("start failed");
        }
        if (method == "tunnel/disconnect") {
            rathole_stop(idx);
            return "\"ok\":true";
        }
        if (method == "tunnel/clear") {
            rathole_clear(idx);
            return "\"ok\":true";
        }
        if (method == "tunnel/config") {
            static const char* keys[] = {"server", "token", "service", "local", "retry"};
            for (uint8_t i = 0; i < 5; i++) {
                String v = query_get(query, keys[i]);
                if (v.length() > 0) rathole_set(idx, keys[i], v);
            }
            String a = query_get(query, "auto");
            if (a.length() > 0) rathole_set(idx, "auto", a);
            String e = query_get(query, "enable");
            if (e.length() > 0) rathole_set(idx, "enable", e);
            return String("\"ok\":true,\"tunnel\":") + rathole_status_json(idx);
        }
        return err("unknown tunnel method");
    }
    if (method == "config/set") {
        String key = query_get(query, "key");
        String val = query_get(query, "val");
        if (key.length() == 0) return err("missing key");
        return config_set(key, val) ? "\"ok\":true" : err("unknown key or invalid value");
    }
    if (method == "config/get") {
        String key = query_get(query, "key");
        if (!config_known(key)) return err("unknown key");
        return String("\"ok\":true,\"value\":\"") + config_get(key) + "\"";
    }
    if (method == "config/list") {
        return String("\"ok\":true,\"keys\":") + config_list_json();
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
    json += ",\"ca_fp\":\"";
    json += g_mqtt_ca_fp;
    json += "\"";
    json += ",\"auto\":";
    json += g_mqtt_auto ? "true" : "false";
    json += "}";
    send_json(json);
}

static void handle_mqtt_config(void)
{
    /* legacy alias over the unified config layer */
    if (g_http.hasArg("broker")) config_set("mqtt.broker", g_http.arg("broker"));
    if (g_http.hasArg("port"))   config_set("mqtt.port",   g_http.arg("port"));
    if (g_http.hasArg("user"))   config_set("mqtt.user",   g_http.arg("user"));
    if (g_http.hasArg("pass"))   config_set("mqtt.pass",   g_http.arg("pass"));
    if (g_http.hasArg("auto"))   config_set("mqtt.auto",   g_http.arg("auto"));
    send_json("{\"ok\":true,\"cmd\":\"mqtt/config\"}");
}

static void handle_mqtt_ca(void)
{
    String fp = g_http.arg("fp");
    if (fp.length() > 0) config_set("mqtt.ca", fp);
    send_json("{\"ok\":true,\"cmd\":\"mqtt/ca\"}");
}

static void handle_wifi_config(void)
{
    /* legacy alias over the unified config layer */
    if (g_http.hasArg("ssid")) config_set("wifi.ssid", g_http.arg("ssid"));
    if (g_http.hasArg("pass")) config_set("wifi.pass", g_http.arg("pass"));
    send_json("{\"ok\":true,\"cmd\":\"wifi/config\",\"ssid\":\"" + g_wifi_ssid + "\"}");
}

static void mqtt_poll(void)
{
    /* Wake MQTT task on manual connect request (non-blocking). */
    if (g_mqtt_connect_pending && g_mqtt_sem) {
        xSemaphoreGive(g_mqtt_sem);
    }
}

static void handle_mqtt_connect(void)
{
    g_mqtt_connect_pending = true;
    if (g_mqtt_sem) xSemaphoreGive(g_mqtt_sem);
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

static void handle_http_config(void)
{
    String val = g_http.arg("enable");
    if (val.length() == 0) val = g_http.arg("plain");
    val.trim();
    if (val == "1" || val == "true" || val == "0" || val == "false") {
        config_set("http.enable", val);
        send_json("{\"ok\":true,\"cmd\":\"http/config\",\"enabled\":" +
                  String(g_http_enabled ? "true" : "false") + "}");
    } else {
        send_json("{\"ok\":false,\"error\":\"expected enable=1|0\"}", 400);
    }
}

static void handle_http_status(void)
{
    String json = "{\"ok\":true,\"cmd\":\"http/status\"";
    json += ",\"enabled\":" + String(g_http_enabled ? "true" : "false");
    json += "}";
    send_json(json);
}

static void handle_http_clear(void)
{
    if (!g_http_enabled) set_http_enabled(true, false);
    prefs.begin("atnode", false);
    prefs.remove("http_enable");
    prefs.end();
    send_json("{\"ok\":true,\"cmd\":\"http/clear\"}");
}

static void handle_nvs_clear(void)
{
    nvs_clear_config();
    schedule_restart(500);
    send_json("{\"ok\":true,\"cmd\":\"nvs/clear\",\"restarting\":true}");
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

    /* Never auto-advertise on disconnect; we manage public/directed modes ourselves. */
    g_server->advertiseOnDisconnect(false);

    /* On boot, if we have a bonded host, advertise privately (directed) to it.
     * This lets the bonded host reconnect without making us publicly discoverable. */
    int nb = NimBLEDevice::getNumBonds();
    if (nb > 0) {
        NimBLEAddress addr = NimBLEDevice::getBondedAddress(nb - 1);
        adv->start(0, &addr);
        Serial.printf("BLE directed advertising started to bonded peer %s\n", addr.toString().c_str());
    }

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
        Serial.println("  AT+SET=<key>=<val> / AT+GET=<key> / AT+KEYS  unified config");
        Serial.println("  AT+CONF=<key>=<val>          name/hostname (NVS, legacy alias)");
        Serial.println("  AT+BT_LIST / AT+BT_DISC / AT+BT_PAIR");
        Serial.println("  AT+GPIO_W=<pin>,<level> / AT+GPIO_R=<pin>");
        Serial.println("  AT+ADC=<ch> / AT+I2C_SCAN / AT+I2C_R / AT+I2C_W");
        Serial.println("  AT+IR=<NEC|SIRC|RAW>,...");
        Serial.println("  AT+WIFI=ssid|pass|status,<val>");
        Serial.println("  AT+MQTT=broker|port,<val> connect|status|clear");
        Serial.println("  AT+TUNNEL=enable,<0|1>            rathole master switch (NVS)");
        Serial.println("  AT+TUNNEL=<1>,server|token|service|local|auto|retry|enable,<val>");
        Serial.println("  AT+TUNNEL=<1>,connect|disconnect|clear|status");
        Serial.println("  AT+AP=<1|0>                  provisioning AP");
        Serial.println("  AT+HTTP=<1|0|status|clear>   HTTP server control (NVS)");
        Serial.println("  AT+HTTP=enable,<1|0>         enable/disable HTTP server");
        Serial.println("  AT+PAIR=<1|0|status>       BLE public pairing advertising (60s timeout)");
        Serial.println("  AT+NVS=clear                 erase all NVS settings and restart");
        Serial.println("  Full API catalog: GET /at-node/help.json or MQTT sys/info");
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
    } else if (line.startsWith("AT+SET=")) {
        /* AT+SET=<config-key>=<value> — unified config layer */
        String kv = line.substring(7);
        int eq = kv.indexOf('=');
        if (eq > 0 && config_set(kv.substring(0, eq), kv.substring(eq + 1))) {
            Serial.println("OK");
        } else {
            Serial.println("ERROR");
        }
    } else if (line.startsWith("AT+GET=")) {
        String key = line.substring(7);
        if (config_known(key)) {
            Serial.println("+GET:" + key + "=" + config_get(key));
            Serial.println("OK");
        } else {
            Serial.println("ERROR");
        }
    } else if (line == "AT+KEYS") {
        Serial.println("+KEYS:" + config_list_json());
        Serial.println("OK");
    } else if (line.startsWith("AT+CONF=")) {
        /* legacy alias: name -> device.name, hostname -> device.hostname,
         * http_enable -> http.enable; anything else falls through to a raw
         * NVS write in the atnode namespace.                            */
        String kv = line.substring(8);
        int eq = kv.indexOf('=');
        if (eq > 0) {
            String key = kv.substring(0, eq);
            String val = kv.substring(eq + 1);
            if (key == "name") {
                config_set("device.name", val);
            } else if (key == "hostname") {
                config_set("device.hostname", val);
            } else if (key == "http_enable") {
                config_set("http.enable", val);
            } else {
                save_config(key, val);
            }
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
            i2c_write_hex(hexData);
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
                Serial.println(config_set("mqtt.broker", val) ? "OK" : "ERROR");
            } else if (sub == "clear") {
                mqtt_clear_config();
                Serial.println("OK");
            } else if (sub == "port") {
                Serial.println(config_set("mqtt.port", val) ? "OK" : "ERROR");
            } else if (sub == "connect") {
                g_mqtt_connect_pending = true;
                if (g_mqtt_sem) xSemaphoreGive(g_mqtt_sem);
                Serial.println("OK");
            } else if (sub == "status") {
                Serial.print("+MQTT:");
                Serial.print(g_mqtt_connected ? "connected" : "disconnected");
                Serial.print(",auto=");
                Serial.println(g_mqtt_auto ? "1" : "0");
                Serial.println("OK");
            } else if (sub == "auto") {
                Serial.println(config_set("mqtt.auto", val) ? "OK" : "ERROR");
            } else if (sub == "ca") {
                /* val = SHA256 fingerprint or "status" */
                if (val == "status") {
                    if (g_mqtt_ca_fp.length() > 0) {
                        Serial.println("+MQTT_CA:" + g_mqtt_ca_fp);
                    } else {
                        Serial.println("+MQTT_CA:none");
                    }
                } else {
                    Serial.println(config_set("mqtt.ca", val) ? "OK" : "ERROR");
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
                Serial.println(config_set("wifi.ssid", val) ? "OK" : "ERROR");
            } else if (sub == "pass") {
                Serial.println(config_set("wifi.pass", val) ? "OK" : "ERROR");
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
    } else if (line.startsWith("AT+HTTP=") || line == "AT+HTTP") {
        String args = (line.length() > 8) ? line.substring(8) : String("status");
        if (args == "status") {
            Serial.println("+HTTP:" + String(g_http_enabled ? "enabled" : "disabled"));
            Serial.println("OK");
        } else if (args == "clear") {
            if (!g_http_enabled) set_http_enabled(true, false);
            prefs.begin("atnode", false);
            prefs.remove("http_enable");
            prefs.end();
            Serial.println("OK");
        } else if (args.startsWith("enable,")) {
            config_set("http.enable", args.substring(7));
            Serial.println("OK");
        } else if (args == "1" || args == "0") {
            config_set("http.enable", args);
            Serial.println("OK");
        } else {
            Serial.println("ERROR");
        }
    } else if (line.startsWith("AT+PAIR=") || line == "AT+PAIR") {
        String args = (line.length() > 8) ? line.substring(8) : String("status");
        if (args == "status") {
            Serial.println("+PAIR:" + String(g_pairing_mode ? "enabled" : "disabled"));
            Serial.println("OK");
        } else if (args == "1" || args == "0") {
            set_ble_adv_enabled(args == "1");
            Serial.println("OK");
        } else {
            Serial.println("ERROR");
        }
    } else if (line.startsWith("AT+NVS=")) {
        String sub = line.substring(7);
        if (sub == "clear") {
            nvs_clear_config();
            schedule_restart(500);
            Serial.println("OK");
        } else {
            Serial.println("ERROR");
        }
    } else if (line.startsWith("AT+TUNNEL=")) {
        /* AT+TUNNEL=status | enable | enable,<0|1>
         * AT+TUNNEL=<1>,server|token|service|local|auto|retry|enable,<val>
         * AT+TUNNEL=<1>,connect|disconnect|clear|status          */
        String args = line.substring(10);
        int c1 = args.indexOf(',');
        if (args == "status") {
            Serial.println("+TUNNEL_EN:" + String(rathole_is_enabled() ? 1 : 0));
            for (int i = 0; i < RATHOLE_MAX_TUNNELS; i++) {
                Serial.println("+TUNNEL:" + rathole_status_json(i));
            }
            Serial.println("OK");
        } else if (args == "enable") {
            Serial.println("+TUNNEL_EN:" + String(rathole_is_enabled() ? 1 : 0));
            Serial.println("OK");
        } else if (args.startsWith("enable,")) {
            rathole_set_enabled(args.substring(7).toInt() != 0);
            Serial.println("OK");
        } else if (c1 > 0) {
            int id = args.substring(0, c1).toInt();
            String rest = args.substring(c1 + 1);
            int c2 = rest.indexOf(',');
            String sub = (c2 < 0) ? rest : rest.substring(0, c2);
            String val = (c2 < 0) ? "" : rest.substring(c2 + 1);
            if (id < 1 || id > RATHOLE_MAX_TUNNELS) {
                Serial.println("ERROR");
            } else if (sub == "server" || sub == "token" || sub == "service" ||
                       sub == "local" || sub == "auto" || sub == "retry" ||
                       sub == "enable") {
                Serial.println((val.length() > 0 && rathole_set(id - 1, sub, val))
                               ? "OK" : "ERROR");
            } else if (sub == "connect") {
                Serial.println(rathole_start(id - 1) ? "OK" : "ERROR");
            } else if (sub == "disconnect") {
                rathole_stop(id - 1);
                Serial.println("OK");
            } else if (sub == "clear") {
                rathole_clear(id - 1);
                Serial.println("OK");
            } else if (sub == "status") {
                Serial.println("+TUNNEL:" + rathole_status_json(id - 1));
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
    /* Modem sleep causes 100ms+ LAN latency spikes and multi-second TCP
     * stalls (dropped beacons under multiple open sockets) — fatal to the
     * tunnel heartbeat margin. This device is USB-powered; keep RF awake. */
    WiFi.setSleep(false);
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

        g_http.on("/", HTTP_GET, handle_web_app);
        g_http.on("/at-node/status", HTTP_GET, handle_legacy_page);
        g_http.on("/at-node/help", HTTP_GET, handle_legacy_page);
        g_http.on("/at-node/pair", HTTP_GET, handle_legacy_page);
        g_http.on("/at-node/tunnel", HTTP_GET, handle_legacy_page);
        g_http.on("/at-node/mqtt", HTTP_GET, handle_legacy_page);
        g_http.on("/at-node/cmd/status", HTTP_GET, handle_cmd_status);
        g_http.on("/at-node/help.json", HTTP_GET, handle_help_json);
        g_http.on("/at-node/at", HTTP_POST, handle_at);
        g_http.on("/at-node/cmd/keyboard/tap", HTTP_POST, handle_keyboard_tap);
        g_http.on("/at-node/cmd/keyboard/text", HTTP_POST, handle_keyboard_text);
        g_http.on("/at-node/cmd/keyboard/key", HTTP_POST, handle_keyboard_key);
        g_http.on("/at-node/cmd/ble/status", HTTP_GET, handle_ble_status);
        g_http.on("/at-node/cmd/ble/pair", HTTP_POST, handle_ble_pair);
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
        g_http.on("/at-node/cmd/mqtt/clear", HTTP_POST, handle_mqtt_clear);
        g_http.on("/at-node/cmd/config", HTTP_GET, handle_config_get);
        g_http.on("/at-node/cmd/config", HTTP_POST, handle_config_set);
        g_http.on("/at-node/cmd/config/list", HTTP_GET, handle_config_list);
        g_http.on("/at-node/cmd/tunnel/status", HTTP_GET, handle_tunnel_status);
        g_http.on("/at-node/cmd/tunnel/config", HTTP_POST, handle_tunnel_config);
        g_http.on("/at-node/cmd/tunnel/enable", HTTP_POST, handle_tunnel_enable);
        g_http.on("/at-node/cmd/tunnel/connect", HTTP_POST, handle_tunnel_connect);
        g_http.on("/at-node/cmd/tunnel/disconnect", HTTP_POST, handle_tunnel_disconnect);
        g_http.on("/at-node/cmd/tunnel/clear", HTTP_POST, handle_tunnel_clear);
        g_http.on("/at-node/cmd/wifi/config", HTTP_POST, handle_wifi_config);
        g_http.on("/at-node/cmd/ap", HTTP_POST, handle_ap);
        g_http.on("/at-node/cmd/net/wol", HTTP_POST, handle_net_wol);
        g_http.on("/at-node/cmd/net/ping", HTTP_POST, handle_net_ping);
        g_http.on("/at-node/cmd/http/config", HTTP_POST, handle_http_config);
        g_http.on("/at-node/cmd/http/status", HTTP_GET, handle_http_status);
        g_http.on("/at-node/cmd/http/clear", HTTP_POST, handle_http_clear);
        g_http.on("/at-node/cmd/nvs/clear", HTTP_POST, handle_nvs_clear);
        g_http.onNotFound(handle_not_found);
        if (g_http_enabled) {
            g_http.begin();
            Serial.println("HTTP server on port 80 (trusted local NAT only; AT+HTTP=0 to disable)");
        } else {
            Serial.println("HTTP server disabled");
        }
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

    /* MQTT background task — owns all PubSubClient operations */
    g_mqtt_sem = xSemaphoreCreateBinary();
    xTaskCreate(mqtt_task_func, "mqtt", 4096, NULL, 1, &g_mqtt_task);

    /* rathole tunnels with auto=1 (tasks wait for WiFi themselves) */
    rathole_init();
}

void loop(void)
{
    if (g_wifi_ready && g_http_enabled) g_http.handleClient();
    handle_serial();
    type_poll();
    mqtt_poll();
    ap_portal_poll();
    ble_adv_poll();
    if (g_adv_restart_at && (int32_t)(millis() - g_adv_restart_at) >= 0) {
        g_adv_restart_at = 0;
        if (!is_connected()) {
            NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
            adv->stop();   /* clear stale state */
            if (g_directed_adv_pending) {
                g_directed_adv_pending = false;
                adv->start(0, &g_directed_adv_addr);
                Serial.println("BLE directed advertising restarted");
            } else if (g_pairing_mode) {
                adv->start();
                Serial.println("BLE public advertising restarted");
            }
        }
    }
    if (g_restart_at && (int32_t)(millis() - g_restart_at) >= 0) {
        g_restart_at = 0;
        Serial.println("restarting...");
        ESP.restart();
    }
    delay(2);
}
