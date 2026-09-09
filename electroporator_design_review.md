# Electroporator Design Review — Summary

**Context:** Reviewing an existing electroporator design (built by a retired engineer) for a biology lab, driving electrodes connected to an egg, and evaluating whether to redesign or extend it.

## Original design
- 48V external power supply
- Off-the-shelf isolated DC/DC converter → 5V for digital electronics
- Microcontroller-modulated pulses (hundreds of ms) via a relay
- Relay (Panasonic **AGN2104H**, DPDT, single-coil latching) switches an **LM317** output to the electrodes, and reverses polarity
- LM317 output adjustable 2V–45V

## 1. Is the original architecture sound?
**Conclusion: yes, largely.**
- LM317 linear regulation is appropriate — absolute power dissipation is low at the expected currents, and linear regulation avoids switching noise/light-load instability that a buck converter would risk into a variable-impedance load.
- Mechanical relay beats solid-state (SSR/MOSFET) for this application because:
  - Near-zero leakage in the open state (matters most in the *fault* case — poor electrode contact — where impedance spikes)
  - Trivial polarity reversal (DPDT contacts) vs. an H-bridge with floating high-side gate drive for solid-state
  - True galvanic isolation when off
- Research on egg/in-ovo electroporation showed **normal-contact tissue impedance is moderate (tens–hundreds of Ω)**, not extremely high — this shifted the relay's main value from "leakage protection" to "fault-condition/dry-contact protection."
- The 2–45V range and hundreds-of-ms pulse timing match published in-ovo electroporation protocols, suggesting the original values were literature-informed, not arbitrary.

## 2. Alternative output stage: DDS → LF356 → OPA445 + boost converter
Evaluated as a way to get arbitrary waveforms (not just square pulses). Real engineering risks identified:
- OPA445 output current (15 mA typical, ~26 mA short-circuit) is likely insufficient for the load current implied by moderate egg impedance at 45V — would need an external current-boost stage (TI app notes confirm this is a standard fix, up to ~1A)
- Generating dual ±45V rails from a single boost converter requires a second boost/inverting stage or a dual-output flyback
- Switching noise from the boost converter would ride directly on the amplifier's supply rails and needs filtering
- Stability with a load that may have parasitic capacitance (egg/tissue) needs bench verification
- **Worth it only if arbitrary waveform shapes (exponential decay, ramps, multi-step poring/transfer pulses) are actually needed biologically** — otherwise it's added complexity without benefit over the relay+LM317 approach

## 3. Digitally controlling the LM317 output voltage
Decision: **keep the LM317 + relay, add DAC control of the output voltage** rather than redesigning the power stage.

### Chosen approach: closed-loop servo around the ADJ pin
- LM317 ADJ pin floats near `Vout − 1.25V` (up to ~44V) — any control circuit touching it must tolerate that swing
- Circuit: a small-signal high-voltage NPN transistor sinks current from ADJ to ground; a **low-voltage op-amp** compares a **sense divider** (tapping ADJ, scaled down) against the **MCU DAC** target and drives the NPN's base
- Only the NPN's collector needs high-voltage tolerance — the op-amp and DAC stay ordinary, ground-referenced, low-voltage parts
- Closed-loop design means output tracks the DAC accurately without needing to characterize/calibrate the LM317's Iadj drift or resistor tolerances
- Outer loop should be deliberately slow (settling in ms) relative to the LM317's own internal loop (tens of kHz) for stability — acceptable since the DAC only needs to settle before the relay fires each pulse

### Alternatives considered and set aside
- **Open-loop DAC-driven NPN sink** (no op-amp) — works, but needs firmware calibration to correct for part tolerances
- **LT3080-family regulator** (ground-referenced SET pin, natively DAC-friendly) — attractive but only within its ~36V native rating; extending it to 45V requires floating it behind an external HV MOSFET pass device, which reintroduces the same floating-control-node problem the LM317 solution already solves. LM317's lack of a hard ground pin makes it *better* suited to this kind of extension than parts like the LT3080.

### References for the DAC-controlled LM317 technique
- TI LM317 datasheet, Application Information section (shows the same NPN-sink-from-ADJ mechanism): https://www.ti.com/lit/ds/symlink/lm317.pdf
- TI E2E: "LM317 dynamic voltage adjustment with DAC" — https://e2e.ti.com/support/power-management-group/power-management/f/power-management-forum/1127399/lm317-dynamic-voltage-adjustment-with-dac
- TI E2E: "Help with programmable LM317 circuit problem" (closest built/debugged match) — https://e2e.ti.com/support/power-management-group/power-management/f/power-management-forum/263898/help-with-programmable-lm317-circuit-problem
- All About Circuits forum thread (Arduino DAC control, notes on filter latency/stability) — https://forum.allaboutcircuits.com/threads/digital-controlled-lm317-dac-with-arduino.190480/
- Electro-Tech-Online thread (grounding/minimum-load discussion) — https://www.electro-tech-online.com/threads/lm317-digital-control-dac.164254/
- EDN: "PWM power DAC incorporates an LM317" (different topology, useful ripple-cancellation background) — https://www.edn.com/pwm-power-dac-incorporates-an-lm317/

### Tools for simulating/drawing the schematic
- **KiCad** + built-in **ngspice** (free)
- **LTspice** (free) + TI's LM317 SPICE macromodel
- TI WEBENCH / PSpice-for-TI

## 4. Relay coil drive
- AGN2104H is a **single-coil latching relay** — genuinely needs current in one direction to set, the opposite direction to reset
- The schematic's single transistor works via a **capacitor-in-series-with-the-coil** technique (confirmed against TE/Axicom's latching relay application note):
  - Transistor ON → initially-uncharged capacitor acts as a near-short, driving a strong SET current pulse; current decays to ~0 as the cap charges (near-zero standing current once latched)
  - Transistor OFF → the charged capacitor discharges back through the coil via a resistor/diode return path, driving a **reverse-polarity** current pulse that RESETs the relay automatically
  - The polarity reversal comes from the capacitor's charge/discharge cycle, not from switching supply rails — no H-bridge needed
- Confirm in the schematic: a capacitor near the coil, and a diode/resistor discharge return path
- Check RC timing against the relay's minimum pulse-width spec (≥5× the datasheet set/reset time)

### BJT vs. MOSFET for the coil driver
Both work fine — coil current (~22 mA nominal) is well within either device's rating.
- **2N2222 (BJT):** needs a base resistor sized for `Ic/hFE` (trivial at this current); more forgiving, minimal drive-level thought needed
- **MOSFET:** no static gate current, but must be a **logic-level** part if driven from a 3.3V MCU pin (e.g., BSS138, AO3400) — generic small-signal MOSFETs like the 2N7000 may not fully enhance at 3.3V
- Either way, verify the flyback/discharge diode also protects the switch device's Vce/Vds from turn-off transients

## 5. R1/R2/potentiometer sizing for 2–45V output
Using `Vout = 1.25V × (1 + R2/R1)`, with **R1 = 270Ω** (sets divider current ~4.6 mA, above the LM317's minimum load spec — no separate bleeder resistor needed):

| Element | Value | Purpose |
|---|---|---|
| R1 | 270Ω (fixed) | Sets divider bias current |
| R2 (fixed) | 160Ω | Sets the 2V floor |
| Potentiometer | 10kΩ | Sweeps up to ~45V (lands at ~93% of travel, leaving margin) |

**Wiring/part notes:**
- Wire the pot as a rheostat (wiper tied to one end) so a lost wiper contact fails toward *added* resistance (lower voltage), not toward max voltage
- Use a 10-turn precision pot for fine, repeatable control across the full range
- Suggested part numbers:
  - Front-panel operator control: **Bourns 3590S-2-103L** (10kΩ, 10-turn, panel-mount), often paired with a **Bourns H-22-6T** turns-counting dial
  - Internal/factory calibration trimmer: **Bourns 3296W-1-103LF** (10kΩ, 10-turn, PCB-mount cermet)
- Consider adding the LM317 datasheet's OUT–ADJ and ADJ–IN protection diodes if not already present, given the output connects to biological tissue and could see a sudden discharge/short fault
