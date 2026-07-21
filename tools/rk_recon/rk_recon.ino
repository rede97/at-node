/*
  ESP32-C3 BLE recon probe — AT-Node development tool.

  Purpose (evolved 2026-07):
    Started as a known-good host to validate AT-Node's BLE keyboard mode
    (connect -> subscribe 0x2A4D -> 8-byte reports, VERIFIED working).
    Now serves as the BLE reconnaissance tool for the dongle (BLE HID
    receiver) investigation: GATT dump + Report Map reading + pairing
    experiments against arbitrary keyboards.

  Current behavior:
    1. Scans FOREVER for "RK-S75RGB" advertising 0x1812 (loop() restarts
       the scan whenever disconnected — eliminates the 5-second window
       race with the keyboard's short pairing-mode timeout).
    2. Connects with security (Bluedroid on-connect, static passkey
       123456, MITM on; NVS erased every boot for a clean pairing).
    3. Dumps all GATT services/characteristics to serial.
    4. Reads Protocol Mode (0x2A4E), dumps the full Report Map (0x2A4B)
       as hex, writes Protocol Mode = Boot, subscribes Boot Keyboard
       Input (0x2A22) or falls back to the first 0x2A4D, plus Battery
       (0x2A19). Prints every notification as hex.

  Findings on RK-S75RGB (2026-07-22, full postmortem in software/PLAN.md §3.0):

  Current status: keyboard displays PAIRED; battery level (0x2A19)
  notifications stream in; keystream does NOT arrive — no keyboard
  input report reaches this host.

    - Boot char 0x2A22 is hollow: subscribes fine, never emits reports.
    - Report Map (331B) defines 5 report IDs: ID1 NKRO bitmap (16B),
      ID2 standard 8-byte boot layout, ID3 vendor, ID4 consumer control,
      ID5 system control. Notifications carry a Report ID prefix byte.
    - Battery (0x2A19) notifies constantly — notify path works; only the
      keyboard input stream is silent to this host.

  Tool limitations (do NOT use as keystream validator):
    - Bluedroid Arduino getCharacteristics() is a std::map keyed by UUID
      string — multiple characteristics sharing one UUID (RK has ~8
      Report chars 0x2A4D) COLLAPSE to a single entry. This probe cannot
      enumerate or subscribe the rest. Use the AT-Node dongle (handle-
      based GATT) for subscription-level testing instead.
    - NVS erase at boot + keyboard bond retention: keyboard must be put
      back into pairing mode (clears its bond) for every test round,
      otherwise it advertises without name/HID UUID (or directed only)
      and the name filter misses it.

  Build (arduino-cli, needs USB CDC for serial output!):
    arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc rk_recon
    arduino-cli upload -p COMx --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc rk_recon
    Then open serial monitor at 115200. esp32 core 3.3.10; on slow
    networks stage tool zips from dl.espressif.com/github_assets into
    %LOCALAPPDATA%/Arduino15/staging/packages.
*/

#include "BLEDevice.h"
#include "BLESecurity.h"
#include "nvs_flash.h"

static const String DeviceName = "RK-S75RGB";

static const int led = 8;

// The remote service we wish to connect to.
static BLEUUID KEYBOARD_serviceUUID(uint16_t(0x1812));
static BLEUUID KEYBOARD_secureCharUUID(uint16_t(0x2a4d));

static BLEUUID BATTERY_serviceUUID(uint16_t(0x180f));  //insecure
static BLEUUID BATTERY_charUUID(uint16_t(0x2a19));

// This must match the server's passkey
#define CLIENT_PIN 123456

static boolean doConnect = false;
static boolean connected = false;
static boolean doScan = false;
static BLEClient *pClient = nullptr;
static BLERemoteCharacteristic *pRemoteBatteryCharacteristic;
static BLERemoteCharacteristic *pRemoteKeyboardCharacteristic;
static BLEAdvertisedDevice *myDevice;

static void printHex(uint8_t *irk, size_t len = 16) {
  for (int i = 0; i < len; i++) {
    if (irk[i] < 0x10) {
      Serial.print("0");
    }
    Serial.print(irk[i], HEX);
    if (i < len - 1) {
      Serial.print(":");
    }
  }
}

static void get_peer_irk(BLEAddress peerAddr) {
  Serial.println("\n=== Retrieving peer IRK (Server) ===\n");

  uint8_t irk[16];

  // Get IRK in binary format
  if (BLEDevice::getPeerIRK(peerAddr, irk)) {
    Serial.println("Successfully retrieved peer IRK in binary format:");
    printHex(irk);
    Serial.println("\n");
  }

  // Get IRK in different string formats
  String irkString = BLEDevice::getPeerIRKString(peerAddr);
  String irkBase64 = BLEDevice::getPeerIRKBase64(peerAddr);
  String irkReverse = BLEDevice::getPeerIRKReverse(peerAddr);

  if (irkString.length() > 0) {
    Serial.println("Successfully retrieved peer IRK in multiple formats:\n");
    Serial.print("IRK (comma-separated hex): ");
    Serial.println(irkString);
    Serial.print("IRK (Base64 for Home Assistant Private BLE Device): ");
    Serial.println(irkBase64);
    Serial.print("IRK (reverse hex for Home Assistant ESPresense): ");
    Serial.println(irkReverse);
    Serial.println();
  } else {
    Serial.println("!!! Failed to retrieve peer IRK !!!");
  }

  Serial.println("=======================================\n");
}

// Callback function to handle notifications
static void notifyCallback(BLERemoteCharacteristic *pBLERemoteCharacteristic, uint8_t *pData, size_t length, bool isNotify) {
  Serial.print("Notify callback for characteristic ");
  Serial.println(pBLERemoteCharacteristic->getUUID().toString().c_str());
  if (pData[2]) {
    digitalWrite(led, LOW);
  } else {
    digitalWrite(led, HIGH);
  }
  printHex(pData, length);
  Serial.println();
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient *pclient) {
    Serial.println("Connected to secure server");
  }

  void onDisconnect(BLEClient *pclient) {
    connected = false;
    Serial.println("Disconnected from server");
  }
};

// Security callbacks to print IRKs once authentication completes
class MySecurityCallbacks : public BLESecurityCallbacks {
};

bool connectToServer() {
  Serial.print("Forming a secure connection to ");
  Serial.println(myDevice->getAddress().toString().c_str());

  pClient = BLEDevice::createClient();
  Serial.println(" - Created client");

  pClient->setClientCallbacks(new MyClientCallback());

  // Connect to the remote BLE Server.
  pClient->connect(myDevice);
  Serial.println(" - Connected to server");

  // Set MTU to maximum for better performance
  pClient->setMTU(517);

  // Obtain a reference to the service we are after in the remote BLE server.
  std::map<std::string, BLERemoteService *> *pServices = pClient->getServices();
  for (auto &servicePair : *pServices) {
    Serial.print("Service: ");
    Serial.println(servicePair.first.c_str());
    std::map<std::string, BLERemoteCharacteristic *> *pChars = servicePair.second->getCharacteristics();
    for (auto &charPair : *pChars) {
      Serial.print("-- Character: ");
      Serial.println(charPair.first.c_str());
      // std::map<std::string, BLERemoteDescriptor *> *pDescs = charPair.second->getDescriptors();
      // for (auto &descPair : *pDescs) {
      //   Serial.print("-- Desc: ");
      //   Serial.println(descPair.first.c_str());
      // }
    }
  }

  {
    // connect keyboard service
    BLERemoteService *pRemoteService = pClient->getService(KEYBOARD_serviceUUID);
    if (pRemoteService == nullptr) {
      Serial.print("Failed to find our service UUID: ");
      Serial.println(KEYBOARD_serviceUUID.toString().c_str());
      pClient->disconnect();
      return false;
    }
    Serial.println(" - Found our service");

    /* HOGP ground truth: read Protocol Mode (0x2A4E), Report Map (0x2A4B),
       then subscribe Boot Keyboard Input (0x2A22) after switching to
       Boot protocol. Report Map dump shows every report's layout and
       whether report IDs are used. */
    BLERemoteCharacteristic *pProto = pRemoteService->getCharacteristic(BLEUUID((uint16_t)0x2a4e));
    if (pProto != nullptr && pProto->canRead()) {
      uint8_t pm = pProto->readUInt8();
      Serial.printf(" - Protocol Mode = %d (%s)\n", pm, pm ? "Report" : "Boot");
    }

    BLERemoteCharacteristic *pMap = pRemoteService->getCharacteristic(BLEUUID((uint16_t)0x2a4b));
    if (pMap != nullptr) {
      String map = pMap->readValue();
      Serial.printf(" - Report Map (%d bytes):\n", map.length());
      for (unsigned i = 0; i < map.length(); i++) {
        Serial.printf("%02X ", (uint8_t)map[i]);
        if (i % 16 == 15) Serial.println();
      }
      Serial.println();
    } else {
      Serial.println(" - Report Map NOT FOUND");
    }

    if (pProto != nullptr) {
      uint8_t boot = 0;
      bool ok = false;
      if (pProto->canWrite()) ok = pProto->writeValue(&boot, 1, true);
      if (!ok && pProto->canWriteNoResponse()) ok = pProto->writeValue(&boot, 1, false);
      Serial.printf(" - Protocol Mode -> Boot write %s\n", ok ? "OK" : "FAILED");
    }

    pRemoteKeyboardCharacteristic = pRemoteService->getCharacteristic(BLEUUID((uint16_t)0x2a22));
    if (pRemoteKeyboardCharacteristic != nullptr && pRemoteKeyboardCharacteristic->canNotify()) {
      pRemoteKeyboardCharacteristic->registerForNotify(notifyCallback);
      Serial.println(" - Registered for BOOT keyboard input (0x2A22)");
    } else {
      pRemoteKeyboardCharacteristic = pRemoteService->getCharacteristic(KEYBOARD_secureCharUUID);
      if (pRemoteKeyboardCharacteristic != nullptr && pRemoteKeyboardCharacteristic->canNotify()) {
        pRemoteKeyboardCharacteristic->registerForNotify(notifyCallback);
        Serial.println(" - Registered for report char (0x2A4D)");
      }
    }
  }

  {
    // connect battery service
    BLERemoteService *pRemoteService = pClient->getService(BATTERY_serviceUUID);
    if (pRemoteService == nullptr) {
      Serial.print("Failed to find our service UUID: ");
      Serial.println(KEYBOARD_serviceUUID.toString().c_str());
      pClient->disconnect();
      return false;
    }
    Serial.println(" - Found our BATTERY service");

    pRemoteBatteryCharacteristic = pRemoteService->getCharacteristic(BATTERY_charUUID);
    if (pRemoteBatteryCharacteristic == nullptr) {
      Serial.print("Failed to find insecure characteristic UUID: ");
      Serial.println(BATTERY_charUUID.toString().c_str());
      pClient->disconnect();
      return false;
    }
    Serial.println(" - Found BATTERY characteristic");
    if (pRemoteBatteryCharacteristic->canNotify()) {
      pRemoteBatteryCharacteristic->registerForNotify(notifyCallback);
      Serial.println(" - Registered for BATTERY characteristic notifications");
    }
  }

  connected = true;
  return true;
}

/**
 * Scan for BLE servers and find the first one that advertises the service we are looking for.
 */
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  /**
   * Called for each advertising BLE server.
   */
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    // Serial.print("BLE Advertised Device found: ");
    // Serial.println(advertisedDevice.getName().c_str());
    if (advertisedDevice.getName().indexOf(DeviceName) < 0) {
      return;
    }



    // We have found a device, let us now see if it contains the service we are looking for.
    if (advertisedDevice.haveServiceUUID() && advertisedDevice.isAdvertisingService(KEYBOARD_serviceUUID)) {
      Serial.print("Found our remoter: ");
      Serial.println(advertisedDevice.getName().c_str());
      BLEDevice::getScan()->stop();
      myDevice = new BLEAdvertisedDevice(advertisedDevice);
      doConnect = true;
      doScan = true;
    }
  }
};

void setup() {
  Serial.begin(115200);
  Serial.println("Starting Secure BLE Client application...");

  pinMode(led, OUTPUT);
  digitalWrite(led, HIGH);

  // Clear NVS to remove any cached pairing information
  // This ensures fresh authentication for testing
  Serial.println("Clearing NVS pairing data...");
  nvs_flash_erase();
  nvs_flash_init();

  BLEDevice::init("Secure BLE Client");

  // Set up security with the same passkey as the server
  BLESecurity *pSecurity = new BLESecurity();

  // Set security parameters
  // Default parameters:
  // - IO capability is set to NONE
  // - Initiator and responder key distribution flags are set to both encryption and identity keys.
  // - Passkey is set to BLE_SM_DEFAULT_PASSKEY (123456). It will warn if you don't change it.
  // - Key size is set to 16 bytes

  // Set the same static passkey as the server
  // The first argument defines if the passkey is static or random.
  // The second argument is the passkey (ignored when using a random passkey).
  pSecurity->setPassKey(true, CLIENT_PIN);

  // Set authentication mode to match server requirements
  // Enable bonding, MITM (for password prompts), and secure connection for this example
  // Bonding is required to store and retrieve the IRK
  pSecurity->setAuthenticationMode(true, true, true);

  // Set IO capability to KeyboardOnly
  // We need the proper IO capability for MITM authentication even
  // if the passkey is static and won't be entered by the user
  // See https://www.bluetooth.com/blog/bluetooth-pairing-part-2-key-generation-methods/
  pSecurity->setCapability(ESP_IO_CAP_IN);

  // Set callbacks to handle authentication completion and print IRKs
  BLEDevice::setSecurityCallbacks(new MySecurityCallbacks());

  // Retrieve a Scanner and set the callback we want to use to be informed when we
  // have detected a new device. Specify that we want active scanning and start the
  // scan to run for 5 seconds.
  BLEScan *pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);
  pBLEScan->start(5, false);
}

void loop() {
  // If the flag "doConnect" is true then we have scanned for and found the desired
  // BLE Server with which we wish to connect. Now we connect to it.
  if (doConnect == true) {
    if (connectToServer()) {
      Serial.println("We are now connected to the secure BLE Server.");
    } else {
      Serial.println("We have failed to connect to the server; there is nothing more we will do.");
    }
    doConnect = false;
  }

  // If we are connected to a peer BLE Server, demonstrate secure communication
  if (connected) {
    if (pRemoteBatteryCharacteristic) {
      uint8_t volt = pRemoteBatteryCharacteristic->readUInt8();
      Serial.print("Battery: ");
      Serial.println(volt);
    }
  } else {
    // Scan FOREVER until the keyboard appears — no 5s window race.
    // (setup's start(5) may expire before the user enters pairing mode;
    // this keeps the scanner hot so pairing can happen at leisure.)
    BLEScan *s = BLEDevice::getScan();
    if (!s->isScanning()) {
      s->start(0, false);
      Serial.println("rescanning...");
    }
  }

  delay(2000);  // Delay 2 seconds between loops
}
