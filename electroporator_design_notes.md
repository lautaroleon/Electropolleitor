# Electroporator Design Notes

In-ovo electroporator for chicken eggs. Reimplementation of a working legacy
device, replacing its manual controls with an Android app over BLE.

Status: hardware topology settled, BLE transport working end to end, pulse
train firmware written and awaiting bench verification.

---

## 1. Hardware

### Topology (final)

```
Mains -> 48 V AC/DC brick (external, pre-existing)
           |
           +-- R-78HB5.0-0.5 --> 5 V rail (+ 470 uF bulk) --> Nano ESP32
           |
           +-- LM317HV --> sense divider --> AGN2004H (both poles)
                             |                    |
                          bleeder            polarity switch (manual DPDT)
                                                  |
                                             electrodes
                                                  |
                                             1 ohm shunt --> GND
```

Grounds are **merged at a single point**. The 48 V rail is SELV and already
isolated from mains by the AC/DC brick, so the isolated DC/DC that was
originally specified (SSTW003A0A) bought nothing once the ESP32 had no wired
connection to the outside world. Wireless comms are what make this safe.

Caveat: reflashing over USB with the output connected to earthed equipment
re-creates the ground loop. Unplug the output, or use a USB isolator.

### Key part decisions

| Function | Part | Notes |
|---|---|---|
| 5 V rail | Recom R-78HB5.0-0.5 | 9-72 Vin, SIP-3, 78xx-compatible footprint |
| Regulator | LM317HV | TO-220; headroom is marginal at 45 V (see below) |
| Gating relay | Panasonic **AGN2004H** | single-side-stable, fails open |
| Polarity | manual DPDT switch | flips once per session; legacy behaviour |
| Sink transistor | 2N5551 (TO-92, 160 V) | BJT not MOSFET - see below |
| Op-amp | LM358 (PDIP-8) | one half = ADJ sink, other half = shunt amp |

Everything is through-hole or SIP so the design can be hand-built. That
constraint drove several choices away from otherwise-better SMD parts.

### Rejected during design

- **Isolated sensing** (isolation amp / opto / second MCU): the hidden cost is
  a low-voltage rail on the 48 V-return side, not the IC. Merging grounds
  removed the need entirely.
- **48 V relay coil**: irrelevant once the relay turned out to be pulse-driven
  or low-duty.
- **Digital potentiometer on ADJ**: the ADJ node sits at Vout - 1.25 V, beyond
  any digipot's rating.
- **MOSFET for the ADJ sink**: needs 3-4 V of gate overdrive stacked on
  V_sense, which an LM358 on 5 V cannot deliver. A BJT only asks for 0.7 V.
- **Switching regulator** for the main output: pulsed duty makes LM317
  dissipation a non-issue, and a switcher would put ripple on pulse amplitude.

---

## 2. Digital voltage control (LM317)

Instead of a potentiometer, steal current out of the ADJ node with a
ground-referenced sink. Only the transistor collector sees high voltage.

**V_out = 1.25 + R2 x (5.26 mA - V_filter / 330)**

```
  LM317 OUT --+-------------------+--> relay / sense divider
              |                   |
            240R (R1)         10k bleeder (1/2 W)
              |                   |
   ADJ -------+--+-----------+   GND
              |  |           |
              | 8k25 (R2)  2N5551 collector
              |  |           |
             GND GND     emitter --330R-- GND

  D1: 1N4148, anode ADJ -> cathode OUT

  GPIO --+-- 10k --+-- LM358 pin 3 (+)
         |         |
      10k to 3V3  1uF film
         |         |
        3V3       GND

  LM358: pin 2 (-) <- emitter,  pin 1 (out) -- 1k --> base
```

| Part | Value | Purpose |
|---|---|---|
| R1 | 240 R 1% | I_R1 = 1.25/240 = 5.21 mA |
| R2 | 8k25 1% | ceiling; = (V_max - 1.25) / 5.26 mA |
| R_sense | 330 R 1% | I_sink = V_filter / 330 |
| Filter | 10k + 1 uF film | tau = 10 ms |
| Pull-up | 10k to 3V3 | safe state when GPIO is high-Z |

**PWM as DAC**: 12-bit at 19.5 kHz (80 MHz / 4096 is forced). No DAC on the
ESP32-S3 - Espressif dropped it from the original ESP32. Two passives instead
of a DAC chip.

**Boot safety**: the pull-up sits *before* the filter's series resistor, so a
high-Z GPIO charges the filter to 3.3 V, demanding 10 mA against 5.26 mA
available. The transistor saturates, V_ADJ pins to 0, output parks at 1.25 V.
Deliberately past the edge of range so tolerances cannot walk it back.

**Do not add** an ADJ bypass capacitor - standard LM317 practice, but it fights
the current sink and slows the setpoint.

**Closed loop**: firmware sets PWM, reads the divider, iterates to within 1%.
This absorbs op-amp offset, base current, rail tolerance, and resistor
tolerance - so nothing in the set path needs to be accurate. Do not run the
loop during a pulse train; settle open-circuit, then fire.

Checks at 8k25: V_ADJ max 43.4 V (2N5551 has 3.7x margin), peak transistor
dissipation 57 mW at half-scale, ripple gain R2/R_sense = 25, resolution
21 mV over 2155 usable PWM counts.

### Sense divider

Two 47k in series over 5.6k. Ratio 17.79, so 45 V reads 2.53 V - top of the
ESP32-S3's usable linear range at 12 dB attenuation. 100 nF at the tap, 1k
series into the pin, plus a clamp.

### Correction made mid-design

R1 was briefly specified as 1k on the grounds of transistor dissipation. That
was wrong: peak power is I_sink x R2 x (I_full - I_sink), which maxes at
half-scale, giving 51 mW at 240 R - not the 230 mW claimed by multiplying
maximum current by maximum V_ADJ (those cannot coincide). 240 R also satisfies
the LM317's minimum load alone and cuts the ADJ pin's parasitic error from
3.8% to 1%.

---

## 3. Open hardware issues

### LM317HV headroom at 45 V

Dropout is 1.8-2.0 V typical, 2.5 V worst case. From 48.0 V that caps output
at ~45.5 V best case, and 45 V needs all of it. Add supply tolerance (a "48 V"
unit may sit at 47.0 V), sag, and ripple, and the regulator falls out of
regulation - passing input ripple straight to the electrodes.

**Fix**: trim the AC/DC brick's adjust trimmer to 51-52 V. At 52 V in and
1.25 V out the differential is 50.75 V against the HV part's 60 V limit.

If staying at 48 V, size R2 for the achievable ceiling (8k25 -> 44.65 V) and
have firmware flag non-convergence rather than chasing an unreachable setpoint.

Worth measuring what the legacy unit actually delivers at full scale.

### Relay contact life

- Electrical life is 10^5 operations at 1 A / 30 V DC resistive, tested at
  **20 cpm**. Actual duty is 105-270 ms per cycle = **222-571 cpm**, i.e.
  11-28x the test rate. Contacts get less recovery time between arcs, so real
  life lands below 10^5.
- GN-series contacts are gold-plated AgPd - signal-relay contacts, ~1 A limit
  with a DC switching-power ceiling near 30 W. At 45 V that is well under 1 A.
- DC arcs do not self-extinguish. Characteristic end-of-life failure is
  **welding closed**.
- At 5 pulses x 100 eggs = ~500 operations/session, that's a few hundred
  sessions.

**Unresolved question**: how often does the legacy unit need its relay
replaced? "Never in ten years" means switched current is low enough that arc
erosion is not real - copy the original. "It's a consumable" confirms the wear
model and justifies the series MOSFET below.

### Series MOSFET (proposed, not accepted)

Relay and MOSFET in **series** in the return path. The relay keeps its legacy
role as the connect/disconnect element and still gives a true galvanic open;
the FET handles pulse edges. Sequence: relay closes with FET off, FET gates the
train, relay opens with FET off.

- Relay only ever switches cold: 10^5 electrical -> 10^8 mechanical
- Two independent series elements, so a shorted FET still cannot conduct
  through an open relay, and a welded relay cannot conduct through an off FET
- Also fixes the 10 ms minimum pulse width, where 4 ms of relay operate time
  is ~40% of the pulse

Deferred: the user wants to stay faithful to the legacy front end for this
iteration.

### Shunt placement

The shunt goes in the **return conductor feeding the polarity switch**, at the
ground end - upstream of the crossover, where the two conductors still have
fixed identity. Downstream of a crossover there is no permanent return.

Current flows load-to-ground through it in the same direction regardless of
switch position, so one shunt covers both polarities and the signal is
single-ended. No differential amplifier needed.

Keep it at 1 ohm. Its drop subtracts from what the electrodes see, and the
voltage loop settles with the relay open, so any burden appears as uncorrected
pulse-time sag. 1 ohm at 200 mA is ~200 mV on 30 V (<1%); 10 ohm would put 2 V
of uncorrectable error in the pulse.

**Grounding detail**: return the LM317 ground, R2, the bleeder, and the sense
divider's bottom leg to the **ground side** of the shunt. Anything on the load
side puts its quiescent current through the shunt as a standing offset.

Payoff: integrate I dt for delivered charge (tracks transfection efficiency
better than commanded voltage), detect dry electrodes or bubbles before
committing an egg, and detect a welded contact by reading current when the
relay should be open.

### RF

RSSI of -90 dBm at 50 cm is 25-40 dB below a healthy peripheral. The NORA-W106
embeds its antenna in the module, so it is very sensitive to nearby copper and
metal - a breadboard's contact strips sit directly underneath.

Diagnosis order: (1) compare against a reference BLE device at the same
distance using nRF Connect - isolates phone vs board; (2) free-air test with
the board out of the breadboard, held by the USB end.

TX power buys ~10 dB at most and only in the board-to-phone direction:

```cpp
esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV,     ESP_PWR_LVL_P9);
```

The `ADV` line is what changes the RSSI seen during scanning.

**For the final PCB**: no copper at all under and around the module's antenna
end; overhang the board edge if possible. Plastic enclosure near the antenna.
If the enclosure must be metal, switch to a U.FL module with an external
antenna.

---

## 4. Pulse specification

| Parameter | Range | Protocol key |
|---|---|---|
| Pulse count | 2 - 7 | `NP` |
| Pulse duration | 10 - 70 ms | `PW` |
| Pulse spacing | 25 - 200 ms | `PG` |
| Voltage | 2 - 45 V | `PV` |

Polarity: flipped rarely, manually, before a session.

Note the ranges widened from an earlier statement of 30-70 ms and 75-200 ms.
The 10 ms lower bound is the problematic one - 4 ms of relay operate time plus
bounce is roughly half the pulse, and it will not be reproducible. If 10 ms is
a real requirement rather than headroom, that argues for the series MOSFET.

---

## 5. BLE architecture

**The ESP32-S3 has no Bluetooth Classic** - BLE 5.0 only. `BluetoothSerial.h`
does not compile. This invalidates most ESP32+Android tutorials, which assume
the original ESP32.

Transport is the **Nordic UART Service**, the de facto serial-over-BLE
substitute. Practical benefit: generic BLE terminals (nRF Connect) recognise
the UUIDs, so firmware can be proven before writing any app code.

```
Service  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
RX       6E400002-...   app -> device, write
TX       6E400003-...   device -> app, notify
```

Device name `EPORATOR01`. At 10 characters it lives in the scan response, not
the primary advertising packet - a 128-bit service UUID eats 18 of the 31
bytes. `setScanResponse(true)` handles this; Android merges scan-response data
into discovery results.

### Measured link characteristics

- MTU negotiates to **517** (Qt requests it during discovery)
- Connection interval settles at **36-40 units (45-50 ms)** after Android
  relaxes from 7.5 ms during discovery
- Supervision timeout 500 ms

**Architectural consequence**: round-trip latency is ~100 ms and not under app
control. The app cannot time anything. It sends parameters, the MCU executes
the train with local hardware timing, the MCU reports results. This is an
independent reason (alongside WiFi-stack stalls) that pulse timing must live
in firmware.

`pAdv->setMinPreferred(0x06)` was removed - it requests a 7.5 ms interval,
gains nothing here, and adds fragility against a 500 ms supervision timeout.

---

## 6. Protocol

Newline-terminated ASCII. Debuggable from nRF Connect.

| Direction | Message | Meaning |
|---|---|---|
| app -> mcu | `SET NP 5` | pulse count |
| app -> mcu | `SET PW 50` | pulse width, ms |
| app -> mcu | `SET PG 120` | gap, ms |
| app -> mcu | `SET PV 45.0` | voltage setpoint |
| app -> mcu | `GET ALL` | resend all current values |
| app -> mcu | `FIRE` | run train with current parameters |
| app -> mcu | `ABORT` | stop mid-train |
| mcu -> app | `ACK NP 5` | accepted, with the value actually stored |
| mcu -> app | `NAK NP` | rejected (out of range) |
| mcu -> app | `NAK BUSY` | rejected (train running) |
| mcu -> app | `ACK FIRE` | train starting |
| mcu -> app | `NAK FIRE BUSY` / `NAK FIRE VOLT` | refused, with reason |
| mcu -> app | `PULSE 3/5` | progress |
| mcu -> app | `DONE 5` / `DONE ABORT` | train finished |
| mcu -> app | `MV 44.83` | measured volts, streamed at 4 Hz |

### Central principle

**The GUI displays what the device confirmed, not what the user typed.**

User types -> app sends -> firmware clamps and replies with the accepted value
-> app updates the LCD from the reply.

Consequences: range validation lives in the one place that matters; a dropped
BLE write cannot leave the GUI lying about device state; reconnect + `GET ALL`
resyncs the whole display.

`MV` is excluded from the debug log and the UI log list - at 4 Hz it would
flood both. It also replaces the old heartbeat as the liveness indicator.

**Line framing is required on both sides.** BLE writes and notifications are
not line-aligned: a long reply can split across packets, and two lines can
arrive in one. Both ends buffer and split on `\n`.

---

## 7. Firmware structure

### Task model

| Task | Core | Priority | Role |
|---|---|---|---|
| Bluedroid stack | 0 | - | BLE, runs RX callbacks |
| Arduino `loop()` | 1 | 1 | MV sampling, TX queue drain |
| `pulseTask` | 1 | 10 | gate timing, created per train |

**Only `loop()` may call `txLine()`.** Everything else calls `enqueue()`. This
was learned the hard way: `sendAll()` firing four `notify()` calls back-to-back
from inside the BLE write callback congested the Bluedroid stack. TX is paced
at 25 ms, above the 45-50 ms connection interval.

**The TX queue is a FreeRTOS queue, not a ring buffer.** Three tasks enqueue
(BLE callback, `loop()`, `pulseTask`), and a plain head/tail ring races between
them. `xQueueSend` with a 0 timeout drops rather than blocks.

### Pulse timing

```cpp
static void waitUs(uint32_t us) {
  const int64_t target = esp_timer_get_time() + (int64_t)us;
  if (us > 3000) vTaskDelay(pdMS_TO_TICKS((us - 2000) / 1000));
  while (esp_timer_get_time() < target) { /* spin */ }
}
```

Sleeps for the bulk, spins only the final <2 ms. Microsecond-accurate edges
without starving the idle task. `vTaskDelay` alone gives 1 ms tick granularity
= 10% error on a 10 ms pulse.

Reality check: the relay's 4 ms operate time plus bounce dominates. Precision
beyond ~1 ms is wasted.

**Parameters are snapshotted at fire time** so a `SET` arriving mid-train
cannot change the width halfway through. Firmware also refuses `SET` while
running.

**Gate pin**: `D2`. Avoid strapping pins (GPIO0/3/45/46).

### Safety

1. **External 10k pulldown from D2 to ground - mandatory.** ESP32 pins are
   high-impedance inputs from reset until `pinMode()` runs, and a floating
   MOSFET gate can partially conduct. `digitalWrite(LOW)` first in `setup()`
   only shortens the window; the resistor is what guarantees it.
2. **AC-coupled gate with an RC timeout** bounds on-time in hardware. Normal
   pulses pass; a stuck GPIO cannot hold conduction. Two passives.
3. `digitalWrite(GATE_PIN, LOW)` unconditionally at the end of `pulseTask`,
   outside the loop.
4. Firmware refuses a second `FIRE` while a train is running.
5. `CHECK_VOLTAGE_BEFORE_FIRE` compares measured against setpoint - currently
   compiled out until the sense divider is wired.
6. A train in flight is allowed to finish on BLE disconnect. It is timed
   locally, and stopping mid-train would leave a partial dose.
7. Single-side-stable relay means power loss or a hung MCU opens the output.
   **Never use a latching relay in the gating path** - it holds state through a
   crash, leaving continuous DC in the embryo.
8. Firmware should also drop the relay if the divider reads above a threshold,
   since the voltage control's failure direction needs a backstop.

---

## 8. Qt app

Qt 6.11, **qmake**, Widgets. Android target, arm64-v8a.

```
QT += core gui widgets bluetooth
SOURCES += main.cpp mainwindow.cpp blescanner.cpp blelink.cpp scanpage.cpp
HEADERS += mainwindow.h blescanner.h blelink.h scanpage.h
FORMS   += mainwindow.ui scanpage.ui
```

### Classes

| Class | Role |
|---|---|
| `BleScanner` | discovery, name-prefix filter `"EPORATOR"`, dedup |
| `BleLink` | `QLowEnergyController` central, GATT, line framing |
| `ScanPage` | full-screen device picker (own .ui), façade API only |
| `MainWindow` | `QStackedWidget` of main page + scan page |

`QMainWindow::takeCentralWidget()` wraps the existing `mainwindow.ui` as page 0
without destroying it, so every `ui->` pointer and the `connectSlotsByName`
auto-connections survive reparenting. This avoided restructuring the form in
Designer.

`ScanPage` exposes methods and signals rather than its widgets, so
`MainWindow` has no dependency on the picker's internals.

### Flow

```
btnConnect -> scan page -> filtered device list -> tap
  -> stop scan -> connectTo() -> discovery -> CCCD write -> ready()
  -> back to main page -> GET ALL -> LCDs populate
```

`onLinkReady` delays `GET ALL` by 300 ms so it does not race the in-flight CCCD
write. A 12 s watchdog covers a connection that never completes.

### Qt/Android gotchas encountered

- **Qt Bluetooth is in the optional "Qt Connectivity" add-on**, installed
  per-target-ABI. Installing it for Desktop leaves Android failing with the
  identical `Unknown module(s) in QT: bluetooth`. Verify
  `<QtRoot>/6.11.x/android_arm64_v8a/mkspecs/modules/qt_lib_bluetooth.pri`
  exists.
- Android 12+ needs `BLUETOOTH_SCAN` (with `neverForLocation`) and
  `BLUETOOTH_CONNECT` in the manifest **and** a runtime request via
  `QBluetoothPermission`. Manifest alone returns an empty device list with no
  error.
- Scanning must start **inside** the permission callback - `requestPermission`
  is async.
- `QBluetoothDeviceDiscoveryAgent::LowEnergyMethod` explicitly; the default
  also runs Classic discovery, which cannot see a BLE-only peripheral.
- Android re-reports a device on every advertising packet - dedup or the list
  fills with duplicates.
- Android throttles ~5 scan starts per 30 s, then returns empty with no error.
  Manual rescan button, not auto-retry.
- `createServiceObject()` returning non-null does **not** mean characteristics
  exist. They are empty until `discoverDetails()` completes and state reaches
  `RemoteServiceDiscovered`.
- `qDebug()` on Android goes through logcat as `D/default`, not coloured
  stderr. Filter on `default`.
- `QInputDialog`'s static helpers cannot be styled. Construct the dialog
  explicitly. Size comes from `min-height` on children - Qt on Android often
  ignores dialog geometry.
- Forward-declare Qt classes inside `QT_BEGIN_NAMESPACE` / `QT_END_NAMESPACE`,
  or just include the header.
- Peripherals serve one central at a time - close nRF Connect before testing
  the app.

### Bugs found and fixed

| Symptom | Cause |
|---|---|
| `conversion from std::string to String` | core 2.x returns `std::string`; use `auto` |
| Duplicate symbol `ScanPage::ScanPage` | `scanpage.cpp` compiled twice |
| Disconnect one write after `ready` | `cleanup()` left inside `send()`, destroying the controller |
| No numpad on parameter buttons | `askAndSend`'s `isReady()` guard returned early - link already dead |
| `readVolts()` never called | MV block pasted **inside** `readVolts()`, recursing; `loop()` had no producer |
| No `MV` output at all | `sendAll()` still called `txLine()`, bypassing the queue |
| Parameter buttons never enable | state set once in constructor, never updated |
| List rows connect to wrong device | log lines appended to `listDevices`, breaking the row -> `results()` index map |

The disconnect bug was the expensive one. Android logged `cancelOpen()` with
`status=0` - GATT_SUCCESS, a deliberate close, not a timeout (status=8) or RF
loss (status=19/22). That distinction pointed at the app rather than the MCU
after several rounds spent on a wrong notify-congestion theory.

---

## 9. Debugging lessons

- **Bisect with a third-party tool before instrumenting.** nRF Connect proves
  or clears the firmware in 30 seconds. Several rounds were spent adding ESP32
  probes when this would have halved the search space immediately.
- **Read the GATT status code.** `status=0` vs `8` vs `19` distinguishes
  deliberate close, supervision timeout, and RF loss.
- **Put a build marker in `setup()`**: `__DATE__ " " __TIME__`. Removes upload
  doubt permanently.
- Treat "transition into state" signals as at-least-once, not exactly-once.
- Serial monitor timestamps distinguish "paced through a queue" from "burst
  from a callback".
- Enable/disable controls rather than silently early-returning; a silent guard
  looks like a broken feature.

---

## 10. Open items

**Hardware**
- [ ] Bench-verify the ADJ current sink on protoboard (sweep, not single point)
- [ ] Decide the 48 V vs 52 V rail trim; set R2 accordingly
- [ ] Measure electrode + albumen load resistance at working voltage
- [ ] Check that measured current is inside the AGN2004H's DC switching curve
- [ ] Fit the D2 pulldown and the AC-coupled gate timeout
- [ ] Wire the sense divider, then enable `CHECK_VOLTAGE_BEFORE_FIRE`
- [ ] Fix the RF environment; verify against a reference device
- [ ] **Ask: how often does the legacy unit need its relay replaced?**

**Firmware**
- [ ] Two-point ADC calibration to NVS
- [ ] Closed voltage loop against the divider
- [ ] Shunt current on the LM358's second half; integrate for delivered charge
- [ ] Persist parameters across power cycles

**App**
- [ ] Separate display for voltage setpoint vs measured (commanded vs actual
      is the first indicator of bad electrode contact)
- [ ] Restore the disconnect branch on `btnConnect`
- [ ] Consider a full-screen numeric keypad instead of `QInputDialog`

**Validation**
- [ ] LED on D2, count blinks
- [ ] Scope D2 for width and spacing
- [ ] Relay only, no electrodes - count clicks
- [ ] Dummy resistive load, compare waveform against the legacy unit
- [ ] Only then, electrodes and an egg

Calibrate against the legacy device, not against firmware numbers: scope both
into the same dummy load at the same nominal setting and match the *delivered*
waveform.
