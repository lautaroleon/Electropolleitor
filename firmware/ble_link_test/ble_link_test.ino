/*
 * ble_link_test.ino — Step 0 connectivity check
 * Target: Arduino Nano ESP32 (ESP32-S3, BLE only — no Bluetooth Classic)
 *
 * Implements Nordic UART Service (NUS) so any generic BLE terminal
 * (nRF Connect, LightBlue) can talk to it before Qt exists.
 *
 *   RX  6E400002  app -> device   (write)
 *   TX  6E400003  device -> app   (notify)
 *
 * Behaviour: echoes anything written to RX, plus a 1 Hz heartbeat.
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// Keep short. A 128-bit UUID eats 18 of the 31 advertising bytes.
#define DEVICE_NAME "EPORATOR01"

BLEServer         *pServer = nullptr;
BLECharacteristic *pTxChar = nullptr;

bool     connectedNow  = false;
bool     connectedPrev = false;
uint32_t lastBeat      = 0;

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override {
    connectedNow = true;
    Serial.println("[BLE] central connected");
  }
  void onDisconnect(BLEServer *s) override {
    connectedNow = false;
    Serial.println("[BLE] central disconnected");
  }
};

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    // arduino-esp32 core 3.x returns Arduino String here.
    // On core 2.x this is std::string — use c->getValue().c_str() instead.

    auto v = c->getValue();              // was: String v = ...
    
    if (v.length() == 0) return;

    Serial.print("[BLE] rx: ");
    Serial.println(v.c_str());           // was: Serial.println(v);

    if (connectedNow && pTxChar) {
      String reply = String("echo:") + v.c_str();
      pTxChar->setValue(reply.c_str());
      pTxChar->notify();
    }
  }
};

void setup() {
  Serial.begin(115200);
  delay(2000);  // Nano ESP32 uses USB CDC; give the port time to enumerate
  Serial.println("\n[BLE] boot");

  BLEDevice::init(DEVICE_NAME);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pTxChar = pService->createCharacteristic(
      CHAR_UUID_TX, BLECharacteristic::PROPERTY_NOTIFY);
  // Core 3.x may add the CCCD automatically. If this line errors, delete it.
  pTxChar->addDescriptor(new BLE2902());

  BLECharacteristic *pRxChar = pService->createCharacteristic(
      CHAR_UUID_RX,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pRxChar->setCallbacks(new RxCallbacks());

  pService->start();

  BLEAdvertising *pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->setScanResponse(true);   // name goes here; adv packet is already full
  pAdv->setMinPreferred(0x06);
  BLEDevice::startAdvertising();

  Serial.println("[BLE] advertising as " DEVICE_NAME);
}

void loop() {
  if (connectedNow && millis() - lastBeat > 1000) {
    lastBeat = millis();
    String msg = "hb " + String(millis() / 1000);
    pTxChar->setValue(msg.c_str());
    pTxChar->notify();
    Serial.println("[BLE] tx: " + msg);
  }

  // The stack does NOT resume advertising on its own after a disconnect.
  if (!connectedNow && connectedPrev) {
    delay(500);
    pServer->startAdvertising();
    Serial.println("[BLE] re-advertising");
  }
  connectedPrev = connectedNow;

  delay(10);
}
