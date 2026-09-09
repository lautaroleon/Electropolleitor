/*
 * ble_params.ino - parameter protocol + voltage telemetry
 * Target: Arduino Nano ESP32 (ESP32-S3), arduino-esp32 core 2.x
 *
 * Firmware owns the limits. The app proposes, this clamps and reports
 * back what was actually stored.
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define DEVICE_NAME  "EPORATOR01"

#define TXQ_LEN 12
String   txQ[TXQ_LEN];
int      txHead = 0, txTail = 0;
uint32_t lastTx = 0;

// ---- voltage sense divider: 47k + 47k over 5.6k ----
static const int    VSENSE_PIN   = A0;
static const float  DIVIDER_GAIN = (47.0f + 47.0f + 5.6f) / 5.6f;   // 17.79
static const int    MV_PERIOD_MS = 250;                             // 4 Hz

// ---- parameters, with authoritative limits ----
struct Params {
  int   nPulses = 5;     // 2..7
  int   widthMs = 30;    // 10..70
  int   gapMs   = 100;   // 25..200
  float volts   = 10.0f; // 2..45
} P;

BLEServer         *pServer = nullptr;
BLECharacteristic *pTxChar = nullptr;
bool     connectedNow  = false;
bool     connectedPrev = false;
uint32_t lastMv        = 0;
String   rxBuf;                 // BLE writes are not line-aligned

// ---------------------------------------------------------------- tx
void enqueue(const String &s) {
  int next = (txHead + 1) % TXQ_LEN;
  if (next == txTail) return;          // full: drop rather than block
  txQ[txHead] = s;
  txHead = next;
}

void txLine(const String &s) {
  if (!connectedNow || !pTxChar) return;
  String out = s + "\n";
  pTxChar->setValue((uint8_t *)out.c_str(), out.length());
  pTxChar->notify();
  Serial.println("[tx] " + s);
}

void sendAll() {
  enqueue("ACK NP " + String(P.nPulses));
  enqueue("ACK PW " + String(P.widthMs));
  enqueue("ACK PG " + String(P.gapMs));
  enqueue("ACK PV " + String(P.volts, 1));
}

// ------------------------------------------------------------ measure
float readVolts() {
  uint32_t acc = 0;
  for (int i = 0; i < 16; i++) acc += analogReadMilliVolts(VSENSE_PIN);
  return (acc / 16.0f) * DIVIDER_GAIN / 1000.0f;
}

// ------------------------------------------------------------- parser

template <typename T>
bool clampSet(T &dst, float v, T lo, T hi) {
  if (v < lo || v > hi) return false;      // reject rather than silently clamp
  dst = (T)v;
  return true;
}

void handleLine(String line) {
  line.trim();
  if (line.length() == 0) return;
  Serial.println("[rx] " + line);

  if (line == "GET ALL") { sendAll(); return; }

  if (!line.startsWith("SET ")) { txLine("NAK ?"); return; }

  // SET <KEY> <VALUE>
  int s1 = line.indexOf(' ', 4);
  if (s1 < 0) { txLine("NAK ?"); return; }

  String key = line.substring(4, s1);
  float  val = line.substring(s1 + 1).toFloat();

  bool ok = false;
  if      (key == "NP") ok = clampSet(P.nPulses, val, 2,   7);
  else if (key == "PW") ok = clampSet(P.widthMs, val, 10,  70);
  else if (key == "PG") ok = clampSet(P.gapMs,   val, 25,  200);
  else if (key == "PV") ok = clampSet(P.volts,   val, 2.0f, 45.0f);
  else { enqueue("NAK " + key); return; }

  if (!ok) { enqueue("NAK " + key); return; }

  // Echo the stored value - this is what the GUI will display.
  if      (key == "PV") enqueue("ACK PV " + String(P.volts, 1));
  else if (key == "NP") enqueue("ACK NP " + String(P.nPulses));
  else if (key == "PW") enqueue("ACK PW " + String(P.widthMs));
  else if (key == "PG") enqueue("ACK PG " + String(P.gapMs));
}

// ---------------------------------------------------------- callbacks

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    connectedNow = true;
    rxBuf = "";
    Serial.println("[BLE] central connected");
  }
  void onDisconnect(BLEServer *) override {
    connectedNow = false;
    Serial.println("[BLE] central disconnected");
  }
};

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    auto v = c->getValue();
    rxBuf += String(v.c_str());

    int nl;
    while ((nl = rxBuf.indexOf('\n')) >= 0) {
      handleLine(rxBuf.substring(0, nl));
      rxBuf = rxBuf.substring(nl + 1);
    }
    if (rxBuf.length() > 128) rxBuf = "";   // runaway guard
  }
};

class CccdCallbacks : public BLEDescriptorCallbacks {
  void onWrite(BLEDescriptor *d) override {
    uint8_t *v = d->getValue();
    Serial.printf("[BLE] cccd: %02X %02X\n", v[0], v[1]);
  }
};

// ------------------------------------------------------------- setup

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n[BLE] boot build=" __DATE__ " " __TIME__);

  analogReadResolution(12);
  analogSetPinAttenuation(VSENSE_PIN, ADC_11db);

  BLEDevice::init(DEVICE_NAME);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pTxChar = pService->createCharacteristic(
      CHAR_UUID_TX, BLECharacteristic::PROPERTY_NOTIFY);
  BLE2902 *p2902 = new BLE2902();
  p2902->setCallbacks(new CccdCallbacks());
  pTxChar->addDescriptor(p2902);      // required on core 2.x

  BLECharacteristic *pRxChar = pService->createCharacteristic(
      CHAR_UUID_RX,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pRxChar->setCallbacks(new RxCallbacks());

  pService->start();

  BLEAdvertising *pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->setScanResponse(true);
  //pAdv->setMinPreferred(0x06);
  BLEDevice::startAdvertising();

  Serial.println("[BLE] advertising as " DEVICE_NAME);
}

void loop() {
  if (millis() - lastMv >= MV_PERIOD_MS) {
    lastMv = millis();
    float v = readVolts();
    Serial.printf("[mv] %.2f conn=%d\n", v, connectedNow);
    if (connectedNow) enqueue("MV " + String(v, 2));
  }

  if (connectedNow && txHead != txTail && millis() - lastTx >= 25) {
    lastTx = millis();
    txLine(txQ[txTail]);
    txTail = (txTail + 1) % TXQ_LEN;
  }

  if (!connectedNow && connectedPrev) {
    delay(500);
    pServer->startAdvertising();
    Serial.println("[BLE] re-advertising");
  }
  connectedPrev = connectedNow;

  delay(5);
}
