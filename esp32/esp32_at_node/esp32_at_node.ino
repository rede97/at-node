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
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEHIDDevice.h>
#include <NimBLECharacteristic.h>
#include "wifi_config.h"

/* --- device configuration --------------------------------------------- */
#define DEFAULT_DEVICE_NAME "AT-Node-ESP"
#define DEFAULT_HOSTNAME    "atnodeesp"

static Preferences prefs;
static String g_device_name;
static String g_hostname;

/* --- BLE globals ------------------------------------------------------ */
static NimBLEServer*        g_server        = nullptr;
static NimBLEHIDDevice*     g_hid           = nullptr;
static NimBLECharacteristic* g_bootInput   = nullptr;
static NimBLECharacteristic* g_bootOutput   = nullptr;
static NimBLECharacteristic* g_protocolMode = nullptr;

static struct {
    uint8_t mods;
    uint8_t keys[6];
} g_key_state;

/* --- HTTP globals ----------------------------------------------------- */
static WebServer g_http(80);
static bool      g_wifi_ready = false;

/* --- typing queue (non-blocking) -------------------------------------- */
static String   g_type_text;
static size_t   g_type_idx  = 0;
static int      g_type_ms   = 40;
static int      g_type_gap  = 30;
static uint32_t g_type_next = 0;
static bool     g_type_busy = false;

/* --- standard boot keyboard report map -------------------------------- */
static const uint8_t REPORT_MAP[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0
};

/* --- modifier table ---------------------------------------------------- */
static const uint8_t MOD_KEYS[8] = {
    0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7
};

/* --- BLE callbacks ---------------------------------------------------- */
class AtNodeServerCallbacks : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        Serial.println("BLE connected");
    }
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo,
                       int reason) override {
        Serial.println("BLE disconnected");
        NimBLEDevice::startAdvertising();
    }
};

/* --- helpers ----------------------------------------------------------- */
static uint8_t parse_uint8(const String& s)
{
    if (s.length() == 0) return 0;
    return (uint8_t)strtoul(s.c_str(), NULL, 0);
}

static void send_report(void)
{
    uint8_t report[8];
    report[0] = g_key_state.mods;
    report[1] = 0;
    for (int i = 0; i < 6; i++) report[i + 2] = g_key_state.keys[i];
    if (g_bootInput) g_bootInput->setValue(report, sizeof(report));
    if (g_bootInput) g_bootInput->notify();
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

    uint8_t old_mods = g_key_state.mods;
    g_key_state.mods |= mods;

    clear_keys();
    key_press(k);
    delay(ms);
    clear_keys();

    g_key_state.mods = old_mods;
    if (g_key_state.mods) send_report();
}

static bool is_connected(void)
{
    return g_server && g_server->getConnectedCount() > 0;
}

/* --- configuration ----------------------------------------------------- */
static void load_config(void)
{
    prefs.begin("atnode", false);
    g_device_name = prefs.getString("name", DEFAULT_DEVICE_NAME);
    g_hostname    = prefs.getString("hostname", DEFAULT_HOSTNAME);
    prefs.end();
}

static void save_config(const String& key, const String& value)
{
    prefs.begin("atnode", false);
    prefs.putString(key.c_str(), value);
    prefs.end();
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
    } else if (c >= '0' && c <= '9') {
        key_tap(0, 0x1E + (c - '0'), g_type_ms);
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

static void handle_status(void)
{
    String json = "{";
    json += "\"device\":\"" + g_device_name + "\"";
    json += ",\"hostname\":\"" + g_hostname + "\"";
    json += ",\"connected\":" + String(is_connected() ? "true" : "false");
    json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
    json += ",\"ble_addr\":\"" + String(NimBLEDevice::getAddress().toString().c_str()) + "\"";
    json += ",\"typing\":" + String(g_type_busy ? "true" : "false");
    json += "}";
    send_json(json);
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
        int ms  = args.substring(0, args.indexOf(',')).toInt();
        int mods = args.substring(args.indexOf(',') + 1).toInt();
        /* NOTE: full parsing omitted in skeleton; see at_parser.cpp later */
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
    } else {
        resp = "ERROR: unknown command";
    }

    String json = "{\"ok\":";
    json += (resp.indexOf("OK") == 0) ? "true" : "false";
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

static void handle_not_found(void)
{
    send_json("{\"ok\":false,\"error\":\"not found\"}", 404);
}

/* --- BLE init ---------------------------------------------------------- */
static bool ble_init(void)
{
    NimBLEDevice::init(g_device_name.c_str());
    NimBLEDevice::setSecurityAuth(true, false, false);

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(new AtNodeServerCallbacks());
    g_server->advertiseOnDisconnect(true);

    g_hid = new NimBLEHIDDevice(g_server);
    g_hid->setManufacturer("AT-Node");
    g_hid->setPnp(0x02, 0xE502, 0xA111, 0x0210);
    g_hid->setHidInfo(0x00, 0x01);
    g_hid->setReportMap(const_cast<uint8_t*>(REPORT_MAP), sizeof(REPORT_MAP));

    g_protocolMode = g_hid->getProtocolMode();
    g_protocolMode->setValue(0);

    NimBLEService* hidSvc = g_hid->getHidService();
    g_bootInput = hidSvc->createCharacteristic(
        (uint16_t)0x2A22,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    g_bootInput->setValue((uint8_t*)"\x00\x00\x00\x00\x00\x00\x00\x00", 8);

    g_bootOutput = g_hid->getBootOutput();
    g_bootOutput->setCallbacks(nullptr);

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
static void handle_serial(void)
{
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) return;

    if (line == "AT") {
        Serial.println("OK");
    } else if (line == "AT+STATUS") {
        Serial.print("role=esp32_at_node connected=");
        Serial.print(is_connected() ? "1" : "0");
        Serial.print(" ip=");
        Serial.println(WiFi.localIP().toString());
    } else if (line.startsWith("AT+TAP=")) {
        String args = line.substring(7);
        int ms  = args.substring(0, args.indexOf(',')).toInt();
        int mods = args.substring(args.indexOf(',') + 1).toInt();
        /* simplified: mods ignored, key from last arg */
        int key = args.substring(args.lastIndexOf(',') + 1).toInt();
        key_tap((uint8_t)mods, (uint8_t)key, ms);
        Serial.println("OK");
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
    } else {
        Serial.println("ERROR");
    }
}

/* --- setup / loop ------------------------------------------------------ */
void setup(void)
{
    Serial.begin(115200);
    delay(500);
    Serial.println("\r\nesp32_at_node start");

    load_config();

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

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

        g_http.on("/at-node/status", HTTP_GET, handle_status);
        g_http.on("/at-node/at", HTTP_POST, handle_at);
        g_http.on("/at-node/cmd/keyboard/tap", HTTP_POST, handle_keyboard_tap);
        g_http.on("/at-node/cmd/keyboard/text", HTTP_POST, handle_keyboard_text);
        g_http.on("/at-node/cmd/keyboard/key", HTTP_POST, handle_keyboard_key);
        g_http.onNotFound(handle_not_found);
        g_http.begin();
        Serial.println("HTTP server on port 80");
    } else {
        Serial.println("\r\nWiFi connection failed, HTTP disabled");
    }

    ble_init();
}

void loop(void)
{
    if (g_wifi_ready) g_http.handleClient();
    handle_serial();
    type_poll();
    delay(2);
}
