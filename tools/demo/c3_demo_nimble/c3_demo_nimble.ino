/*
 * c3_demo_nimble.ino - Minimal NimBLE BLE HID keyboard demo (Windows pairing test)
 *
 * Report-protocol keyboard (Report ID 1 = input 8B, ID 2 = LED output 1B).
 * Serial commands (115200):
 *   t  -> type "Hello" + Enter
 *   c  -> clear all bonds
 *   s  -> print status
 *
 * Pair from Windows Bluetooth settings as "AT-NimBLE-Demo".
 */

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEHIDDevice.h>
#include <NimBLECharacteristic.h>

static NimBLEServer*         g_server       = nullptr;
static NimBLEHIDDevice*      g_hid          = nullptr;
static NimBLECharacteristic* g_inputReport  = nullptr;
static NimBLECharacteristic* g_outputReport = nullptr;

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

class DemoServerCallbacks : public NimBLEServerCallbacks {
public:
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
        Serial.printf("BLE connected, handle=%d addr=%s\n",
                      connInfo.getConnHandle(),
                      connInfo.getAddress().toString().c_str());
    }
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo,
                      int reason) override {
        Serial.printf("BLE disconnected, reason=0x%02X\n", reason);
        NimBLEDevice::startAdvertising();
    }
    void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
        Serial.printf("Auth complete: bonded=%d encrypted=%d addr=%s\n",
                      connInfo.isBonded(), connInfo.isEncrypted(),
                      connInfo.getAddress().toString().c_str());
    }
};

class LedOutputCallbacks : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
        NimBLEAttValue v = pChar->getValue();
        if (v.size() > 0) Serial.printf("LED state: 0x%02X\n", v.data()[0]);
    }
};

static bool is_connected(void)
{
    return g_server && g_server->getConnectedCount() > 0;
}

static void send_report(uint8_t mods, uint8_t key)
{
    uint8_t report[8] = {0};
    report[0] = mods;
    report[2] = key;
    g_inputReport->setValue(report, sizeof(report));
    g_inputReport->notify();
    delay(30);
    memset(report, 0, sizeof(report));
    g_inputReport->setValue(report, sizeof(report));
    g_inputReport->notify();
}

static void type_hello(void)
{
    if (!is_connected()) {
        Serial.println("not connected");
        return;
    }
    /* "Hello" : H(shift+0x0B) e(0x08) l(0x0F) l o(0x12), Enter(0x28) */
    send_report(0x02, 0x0B);
    send_report(0x00, 0x08);
    send_report(0x00, 0x0F);
    send_report(0x00, 0x0F);
    send_report(0x00, 0x12);
    send_report(0x00, 0x28);
    Serial.println("typed Hello");
}

void setup(void)
{
    Serial.begin(115200);
    delay(500);
    Serial.println("\r\nc3_demo_nimble start");

    NimBLEDevice::init("AT-NimBLE-Demo");
    NimBLEDevice::setSecurityAuth(true, false, false); /* bond, no MITM, no SC */

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(new DemoServerCallbacks());
    g_server->advertiseOnDisconnect(true);

    g_hid = new NimBLEHIDDevice(g_server);
    g_hid->setManufacturer("AT-Node");
    g_hid->setPnp(0x02, 0xE502, 0xA111, 0x0210);
    g_hid->setHidInfo(0x00, 0x03);
    g_hid->setReportMap(const_cast<uint8_t*>(REPORT_MAP), sizeof(REPORT_MAP));
    g_hid->setBatteryLevel(100);

    g_inputReport = g_hid->getInputReport(1);
    uint8_t empty_report[8] = {0};
    g_inputReport->setValue(empty_report, sizeof(empty_report));

    g_outputReport = g_hid->getOutputReport(2);
    g_outputReport->setCallbacks(new LedOutputCallbacks());

    g_hid->startServices();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setName("AT-NimBLE-Demo");
    adv->setAppearance(HID_KEYBOARD);
    adv->addServiceUUID(g_hid->getHidService()->getUUID());
    adv->enableScanResponse(false);
    adv->start();

    Serial.println("advertising as AT-NimBLE-Demo");
}

void loop(void)
{
    if (Serial.available()) {
        char c = (char)Serial.read();
        if (c == 't') {
            type_hello();
        } else if (c == 'c') {
            bool ok = NimBLEDevice::deleteAllBonds();
            Serial.printf("bonds cleared: %d\n", ok);
        } else if (c == 's') {
            Serial.printf("connected=%d bonds=%d adv=%d\n",
                          is_connected(),
                          NimBLEDevice::getNumBonds(),
                          NimBLEDevice::getAdvertising()->isAdvertising());
        }
    }
    delay(10);
}
