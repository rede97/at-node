/*
 * esp32c3_kbd.ino
 *
 * ESP32-C3 programmable BLE HID keyboard test bench.
 * Used as an alternative keyboard peripheral to validate the
 * CH582F dongle (DONGLE=1 or MODE=DUAL) over BLE.
 *
 * Features:
 *   - Minimal NimBLE HID keyboard (boot protocol, 8-byte reports).
 *     Exposes boot keyboard input/output reports and protocol mode so
 *     the CH582 dongle discovers it as a simple boot keyboard.
 *   - WiFi HTTP control plane on http://esp32kbd.local
 *   - USB serial fallback commands
 *
 * Endpoints:
 *   GET  /status          -> JSON with BLE connection state, IP and MAC
 *   POST /tap?k=<key>&mods=<mods>&ms=<ms>
 *                         -> press and release a single key
 *   POST /text?s=<text>   -> type a string
 *
 * Build/upload (Windows, arduino-cli):
 *   arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc .
 *   arduino-cli upload  --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc -p COMx .
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEHIDDevice.h>
#include <NimBLECharacteristic.h>
#include "wifi_config.h"

#define HOSTNAME      "esp32kbd"
#define DEVICE_NAME   "C3-Kbd"
#define MANUFACTURER  "AT-Node"

/* Standard boot keyboard report map (no Report ID prefix).
 * 8-byte input report: [modifiers, reserved, key0..key5]. */
static const uint8_t REPORT_MAP[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x06,       /* Usage (Keyboard) */
    0xA1, 0x01,       /* Collection (Application) */
    0x05, 0x07,       /*   Usage Page (Key Codes) */
    0x19, 0xE0,       /*   Usage Minimum (224) */
    0x29, 0xE7,       /*   Usage Maximum (231) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x08,       /*   Report Count (8) */
    0x81, 0x02,       /*   Input (Data, Variable, Absolute) */
    0x95, 0x01,       /*   Report Count (1) */
    0x75, 0x08,       /*   Report Size (8) */
    0x81, 0x01,       /*   Input (Constant) */
    0x95, 0x05,       /*   Report Count (5) */
    0x75, 0x01,       /*   Report Size (1) */
    0x05, 0x08,       /*   Usage Page (LEDs) */
    0x19, 0x01,       /*   Usage Minimum (1) */
    0x29, 0x05,       /*   Usage Maximum (5) */
    0x91, 0x02,       /*   Output (Data, Variable, Absolute) */
    0x95, 0x01,       /*   Report Count (1) */
    0x75, 0x03,       /*   Report Size (3) */
    0x91, 0x01,       /*   Output (Constant) */
    0x95, 0x06,       /*   Report Count (6) */
    0x75, 0x08,       /*   Report Size (8) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x65,       /*   Logical Maximum (101) */
    0x05, 0x07,       /*   Usage Page (Key Codes) */
    0x19, 0x00,       /*   Usage Minimum (0) */
    0x29, 0x65,       /*   Usage Maximum (101) */
    0x81, 0x00,       /*   Input (Data, Array) */
    0xC0              /* End Collection */
};

/* modifier bit -> USB HID modifier keycode */
static const uint8_t MOD_KEYS[8] = {
    0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7
};

/* --- BLE server callbacks --------------------------------------------- */
class C3ServerCallbacks : public NimBLEServerCallbacks {
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

/* --- globals ----------------------------------------------------------- */
static NimBLEServer*        g_server        = nullptr;
static NimBLEHIDDevice*     g_hid           = nullptr;
static NimBLECharacteristic* g_bootInput   = nullptr;
static NimBLECharacteristic* g_bootOutput    = nullptr;
static NimBLECharacteristic* g_protocolMode = nullptr;
static WebServer            g_server_http(80);
static bool                 g_wifi_ready    = false;

static struct {
    uint8_t mods;
    uint8_t keys[6];
} g_key_state;

static String  g_type_text;
static size_t  g_type_idx   = 0;
static int     g_type_ms    = 40;
static int     g_type_gap   = 30;
static uint32_t g_type_next  = 0;
static bool    g_type_busy  = false;

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

/* --- HTTP handlers ----------------------------------------------------- */
static void handle_status(void)
{
    String json = "{";
    json += "\"connected\":";
    json += is_connected() ? "true" : "false";
    json += ",\"ip\":\"";
    json += WiFi.localIP().toString();
    json += "\"";
    json += ",\"hostname\":\"" HOSTNAME "\"";
    json += ",\"ble_addr\":\"";
    json += NimBLEDevice::getAddress().toString().c_str();
    json += "\"";
    json += "}";
    g_server_http.send(200, "application/json", json);
}

static void handle_tap(void)
{
    if (!is_connected()) {
        g_server_http.send(409, "text/plain", "BLE not connected");
        return;
    }
    uint8_t mods = parse_uint8(g_server_http.arg("mods"));
    uint8_t key  = parse_uint8(g_server_http.arg("k"));
    int ms       = g_server_http.arg("ms").toInt();
    key_tap(mods, key, ms);
    g_server_http.send(200, "text/plain", "OK");
}

static void handle_text(void)
{
    if (!is_connected()) {
        g_server_http.send(409, "text/plain", "BLE not connected");
        return;
    }
    if (g_type_busy) {
        g_server_http.send(423, "text/plain", "typing in progress");
        return;
    }
    String text = g_server_http.arg("s");
    if (text.length() == 0) {
        g_server_http.send(400, "text/plain", "missing s=");
        return;
    }
    int ms  = g_server_http.arg("ms").toInt();
    int gap = g_server_http.arg("gap").toInt();
    g_type_ms  = (ms > 0) ? ms : 40;
    g_type_gap = (gap > 0) ? gap : 30;
    g_type_text = text;
    g_type_idx  = 0;
    g_type_next = 0;
    g_type_busy = true;
    g_server_http.send(200, "text/plain", "OK queued");
}

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
        key_tap(0, 0x28, g_type_ms); /* Return */
    } else {
        key_tap(0, c, g_type_ms);
    }
    g_type_idx++;
    g_type_next = now + g_type_ms + g_type_gap;
}

static void handle_not_found(void)
{
    g_server_http.send(404, "text/plain", "not found");
}

/* --- serial fallback --------------------------------------------------- */
static void handle_serial(void)
{
    if (!Serial.available()) return;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) return;

    if (line.startsWith("TAP ")) {
        String args = line.substring(4);
        args.trim();
        uint8_t key = (uint8_t)strtoul(args.c_str(), NULL, 0);
        key_tap(0, key, 100);
        Serial.println("OK TAP");
    } else if (line.startsWith("TEXT ")) {
        String text = line.substring(5);
        if (is_connected()) {
            for (unsigned int i = 0; i < text.length(); i++) {
                uint8_t c = (uint8_t)text[i];
                if (c >= 'a' && c <= 'z') {
                    key_tap(0, 0x04 + (c - 'a'), 40);
                } else if (c >= 'A' && c <= 'Z') {
                    key_tap(0x02, 0x04 + (c - 'A'), 40);
                } else if (c == ' ') {
                    key_tap(0, 0x2C, 40);
                } else if (c >= '0' && c <= '9') {
                    key_tap(0, 0x1E + (c - '0'), 40);
                } else if (c == '\n') {
                    key_tap(0, 0x28, 40);
                } else {
                    key_tap(0, c, 40);
                }
                delay(30);
            }
            Serial.println("OK TEXT");
        } else {
            Serial.println("ERR NOT CONNECTED");
        }
    } else if (line == "STATUS") {
        Serial.print("CONNECTED=");
        Serial.println(is_connected() ? "1" : "0");
    } else {
        Serial.println("ERR UNKNOWN");
    }
}

/* --- BLE init ---------------------------------------------------------- */
static bool ble_init(void)
{
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setSecurityAuth(true, false, false); /* bonding, no MITM */

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(new C3ServerCallbacks());
    g_server->advertiseOnDisconnect(true);

    g_hid = new NimBLEHIDDevice(g_server);
    g_hid->setManufacturer(MANUFACTURER);
    g_hid->setPnp(0x02, 0xE502, 0xA111, 0x0210);
    g_hid->setHidInfo(0x00, 0x01);
    g_hid->setReportMap(const_cast<uint8_t*>(REPORT_MAP), sizeof(REPORT_MAP));

    g_protocolMode = g_hid->getProtocolMode();
    g_protocolMode->setValue(0); /* boot protocol default */

    /* Create boot input ourselves with READ|NOTIFY so the dongle's
     * Read-Using-Char-UUID discovery can read the value handle. */
    NimBLEService* hidSvc = g_hid->getHidService();
    g_bootInput = hidSvc->createCharacteristic(
        (uint16_t)0x2A22,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    g_bootInput->setValue((uint8_t*)"\x00\x00\x00\x00\x00\x00\x00\x00", 8);

    g_bootOutput = g_hid->getBootOutput();
    g_bootOutput->setCallbacks(nullptr);

    g_hid->startServices();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setName(DEVICE_NAME);
    adv->setAppearance(HID_KEYBOARD);
    adv->addServiceUUID(g_hid->getHidService()->getUUID());
    adv->enableScanResponse(false);
    adv->start();

    Serial.println("BLE keyboard started");
    return true;
}

/* --- setup / loop ------------------------------------------------------ */
void setup(void)
{
    Serial.begin(115200);
    delay(500);
    Serial.println("\r\nesp32c3_kbd start");

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

        if (MDNS.begin(HOSTNAME)) {
            Serial.println("mDNS: " HOSTNAME ".local");
        } else {
            Serial.println("mDNS init failed");
        }

        g_server_http.on("/status", HTTP_GET, handle_status);
        g_server_http.on("/tap",    HTTP_POST, handle_tap);
        g_server_http.on("/text",   HTTP_POST, handle_text);
        g_server_http.onNotFound(handle_not_found);
        g_server_http.begin();
        Serial.println("HTTP server on port 80");
    } else {
        Serial.println("\r\nWiFi connection failed, HTTP disabled");
    }

    ble_init();
}

void loop(void)
{
    if (g_wifi_ready) g_server_http.handleClient();
    handle_serial();
    type_poll();
    delay(2);
}
