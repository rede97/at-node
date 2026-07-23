/*
 * esp32c3_kbd.ino
 *
 * ESP32-C3 programmable BLE HID keyboard test bench.
 * Used as an alternative keyboard peripheral to validate the
 * CH582F dongle (DONGLE=1 or MODE=DUAL) over BLE.
 *
 * Features:
 *   - Standard Arduino ESP32-BLE-Keyboard library, boot keyboard report.
 *   - WiFi HTTP control plane on http://esp32kbd.local
 *   - USB serial fallback commands
 *
 * Endpoints:
 *   GET  /status          -> JSON with BLE connection state and IP
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
#include <BleKeyboard.h>

/* --- user configuration: edit before build ----------------------------- */
#ifndef WIFI_SSID
#define WIFI_SSID     "YOUR_SSID"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "YOUR_PASSWORD"
#endif

#define HOSTNAME      "esp32kbd"
#define DEVICE_NAME   "C3-Kbd"
#define MANUFACTURER  "AT-Node"

/* --- globals ----------------------------------------------------------- */
BleKeyboard bleKeyboard(DEVICE_NAME, MANUFACTURER, 100);
WebServer server(80);

static bool wifi_ready = false;

/* --- helpers ----------------------------------------------------------- */
static uint8_t parse_mods(const String& s)
{
    if (s.length() == 0) {
        return 0;
    }
    return (uint8_t)strtoul(s.c_str(), NULL, 0);
}

static uint8_t parse_key(const String& s)
{
    if (s.length() == 0) {
        return 0;
    }
    return (uint8_t)strtoul(s.c_str(), NULL, 0);
}

static void send_key_report(uint8_t mods, uint8_t k0, uint8_t k1,
                            uint8_t k2, uint8_t k3, uint8_t k4, uint8_t k5)
{
    KeyReport report;
    report.modifiers = mods;
    report.reserved  = 0;
    report.keys[0]   = k0;
    report.keys[1]   = k1;
    report.keys[2]   = k2;
    report.keys[3]   = k3;
    report.keys[4]   = k4;
    report.keys[5]   = k5;
    bleKeyboard.sendReport(&report);
}

static void clear_keys(void)
{
    send_key_report(0, 0, 0, 0, 0, 0, 0);
}

static void key_tap(uint8_t mods, uint8_t key, int ms)
{
    if (ms <= 0) {
        ms = 100;
    }
    send_key_report(mods, key, 0, 0, 0, 0, 0);
    delay(ms);
    clear_keys();
}

/* --- HTTP handlers ----------------------------------------------------- */
static void handle_status(void)
{
    String json = "{";
    json += "\"connected\":";
    json += bleKeyboard.isConnected() ? "true" : "false";
    json += ",\"ip\":\"";
    json += WiFi.localIP().toString();
    json += "\"";
    json += ",\"hostname\":\"" HOSTNAME "\"";
    json += "}";
    server.send(200, "application/json", json);
}

static void handle_tap(void)
{
    if (!bleKeyboard.isConnected()) {
        server.send(409, "text/plain", "BLE not connected");
        return;
    }

    uint8_t mods = parse_mods(server.arg("mods"));
    uint8_t key  = parse_key(server.arg("k"));
    int ms       = server.arg("ms").toInt();

    key_tap(mods, key, ms);
    server.send(200, "text/plain", "OK");
}

static void handle_text(void)
{
    if (!bleKeyboard.isConnected()) {
        server.send(409, "text/plain", "BLE not connected");
        return;
    }

    String text = server.arg("s");
    if (text.length() == 0) {
        server.send(400, "text/plain", "missing s=");
        return;
    }

    bleKeyboard.print(text);
    server.send(200, "text/plain", "OK");
}

static void handle_not_found(void)
{
    server.send(404, "text/plain", "not found");
}

/* --- serial fallback --------------------------------------------------- */
static void handle_serial(void)
{
    if (!Serial.available()) {
        return;
    }

    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) {
        return;
    }

    if (line.startsWith("TAP ")) {
        String args = line.substring(4);
        args.trim();
        uint8_t key  = (uint8_t)strtoul(args.c_str(), NULL, 0);
        key_tap(0, key, 100);
        Serial.println("OK TAP");
    } else if (line.startsWith("TEXT ")) {
        String text = line.substring(5);
        if (bleKeyboard.isConnected()) {
            bleKeyboard.print(text);
            Serial.println("OK TEXT");
        } else {
            Serial.println("ERR NOT CONNECTED");
        }
    } else if (line == "STATUS") {
        Serial.print("CONNECTED=");
        Serial.println(bleKeyboard.isConnected() ? "1" : "0");
    } else {
        Serial.println("ERR UNKNOWN");
    }
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
        wifi_ready = true;
        Serial.println();
        Serial.print("WiFi connected, IP=");
        Serial.println(WiFi.localIP());

        if (MDNS.begin(HOSTNAME)) {
            Serial.println("mDNS: " HOSTNAME ".local");
        } else {
            Serial.println("mDNS init failed");
        }

        server.on("/status", HTTP_GET, handle_status);
        server.on("/tap",    HTTP_POST, handle_tap);
        server.on("/text",   HTTP_POST, handle_text);
        server.onNotFound(handle_not_found);
        server.begin();
        Serial.println("HTTP server on port 80");
    } else {
        Serial.println("\r\nWiFi connection failed, HTTP disabled");
    }

    bleKeyboard.begin();
    Serial.println("BLE keyboard started");
}

void loop(void)
{
    if (wifi_ready) {
        server.handleClient();
    }
    handle_serial();
    delay(2);
}
