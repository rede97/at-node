/*
 * c3_demo_tvk.ino - Bluedroid BLE keyboard demo (T-vK ESP32-BLE-Keyboard)
 *
 * Known-good Windows-compatible implementation (reference test).
 * Serial commands (115200):
 *   t  -> type "Hello" + Enter
 *   s  -> print connection state
 *
 * Pair from Windows Bluetooth settings as "ESP32 Keyboard".
 * Library patched for esp32 core 3.3.10 (std::string -> String).
 */

#include <BleKeyboard.h>

BleKeyboard bleKeyboard;

static void type_hello(void)
{
    if (!bleKeyboard.isConnected()) {
        Serial.println("not connected");
        return;
    }
    bleKeyboard.print("Hello");
    bleKeyboard.write(KEY_RETURN);
    Serial.println("typed Hello");
}

void setup(void)
{
    Serial.begin(115200);
    delay(500);
    Serial.println("\r\nc3_demo_tvk start (Bluedroid)");
    bleKeyboard.begin();
    Serial.println("advertising as ESP32 Keyboard");
}

void loop(void)
{
    if (Serial.available()) {
        char c = (char)Serial.read();
        if (c == 't') {
            type_hello();
        } else if (c == 's') {
            Serial.printf("connected=%d\n", bleKeyboard.isConnected());
        }
    }
    delay(10);
}
