/*
 * ble_params.ino - parameter protocol + voltage telemetry + voltage loop
 * Target: Arduino Nano ESP32 (ESP32-S3), arduino-esp32 core 2.x
 *
 * Firmware owns the limits. The app proposes, this clamps and reports
 * back what was actually stored.
 *
 * Voltage control is closed-loop: a 12-bit PWM drives the LM317 ADJ current
 * sink, the sense divider is read back, and the duty is trimmed until the
 * measurement is within 1 % of the setpoint.
 *
 * A SET jumps duty straight to the feed-forward estimate from the measured
 * line V_OUT(D) = CAL_V0 - CAL_K * D, freezes the integrator for REG_HOLD
 * periods while the RC filter catches up, then trims. The integrator only
 * has to cover the calibration residual, never search the range.
 *
 * Two calibrations, and they are not interchangeable:
 *   CAL_V0 / CAL_K   the SET path, two duty points against a DMM
 *   VSENSE_GAIN      the READ path, the divider ratio
 * Getting one wrong is corrected by the loop. Getting the other wrong moves
 * what the loop converges TO.
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

// ---- SET-PATH CALIBRATION -------------------------------------------------
// Over the usable range the set path is a straight line:
//
//     V_OUT(D) = CAL_V0 - CAL_K * D          D = duty, 0.0 .. 1.0
//
// Measure it at bring-up. Command two duty values well inside the range
// (0.10 and 0.60 are convenient), read the output with a DMM, and solve:
//
//     CAL_K  = (V_lo_duty - V_hi_duty) / (D_hi - D_lo)
//     CAL_V0 =  V_lo_duty + CAL_K * D_lo
//
// These are MEASURED, not computed. The design-notes nominals work out to
// V0 = 44.65 and K = 82.5; the bench figures below differ by 10 % and 26 %,
// which is a ~4 V error in the feed-forward jump if you trust the nominals.
// That gap is the whole reason this is a two-point measurement.
//
// Design notes S10 wants these in NVS eventually. Constants until then -
// reflashing to recalibrate is acceptable, silently running on stale flash
// after a hardware change is not.
static const float  CAL_V0 = 49.1f;    // V at D = 0
static const float  CAL_K  = 61.3f;    // V per unit duty

// ---- regulator tuning ----
// Ki is per 20 ms step, from the bench sweep: 0.25 critically damped,
// 0.5 rings, 1.5 unstable. Do not raise it without redoing that sweep.
static const int    REG_PERIOD_MS = 20;
static const float  REG_KI        = 0.25f;
static const int    REG_HOLD      = 5;       // 100 ms, ~10 tau of the ADJ filter

static const float  REG_TOL_FRAC  = 0.01f;   // the 1 % target from the notes
static const float  REG_TOL_ABS   = 0.15f;   // floor, so 2 V setpoints stay reachable
static const int    REG_SETTLE_N  = 3;
static const int    REG_MAX_STEPS = 150;     // 3 s of integrating without arriving

// Hard overvoltage backstop, design notes S7 item 8: the voltage control's
// failure direction needs one. Above this the loop is not trusted to correct
// itself - park at full sink immediately. Sits above the 45 V protocol
// maximum and below the CAL_V0 ceiling the hardware can actually reach.
static const float  V_TRIP = 47.0f;

// Boot state is full sink: output parked at 1.25 V, matching where the 10k
// pull-up holds it while the GPIO is still high-Z before setup() runs.
float    vsetDuty    = 1.0f;
bool     regSettled  = false;
bool     regFailed   = false;
int      regSettleHits = 0;
int      regSteps    = 0;
int      regHold     = 0;
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
//
// analogReadMilliVolts() is used deliberately in place of
// raw_counts * V_REF / 4096. It applies the ADC calibration burned into this
// individual die's eFuse, which is a per-chip measured reference rather than
// a board-level one - strictly better than any V_REF constant, and it also
// linearises the attenuator. Do NOT multiply by a separate measured V_REF on
// top of it: that double-corrects. VSENSE_GAIN below is the remaining trim,
// and it exists to correct the DIVIDER, not the reference.
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

void vsetWrite(float duty) {
  vsetDuty = constrain(duty, 0.0f, 1.0f);
  vsetApply((int)lrintf(vsetDuty * VSET_MAX));
}

// Feed-forward jump straight from the measured line. Lands within the
// calibration residual, so the integrator only trims - it never has to search
// the range, which is what keeps a full-scale change down to ~100 ms.
float dutyForVolts(float v) {
  return constrain((CAL_V0 - v) / CAL_K, 0.0f, 1.0f);
}

// Call whenever the target moves. Freezing the integrator for REG_HOLD
// periods matters: the RC filter lags ~60 ms, and integrating through that
// lag winds up and undershoots badly on a large jump.
void regRestart() {
  regSettled     = false;
  regFailed      = false;
  regSettleHits  = 0;
  regSteps       = 0;
  regHold        = REG_HOLD;
  vsetWrite(dutyForVolts(P.volts));
}

void serviceRegulator() {
  const float measured = readVolts();

  // Checked before anything else and regardless of hold or fault state. This
  // is the backstop, so it must not sit behind a guard that a fault could
  // disable.
  if (measured > V_TRIP) {
    vsetWrite(1.0f);
    if (!regFailed) {
      regFailed = true;
      enqueue("FAULT VOLT TRIP");
      Serial.printf("[reg] OVERVOLTAGE %.2f V > %.2f V trip - parked\n",
                    measured, V_TRIP);
    }
    return;
  }

  if (regFailed) return;                 // latched; a new SET PV clears it

  // The feed-forward jump has been applied but the RC filter has not caught
  // up. Reading now and integrating the difference is pure windup.
  if (regHold > 0) { regHold--; return; }

  const float err = P.volts - measured;
  const float tol = max(REG_TOL_ABS, REG_TOL_FRAC * P.volts);

  if (fabsf(err) <= tol) {
    // REG_MAX_STEPS has to bound one convergence attempt, not the lifetime of
    // the sketch. Without this reset, hours of slow thermal drift accumulate
    // single-count corrections until the counter trips and reports a fault on
    // a rail that is behaving perfectly.
    regSteps = 0;
    if (!regSettled && ++regSettleHits >= REG_SETTLE_N) {
      regSettled = true;
      Serial.printf("[reg] settled %.2f V at D=%.3f\n", measured, vsetDuty);
    }
    return;                              // deadband: do not chase ADC noise
  }

  regSettled    = false;
  regSettleHits = 0;

  // Negative slope, hence the minus: too high a reading must INCREASE duty,
  // which sinks more ADJ current and brings the output down. No minimum-step
  // guard is needed - at the REG_TOL_ABS floor the step is already ~2.5 PWM
  // counts, so the loop cannot stall on quantisation just outside tolerance.
  const float raw  = vsetDuty - REG_KI * err / CAL_K;
  const float next = constrain(raw, 0.0f, 1.0f);

  // Already at a clamp and the integrator still wants to push past it: the
  // setpoint is unreachable on this supply. Notes S3 - flag it rather than
  // chase it. The output sits at its best achievable value and the reading
  // agrees, so HOLD; parking would be an overreaction.
  if ((raw <= 0.0f && vsetDuty <= 0.0f) || (raw >= 1.0f && vsetDuty >= 1.0f)) {
    regFailed = true;
    enqueue("FAULT VOLT RAIL");
    Serial.printf("[reg] unreachable: want %.2f V, railed at D=%.3f, reading %.2f V\n",
                  P.volts, vsetDuty, measured);
    return;
  }

  vsetWrite(next);

  // Not railed, not tripped, but the reading will not come to the setpoint.
  // The loop is driving and the measurement is not responding, so the sense
  // path is suspect - open divider, dead op-amp, unseated wire. V_TRIP cannot
  // catch this: a divider reading LOW drives duty toward 0, which pushes the
  // real output UP toward CAL_V0 while the trip sees nothing. Park.
  if (++regSteps > REG_MAX_STEPS) {
    regFailed = true;
    vsetWrite(1.0f);
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
  vsetWrite(1.0f);

  Serial.begin(115200);
  delay(2000);
  Serial.println("\n[BLE] boot build=" __DATE__ " " __TIME__);

  analogReadResolution(12);
  analogSetPinAttenuation(VSENSE_PIN, ADC_11db);

  // Seed the loop at the default setpoint. The gating relay is unpowered and
  // therefore open, so the electrodes see nothing while this settles.
  regRestart();
  Serial.printf("[reg] cal V0=%.2f K=%.2f -> seed D=%.3f for %.1f V, trip %.1f V\n",
                CAL_V0, CAL_K, vsetDuty, P.volts, V_TRIP);

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
