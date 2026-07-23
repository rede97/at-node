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
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Wire.h>
#include <PubSubClient.h>
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

/* --- MQTT client ------------------------------------------------------- */
static WiFiClientSecure g_mqtt_wifi;
static PubSubClient     g_mqtt(g_mqtt_wifi);
static bool             g_mqtt_connected = false;
static String           g_mqtt_broker;
static int              g_mqtt_port = 8883;
static String           g_mqtt_user;
static String           g_mqtt_pass;
static String           g_mqtt_client_id;
static String           g_mqtt_topic_prefix;

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
    g_mqtt_broker = prefs.getString("mqtt_broker", "");
    g_mqtt_port   = prefs.getInt("mqtt_port", 8883);
    g_mqtt_user   = prefs.getString("mqtt_user", "");
    g_mqtt_pass   = prefs.getString("mqtt_pass", "");
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
        if (c1 > 0) {
            String sub = args.substring(0, c1);
            String val = args.substring(c1 + 1);
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
            } else if (sub == "status") {
                resp = "+MQTT:" + String(g_mqtt_connected ? "connected" : "disconnected");
            } else {
                resp = "ERROR";
            }
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
static void mqtt_callback(char* topic, byte* payload, unsigned int length)
{
    Serial.printf("MQTT [%s] ", topic);
    for (unsigned int i = 0; i < length; i++) Serial.print((char)payload[i]);
    Serial.println();
}

static bool mqtt_connect(void)
{
    if (g_mqtt_broker.length() == 0) return false;
    g_mqtt.setServer(g_mqtt_broker.c_str(), g_mqtt_port);
    g_mqtt.setCallback(mqtt_callback);
    g_mqtt_client_id = "atnode-" + g_hostname;
    g_mqtt_topic_prefix = "atnode/" + g_hostname;

    g_mqtt_wifi.setInsecure(); /* skip cert verify for local dev; use setCACert for prod */

    bool ok;
    if (g_mqtt_user.length() > 0) {
        ok = g_mqtt.connect(g_mqtt_client_id.c_str(), g_mqtt_user.c_str(), g_mqtt_pass.c_str());
    } else {
        ok = g_mqtt.connect(g_mqtt_client_id.c_str());
    }
    g_mqtt_connected = ok;
    return ok;
}

static void mqtt_poll(void)
{
    if (!g_mqtt_connected) return;
    if (!g_mqtt.loop()) {
        g_mqtt_connected = false;
    }
}

static void handle_mqtt_status(void)
{
    String json = "{";
    json += "\"connected\":";
    json += g_mqtt_connected ? "true" : "false";
    json += ",\"broker\":\"" + g_mqtt_broker + "\"";
    json += ",\"port\":" + String(g_mqtt_port);
    json += ",\"client_id\":\"" + g_mqtt_client_id + "\"";
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

static void handle_mqtt_connect(void)
{
    bool ok = mqtt_connect();
    send_json("{\"ok\":" + String(ok ? "true" : "false") +
              ",\"cmd\":\"mqtt/connect\",\"connected\":" +
              String(g_mqtt_connected ? "true" : "false") + "}");
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
        if (c1 > 0) {
            String sub = args.substring(0, c1);
            String val = args.substring(c1 + 1);
            if (sub == "broker") {
                g_mqtt_broker = val;
                save_config("mqtt_broker", val);
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
            } else {
                Serial.println("ERROR");
            }
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
        g_http.on("/at-node/cmd/gpio/write", HTTP_POST, handle_gpio_write);
        g_http.on("/at-node/cmd/gpio/read", HTTP_POST, handle_gpio_read);
        g_http.on("/at-node/cmd/adc/read", HTTP_POST, handle_adc_read);
        g_http.on("/at-node/cmd/i2c/scan", HTTP_POST, handle_i2c_scan);
        g_http.on("/at-node/cmd/i2c/read", HTTP_POST, handle_i2c_read);
        g_http.on("/at-node/cmd/i2c/write", HTTP_POST, handle_i2c_write);
        g_http.on("/at-node/cmd/ir/send", HTTP_POST, handle_ir_send);
        g_http.on("/at-node/cmd/mqtt/status", HTTP_GET, handle_mqtt_status);
        g_http.on("/at-node/cmd/mqtt/config", HTTP_POST, handle_mqtt_config);
        g_http.on("/at-node/cmd/mqtt/connect", HTTP_POST, handle_mqtt_connect);
        g_http.on("/at-node/cmd/mqtt/publish", HTTP_POST, handle_mqtt_publish);
        g_http.on("/at-node/cmd/mqtt/subscribe", HTTP_POST, handle_mqtt_subscribe);
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
}

void loop(void)
{
    if (g_wifi_ready) g_http.handleClient();
    handle_serial();
    type_poll();
    mqtt_poll();
    delay(2);
}
