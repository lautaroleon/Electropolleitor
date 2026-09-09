<div align="center">

<img src="title.png" alt="Electropolleitor" width="640">

**Electroporator for chicken embryo — Android app, BLE link, ESP32 firmware.**

</div>

---

## What this is

A reimplementation of a working legacy electroporator built for a biology lab.
The original is a good machine with one limitation: every parameter is set by
hand on the front panel. This project keeps the legacy analogue front end —
`LM317` regulator, mechanical relay, manual polarity switch — and replaces the
manual controls with an Android app talking to an ESP32 over Bluetooth Low
Energy.

The design goal throughout was **faithfulness to the original's electrical
behaviour**, not novelty. Where a modern part would have been better but would
have changed the delivered waveform, the legacy choice won.

> [!WARNING]
> This drives up to 45 V into electrodes placed in a living embryo. It is lab
> equipment, not a toy. Read [`electroporator_design_notes.md`](electroporator_design_notes.md)
> §7 *Safety* before wiring anything, and never substitute a latching relay
> into the gating path — it holds state through a crash, which leaves
> continuous DC in the embryo.

---

## Repository layout

| Path | What it is |
|---|---|
| `*.cpp` `*.h` `*.ui` `*.pro` | Qt 6 Widgets app (Android target, arm64-v8a) |
| `firmware/ble_link_test/` | Step 0 — NUS echo + heartbeat, for proving the link |
| `firmware/ble_params/` | Step 1 — parameter protocol + voltage telemetry |
| `resources.qrc`, `*.png` | Title art and the five reaction-chicken states |
| `electroporator_design_notes.md` | **The real documentation.** Hardware, firmware, protocol, debugging lessons |
| `electroporator_design_review.md` | Architecture review that preceded the build |

The Qt project file is `Electoporator.pro` — the spelling predates the
`Electropolleitor` name and is left alone so no build path breaks.

---

## Hardware

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

Grounds are merged at a single point. The 48 V rail is SELV and already
isolated from mains by the AC/DC brick, so the isolated DC/DC originally
specified bought nothing once the ESP32 had no wired connection to the outside
world — **the wireless link is what makes this safe**.

> [!CAUTION]
> Reflashing over USB with the output connected to earthed equipment re-creates
> the ground loop that isolation was protecting you from. Unplug the output, or
> use a USB isolator.

Output voltage is set digitally without a potentiometer: a ground-referenced
`2N5551` steals current from the `LM317`'s ADJ node, driven by half an `LM358`
against a 12-bit PWM-as-DAC. Only the transistor's collector sees high voltage.
Full derivation, part values and the boot-safety argument are in the design
notes, §2.

---

## Protocol

Newline-terminated ASCII over the Nordic UART Service, so any generic BLE
terminal (nRF Connect, LightBlue) can drive the device before the app is
involved. That property is deliberate and worth preserving — it bisects
app-versus-firmware bugs in about thirty seconds.

```
Service  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
RX       6E400002-...   app -> device, write
TX       6E400003-...   device -> app, notify
Name     EPORATOR01
```

| Direction | Message | Meaning |
|---|---|---|
| app → mcu | `SET NP 5` | pulse count, 2–7 |
| app → mcu | `SET PW 50` | pulse width, 10–70 ms |
| app → mcu | `SET PG 120` | gap, 25–200 ms |
| app → mcu | `SET PV 45.0` | voltage setpoint, 2–45 V |
| app → mcu | `GET ALL` | resend all current values |
| app → mcu | `FIRE` / `ABORT` | run / stop a pulse train |
| mcu → app | `ACK NP 5` | accepted, with the value actually stored |
| mcu → app | `NAK NP` | rejected (out of range) |
| mcu → app | `PULSE 3/5` | progress |
| mcu → app | `DONE 5` / `DONE ABORT` | train finished |
| mcu → app | `MV 44.83` | measured volts, streamed at 4 Hz |

**The GUI displays what the device confirmed, not what the user typed.** The
app sends a `SET`, the firmware clamps it and replies with the value it
actually stored, and only then does the readout change. Range validation lives
in exactly one place, a dropped BLE write cannot leave the display lying about
device state, and reconnect + `GET ALL` resyncs everything.

Both ends buffer and split on `\n`. BLE writes and notifications are not
line-aligned — a long reply can split across packets, and two lines can arrive
in one.

---

## Building the app

Requires **Qt 6.11** with the **Qt Connectivity** add-on, qmake, and the
Android arm64-v8a kit.

```bash
qmake Electoporator.pro
make            # or build from Qt Creator with the Android kit selected
```

Three things that will cost you an afternoon if you don't know them:

- **Qt Bluetooth is an optional add-on, installed per target ABI.** Installing
  it for Desktop leaves Android failing with an identical
  `Unknown module(s) in QT: bluetooth`. Verify that
  `<QtRoot>/6.11.x/android_arm64_v8a/mkspecs/modules/qt_lib_bluetooth.pri`
  actually exists.
- **Android 12+ needs `BLUETOOTH_SCAN` (with `neverForLocation`) and
  `BLUETOOTH_CONNECT`** in the manifest *and* a runtime request via
  `QBluetoothPermission`. Manifest alone returns an empty device list with no
  error at all.
- **Scanning must start inside the permission callback.** `requestPermission`
  is asynchronous.

The ESP32-S3 has no Bluetooth Classic — BLE 5.0 only. `BluetoothSerial.h` does
not compile, which invalidates most ESP32-plus-Android tutorials you will find.

## Flashing the firmware

Arduino IDE, board **Arduino Nano ESP32**, arduino-esp32 core 2.x. Open either
sketch under `firmware/` and upload. Start with `ble_link_test` and confirm the
echo and heartbeat from nRF Connect before touching the app.

Every sketch prints `__DATE__ " " __TIME__` in `setup()`. Keep that habit — it
removes "did the upload actually take?" from the list of things a bug could be.

---

## Status

**Working**

- BLE transport end to end, app ↔ firmware
- Parameter set/get with firmware-side clamping and ACK echo
- Voltage telemetry at 4 Hz, driving a settle detector and the arming logic
- Full app UI: device picker, parameter entry, state machine, reaction chicken

**Written, not yet bench-verified**

- Pulse-train timing (`pulseTask`, microsecond-accurate edges) — designed and
  documented in §7, *not present in `firmware/` yet*. The app already speaks
  `FIRE`/`PULSE`/`DONE`/`ABORT`; the committed sketches do not answer them.
- Closed-loop voltage control — implemented in `ble_params.ino`, verified only
  against a simulated plant. No protoboard sweep run, and **`VSENSE_GAIN` /
  `VSENSE_OFFSET` are inert placeholders until a two-point calibration is
  done against a trusted meter.** Until then the reported voltage is only as
  good as the nominal 17.79 divider ratio.

**Open questions**

- `LM317HV` headroom at 45 V is marginal from a 48 V rail; the rail may need
  trimming to 51–52 V
- Relay contact life at the real duty cycle (222–571 cpm against a 20 cpm test
  rating) — hinges on how often the legacy unit actually needs its relay
  replaced
- RF: −90 dBm at 50 cm, likely breadboard copper under the module's embedded
  antenna

The full checklist lives in design notes §10. Validation order is deliberate
and should not be shortcut: LED on the gate pin, then a scope, then the relay
alone, then a dummy resistive load compared against the legacy unit — and only
then, electrodes and an egg.

---

## Calibration

Calibrate against the legacy device, not against firmware numbers. Scope both
units into the same dummy load at the same nominal setting and match the
*delivered* waveform.
