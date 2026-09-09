/*
 * ble_params.ino - parameter protocol + voltage telemetry + voltage loop
 * Target: Arduino Nano ESP32 (ESP32-S3), arduino-esp32 core 2.x
 *
 * Firmware owns the limits. The app proposes, this clamps and reports
 * back what was actually stored.
 *
 * Voltage control is closed-loop: a 12-bit PWM drives the LM317 ADJ current
 * sink, the sense divider is read back, and the duty is trimmed until the
 * measurement is within 1 % of the setpoint. Nothing in the set path needs
 * to be accurate - the loop absorbs op-amp offset, base current, and rail
 * and resistor tolerance. Only the READ path is calibrated; see VSENSE_GAIN.
 *
 * No pulse train here. FIRE/ABORT are not implemented in this sketch.
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

// ---- CALIBRATION ----------------------------------------------------------
// Placeholders. Put a trusted meter on the electrodes, command a low and a
// high setpoint, and solve the two-point fit:
//
//   GAIN   = (V_meter_hi - V_meter_lo) / (V_reported_hi - V_reported_lo)
//   OFFSET = V_meter_lo - GAIN * V_reported_lo
//
// Only the READ path needs this. The set path is inside the closed loop, so
// its tolerances - op-amp offset, base current, rail and resistor error - are
// absorbed automatically and must NOT be calibrated out separately.
static const float  VSENSE_GAIN   = 1.000f;
static const float  VSENSE_OFFSET = 0.000f;   // volts

// ---- voltage set: PWM as a DAC into the LM317 ADJ current sink ------------
// D2 is reserved for the pulse gate, so the DAC lives on D3 (GPIO6). Avoid
// the strapping pins (GPIO0/3/45/46).
static const int    VSET_PIN     = D3;
static const int    VSET_CH      = 0;
static const int    VSET_BITS    = 12;
static const int    VSET_MAX     = (1 << VSET_BITS) - 1;             // 4095
static const float  VSET_FULL    = (float)(1 << VSET_BITS);          // 4096
static const int    VSET_FREQ_HZ = 19531;    // 80 MHz / 4096, forced at 12 bit

// Plant constants, design notes S2. V_out = 1.25 + R2 * (5.26mA - V_filter/330)
static const float  VDD_PWM    = 3.3f;
static const float  R2_OHM     = 8250.0f;
static const float  RSENSE_OHM = 330.0f;
static const float  I_ADJ_FULL = 5.26e-3f;   // 1.25/240 plus the LM317's I_adj
static const float  VREF_317   = 1.25f;

// Output volts per PWM count, ~20 mV. The slope is NEGATIVE: more duty means
// more current stolen from ADJ, which means a LOWER output. Every sign in the
// loop below follows from that.
static const float  VOLTS_PER_COUNT = (VDD_PWM * R2_OHM) / (RSENSE_OHM * VSET_FULL);

// ---- regulator tuning ----
// The ADJ filter is 10k + 1uF, tau = 10 ms. Sampling faster than ~5 tau reads
// the filter mid-slew and the loop chases its own settling.
static const int    REG_PERIOD_MS = 60;
static const float  REG_GAIN      = 0.6f;    // damping; 1.0 overshoots on ADC noise
static const float  REG_TOL_FRAC  = 0.01f;   // the 1 % target from the notes
static const float  REG_TOL_ABS   = 0.15f;   // floor, so 2 V setpoints stay reachable
static const int    REG_SETTLE_N  = 3;
static const int    REG_MAX_STEPS = 60;      // ~3.6 s, then declare non-convergence

// Boot state is full sink: output parked at 1.25 V, matching where the 10k
// pull-up holds it while the GPIO is still high-Z before setup() runs.
int      vsetCounts  = VSET_MAX;
bool     regSettled  = false;
bool     regFailed   = false;
int      regSettleHits = 0;
int      regSteps    = 0;
uint32_t lastReg     = 0;

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
  const float raw = (acc / 16.0f) * DIVIDER_GAIN / 1000.0f;
  return raw * VSENSE_GAIN + VSENSE_OFFSET;
}

// ---------------------------------------------------------- regulator

// Core 3.x renamed the LEDC API. This sketch targets 2.x, but the IDE now
// installs 3.x by default and the failure is a confusing compile error.
static inline void vsetApply(int counts) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(VSET_PIN, counts);
#else
  ledcWrite(VSET_CH, counts);
#endif
}

void vsetWrite(int counts) {
  vsetCounts = constrain(counts, 0, VSET_MAX);
  vsetApply(vsetCounts);
}

// Open-loop seed straight from the design equation. It lands within a few
// hundred mV, so the closed loop only trims tolerances instead of searching
// the whole range - which is what keeps convergence to a handful of steps.
int countsForVolts(float v) {
  const float iSink   = I_ADJ_FULL - (v - VREF_317) / R2_OHM;
  const float vFilter = iSink * RSENSE_OHM;
  return constrain((int)lrintf(vFilter / VDD_PWM * VSET_FULL), 0, VSET_MAX);
}

// Call whenever the target moves. Re-seeding beats letting the integrator
// walk across the range.
void regRestart() {
  regSettled     = false;
  regFailed      = false;
  regSettleHits  = 0;
  regSteps       = 0;
  vsetWrite(countsForVolts(P.volts));
}

void serviceRegulator() {
  if (regFailed) return;                 // latched; a new SET PV clears it

  const float measured = readVolts();
  const float err      = P.volts - measured;
  const float tol      = max(REG_TOL_ABS, REG_TOL_FRAC * P.volts);

  if (fabsf(err) <= tol) {
    // REG_MAX_STEPS has to bound one convergence attempt, not the lifetime of
    // the sketch. Without this reset, hours of slow thermal drift accumulate
    // single-count corrections until the counter trips and reports a fault on
    // a rail that is behaving perfectly.
    regSteps = 0;
    if (!regSettled && ++regSettleHits >= REG_SETTLE_N) {
      regSettled = true;
      Serial.printf("[reg] settled %.2f V at %d counts\n", measured, vsetCounts);
    }
    return;                              // deadband: do not chase ADC noise
  }

  regSettled    = false;
  regSettleHits = 0;

  // Negative slope, hence the minus. Always move at least one count, or a
  // sub-LSB error stalls the loop just outside tolerance forever.
  int step = (int)lrintf(-(err / VOLTS_PER_COUNT) * REG_GAIN);
  if (step == 0) step = (err > 0.0f) ? -1 : 1;

  const int next = constrain(vsetCounts + step, 0, VSET_MAX);

  // Railed and still short of target: the setpoint is simply unreachable on
  // this supply. Notes S3 - flag it rather than chase it. At R2 = 8k25 the
  // ceiling is ~44.65 V, so a 45 V request lands here until the brick is
  // trimmed up. The output is at the best achievable value and the reading
  // agrees with it, so HOLD - parking would be an overreaction.
  if (next == vsetCounts) {
    regFailed = true;
    enqueue("FAULT VOLT RAIL");
    Serial.printf("[reg] unreachable: want %.2f V, railed at %d counts, reading %.2f V\n",
                  P.volts, vsetCounts, measured);
    return;
  }

  vsetWrite(next);

  // Not railed, but the reading will not come to the setpoint. The loop is
  // driving and the measurement is not responding, so the sense path is
  // suspect - open divider, dead op-amp, unseated wire - and the true output
  // is unknown. That is the one case where the failure direction could be
  // upward, so park at full sink (1.25 V) instead of holding.
  if (++regSteps > REG_MAX_STEPS) {
    regFailed = true;
    vsetWrite(VSET_MAX);
    enqueue("FAULT VOLT NOCONV");
    Serial.printf("[reg] no convergence after %d steps, reading %.2f V - parked\n",
                  regSteps, measured);
  }
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

  // A new target re-seeds the loop and clears any latched fault, so an
  // unreachable setpoint can be backed off from without a power cycle.
  if (key == "PV") regRestart();

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
  // First thing, ahead of the serial delay: take the DAC pin out of high-Z
  // and drive full sink, which parks the output at 1.25 V. This shortens the
  // window where only the external 10k pull-up is holding the safe state. The
  // resistor is still what guarantees it - this only narrows the gap.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(VSET_PIN, VSET_FREQ_HZ, VSET_BITS);
#else
  ledcSetup(VSET_CH, VSET_FREQ_HZ, VSET_BITS);
  ledcAttachPin(VSET_PIN, VSET_CH);
#endif
  vsetWrite(VSET_MAX);

  Serial.begin(115200);
  delay(2000);
  Serial.println("\n[BLE] boot build=" __DATE__ " " __TIME__);

  analogReadResolution(12);
  analogSetPinAttenuation(VSENSE_PIN, ADC_11db);

  // Seed the loop at the default setpoint. The gating relay is unpowered and
  // therefore open, so the electrodes see nothing while this settles.
  regRestart();
  Serial.printf("[reg] seed %d counts for %.1f V (%.1f mV/count)\n",
                vsetCounts, P.volts, VOLTS_PER_COUNT * 1000.0f);

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
  // Runs whether or not a central is connected - the rail should be at the
  // commanded voltage before anyone asks to fire. When the pulse train lands,
  // this must be suspended for the duration of a train (design notes S2:
  // settle open-circuit, then fire) - the load changes the reading and the
  // loop would fight it.
  if (millis() - lastReg >= REG_PERIOD_MS) {
    lastReg = millis();
    serviceRegulator();
  }

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
