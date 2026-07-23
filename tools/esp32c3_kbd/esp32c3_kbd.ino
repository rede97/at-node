/*
 * esp32c3_kbd.ino
 *
 * ESP32-C3 programmable BLE HID keyboard test bench.
 * Used as an alternative keyboard peripheral to validate the
 * CH582F dongle (DONGLE=1 or MODE=DUAL) over BLE.
 *
 * Features:
 *   - ESP32BLECombo library (NimBLE), keyboard-only mode.
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
#include <ESP32BLECombo.h>
#include "wifi_config.h"

#define HOSTNAME      "esp32kbd"
#define DEVICE_NAME   "C3-Kbd"
#define MANUFACTURER  "AT-Node"

/* modifier bit -> USB HID modifier keycode */
static const uint8_t MOD_KEYS[8] = {
    0xE0,   /* Left Ctrl  */
    0xE1,   /* Left Shift */
    0xE2,   /* Left Alt   */
    0xE3,   /* Left GUI   */
    0xE4,   /* Right Ctrl */
    0xE5,   /* Right Shift*/
    0xE6,   /* Right Alt  */
    0xE7    /* Right GUI  */
};

/* --- globals ----------------------------------------------------------- */
ESP32BLECombo bleCombo;
WebServer server(80);

static bool wifi_ready = false;

/* --- helpers ----------------------------------------------------------- */
static uint8_t parse_uint8(const String& s)
{
    if (s.length() == 0) {
        return 0;
    }
    return (uint8_t)strtoul(s.c_str(), NULL, 0);
}

static void key_tap(uint8_t mods, uint8_t key, int ms)
{
    if (ms <= 0) {
        ms = 100;
    }

    for (int i = 0; i < 8; i++) {
        if (mods & (1 << i)) {
            bleCombo.press(MOD_KEYS[i]);
        }
    }

    bleCombo.press(key);
    delay(ms);
    bleCombo.release(key);

    for (int i = 0; i < 8; i++) {
        if (mods & (1 << i)) {
            bleCombo.release(MOD_KEYS[i]);
        }
    }
}

/* --- HTTP handlers ----------------------------------------------------- */
static void handle_status(void)
{
    String json = "{";
    json += "\"connected\":";
    json += bleCombo.isConnected() ? "true" : "false";
    json += ",\"ip\":\"";
    json += WiFi.localIP().toString();
    json += "\"";
    json += ",\"hostname\":\"" HOSTNAME "\"";
    json += "}";
    server.send(200, "application/json", json);
}

static void handle_tap(void)
{
    if (!bleCombo.isConnected()) {
        server.send(409, "text/plain", "BLE not connected");
        return;
    }

    uint8_t mods = parse_uint8(server.arg("mods"));
    uint8_t key  = parse_uint8(server.arg("k"));
    int ms       = server.arg("ms").toInt();

    key_tap(mods, key, ms);
    server.send(200, "text/plain", "OK");
}

static void handle_text(void)
{
    if (!bleCombo.isConnected()) {
        server.send(409, "text/plain", "BLE not connected");
        return;
    }

    String text = server.arg("s");
    if (text.length() == 0) {
        server.send(400, "text/plain", "missing s=");
        return;
    }

    bleCombo.print(text);
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
        uint8_t key = (uint8_t)strtoul(args.c_str(), NULL, 0);
        key_tap(0, key, 100);
        Serial.println("OK TAP");
    } else if (line.startsWith("TEXT ")) {
        String text = line.substring(5);
        if (bleCombo.isConnected()) {
            bleCombo.print(text);
            Serial.println("OK TEXT");
        } else {
            Serial.println("ERR NOT CONNECTED");
        }
    } else if (line == "STATUS") {
        Serial.print("CONNECTED=");
        Serial.println(bleCombo.isConnected() ? "1" : "0");
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

    ESP32BLEComboConfig cfg;
    cfg.mode = ESP32BLEComboMode::KEYBOARD_ONLY;
    cfg.deviceName = DEVICE_NAME;
    cfg.manufacturer = MANUFACTURER;
    cfg.batteryLevel = 100;
    cfg.appearance = ESP32BLEComboAppearance::KEYBOARD;
    cfg.enableSecurity = true;
    cfg.keyPressDelayMs = 20;
    cfg.keyReleaseDelayMs = 20;
    cfg.keyIntervalDelayMs = 10;

    bleCombo.begin(cfg);
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
