# Hardware Reference

---

## Status

This document tracks two design revisions:

- **Rev A (baseline)** — the hardware set reflected in the current source tree:
  BME280 + ADS1115 + MIKROE-2815 PT100s + DS3231 RTC, one stepper motor.
- **Rev B (in review, April 2026)** — proposed replacement parts list that adds
  an RS485 bus, swaps parts of the ambient/RTD stack, and adds a second
  stepper motor. Section [Rev B parts list](#rev-b-parts-list-in-review) captures
  the vendor links, per-part compatibility notes, open questions, and
  architectural impact. Source still implements Rev A; the Rev B plan is the
  next implementation milestone.

---

## System Overview (Rev A — current implementation)

COATHEAL runs on a **Raspberry Pi 4** and interfaces with the following hardware:

| Subsystem | Component | Interface | Status |
|---|---|---|---|
| Ambient environment | BME280 | I2C | Driver pending |
| Sample temperatures | MIKROE-2815 × N (PT100 RTD) | SPI | Driver pending |
| UV irradiance | BPW21 photodiode via ADS1115 ADC | I2C | Driver pending |
| Real-time clock | External RTC (DS3231 or similar) | I2C | Driver pending |
| Sample heaters (×9) | Resistive heaters via MOSFET | GPIO PWM | Pin mapping pending |
| Electronics heater (×1) | Resistive heater via MOSFET | GPIO PWM | Pin mapping pending |
| Heartbeat status LED | Discrete LED + series resistor | GPIO (libgpiod) | BCM 17 (`hal.status_led_line`) |
| Mode indicator LED | Discrete LED + series resistor | GPIO (libgpiod) | BCM 27 (`hal.mode_led_line`) |
| Sample-bend stepper | NEMA-17 via STEP/DIR/EN driver | GPIO (libgpiod) | Driver IC TBD |

---

## Sensors (Rev A)

### BME280 — Ambient Environment (I2C)

Measures ambient temperature, pressure, and humidity inside the gondola (outside the sample enclosure).

| Parameter | Description |
|---|---|
| Temperature range | −40 °C to +85 °C |
| Pressure range | 300–1100 hPa (3–11 mbar) |
| Interface | I2C (address 0x76 or 0x77) |
| HAL class | `I2cAdapter` |

**Driver status:** Stub only. `I2cAdapter` tracks a `healthy_` flag but does not perform real I2C transactions. The BME280 driver must be written to read temperature, pressure, and humidity registers and populate `SensorSnapshot`.

### MIKROE-2815 — PT100 RTD Temperature (SPI)

One MIKROE-2815 click board per sample heater zone. Each board reads one PT100 resistance temperature detector (RTD) and returns a temperature value via SPI. Internally this is a MAX31865 — the **Adafruit #2711** PT100 amplifier breakout uses the same chip and is driver-compatible.

| Parameter | Description |
|---|---|
| Temperature range | −200 °C to +850 °C |
| Resolution | ~0.03125 °C (15-bit) |
| Interface | SPI (one CS line per board) |
| HAL class | `SpiAdapter` |

**Driver status:** Stub only. `SpiAdapter` tracks a `healthy_` flag but does not perform real SPI transactions. A driver must be written to issue the correct SPI read sequence to each MIKROE-2815 board and convert the returned raw value to °C.

**Note:** The number of PT100 channels determines `hardware.heater_count - 1` (9 sample sensors for 9 sample heaters). Fewer boards can be used — set `hardware.heater_count` accordingly (minimum recommended: 5, including the electronics heater).

### ADS1115 — UV Sensor ADC (I2C)

The BPW21 photodiode output is read via an ADS1115 16-bit ADC over I2C. The ADC result is normalized to a float in `SensorSnapshot.uv`.

| Parameter | Description |
|---|---|
| Resolution | 16-bit |
| Interface | I2C (address 0x48–0x4B, configurable) |
| HAL class | `I2cAdapter` |

**Driver status:** Pending. Shares the `I2cAdapter` instance with the BME280. The ADS1115 driver must configure the multiplexer and gain, trigger a conversion, read the result, and normalize to a 0–1 float.

### External RTC (I2C)

An external real-time clock (**DS3231** required — see Rev B note below) provides accurate UTC time even after power cycling.

| Parameter | Description |
|---|---|
| Interface | I2C |
| HAL class | `RtcAdapter` |

**Driver status:** `RtcAdapter` currently returns the system clock time (`std::chrono::system_clock`) and sets `rtc_valid = false`. A real driver must read the RTC registers, set `rtc_valid = true`, and synchronize the system clock on startup.

---

## Heater Channels

### Overview

| Channel | Index | Type | Controller |
|---|---|---|---|
| Sample heater 0 | 0 | Resistive (plate) | Sample PID |
| Sample heater 1 | 1 | Resistive (plate) | Sample PID |
| Sample heater 2 | 2 | Resistive (plate) | Sample PID |
| Sample heater 3 | 3 | Resistive (plate) | Sample PID |
| Sample heater 4 | 4 | Resistive (plate) | Sample PID |
| Sample heater 5 | 5 | Resistive (plate) | Sample PID |
| Sample heater 6 | 6 | Resistive (plate) | Sample PID |
| Sample heater 7 | 7 | Resistive (plate) | Sample PID |
| Sample heater 8 | 8 | Resistive (plate) | Sample PID |
| Electronics box | 9 | Resistive | Box PID |

The number of sample heaters can be reduced (minimum ~4) by setting `hardware.heater_count` and `hardware.electronics_heater_index` in the config, and populating only the required PT100 channels.

### PWM Control

Heater duty cycles are output via GPIO PWM using `libgpiod`. The `LibgpiodPwmController` is implemented but requires GPIO pin assignments before flight.

**Pending:** Map each heater channel index (0–9) to a specific GPIO line number on the Pi's 40-pin header, and configure the corresponding MOSFET gate pins in the wiring.

### Power Budget

| Parameter | Value |
|---|---|
| Heater nominal power | 10 W each at 100% duty |
| Max active heaters | 4 simultaneously |
| Max thermal power | 40 W |
| Max system power | 48.23 W |

The `HeaterScheduler` enforces the 4-heater and 40 W limits. The electronics heater is de-prioritized during `ACTIVATION_RAMP`.

---

## Visual Status LEDs

Two discrete LEDs on the Pi 40-pin header provide at-a-glance flight-software
health without requiring a ground-station link.

| LED | Default GPIO line | Config key | Behaviour |
|---|---|---|---|
| Heartbeat | BCM 17 | `hal.status_led_line` | Toggles once per main-loop tick (1 Hz at default `runtime.tick_hz=1.0`). If it stops blinking, the main loop is stalled. |
| Mode indicator | BCM 27 | `hal.mode_led_line` | Blink pattern reflects `SystemMode` (SOLID = nominal boot, HEARTBEAT = running, FAST_BLINK = warning, SOS = fault). |

Wire each LED anode through a ~330 Ω series resistor to the listed BCM GPIO,
cathode to ground. Both lines are requested as outputs by `GpioStatusLed`
(libgpiod). When `runtime.use_simulated_pwm=true`, a `SimulatedStatusLed`
replacement logs state transitions to stderr instead of touching GPIO.

## HAL Status Summary

| Component | File | Status | Pending Work |
|---|---|---|---|
| `SpiAdapter` | `hal/spi_adapter.hpp` | **Stub** | MIKROE-2815 / MAX31865 SPI RTD driver |
| `I2cAdapter` | `hal/i2c_adapter.hpp` | **Stub** | BME280 + ADS1115 I2C drivers |
| `RtcAdapter` | `hal/rtc_adapter.hpp` | **Stub** (system clock) | External RTC I2C driver (DS3231) |
| `LibgpiodPwmController` | `hal/pwm_controller.hpp` | **Implemented** | GPIO pin mapping |
| `SimulatedPwmController` | `hal/pwm_controller.hpp` | **Implemented** | (bench testing only) |
| `GpioStatusLed` | `hal/status_led.hpp` | **Implemented** | libgpiod output on `hal.status_led_line` / `hal.mode_led_line` |
| `SimulatedStatusLed` | `hal/status_led.hpp` | **Implemented** | (bench testing only; logs transitions to stderr) |
| `Rs485ModbusAdapter` | *not yet created* | **Planned (Rev B)** | See [RS485 / Modbus bus](#rs485--modbus-bus-rev-b) |
| `StepperDriver` (channel 0) | `stepper/` | **Implemented** | Driver IC selection; SPI config path for TMC5160 |
| `StepperDriver` (channel 1) | *not yet created* | **Planned (Rev B)** | Second motor, role TBD |

### Writing a HAL Driver

All sensor data is collected by `SensorManager` in `onboard/src/sensor_manager.cpp`. To implement a real hardware driver:

1. Add the necessary read logic inside `SensorManager::ReadSnapshot()` using the shared `spi_adapter_` or `i2c_adapter_` instance.
2. Populate the corresponding fields in the `SensorSnapshot` struct.
3. Set `spi_adapter_.set_healthy(true)` / `i2c_adapter_.set_healthy(true)` on success; `set_healthy(false)` on bus errors.
4. The `SPI_OK`/`I2C_OK` status flags in telemetry are derived from these healthy flags.

---

## Simulation Model

When `bench_mode=true` (or hardware is absent), `SensorManager` uses a simplified thermal model:

- Sample temperatures start at simulated ambient and converge toward the PID setpoint at a rate proportional to applied heater duty
- Ambient pressure decreases linearly over time, simulating ascent to float altitude
- After reaching `ascent_to_activation_mbar`, pressure holds briefly then rises (simulating descent)

This allows full end-to-end software testing including automatic phase transitions, without any physical hardware.

## Stepper motor + Pi-EzConnect HAT (Rev A — one motor)

- **HAT:** Adafruit Pi-EzConnect Terminal Block Breakout. All GPIO lines pass
  through unchanged — wire sensors and the stepper driver to the terminal
  block next to the matching BCM number.
- **Stepper driver:** TBD (A4988 / DRV8825 / TMC2209-class). Software uses a
  STEP/DIR/EN abstraction (`coatheal::StepperDriver`) so any of those drop in.
- **Default BCM assignments** (overridable via `onboard.ini`):
  - `stepper.step_line=5`
  - `stepper.dir_line=6`
  - `stepper.enable_line=13` (active-low on most drivers)
- **Power:** drive the stepper from the gondola 28.8 V rail through the
  driver's VMOT input, not from the Pi 5 V supply. Share ground with the HAT.
- **Pulse generation:** the flight software currently issues pulses from the
  main tick loop (up to `stepper.default_step_hz`). Once the driver choice is
  locked, a dedicated RT thread / hardware PWM path will replace the loop
  pulser; the `StepperDriver` interface is stable.

Rev B extends this to two independent stepper channels — see
[Two stepper motors](#two-stepper-motors-rev-b).

---

## Rev B parts list (in review)

Proposed replacement / addition of parts, posted 2026-04-16. The table
records the vendor link, what the part does in the COATHEAL design, and the
compatibility verdict. "Drop-in" means no architectural change, "Extends
architecture" means new HAL or config, "Blocker" means the part cannot be
used as-is.

### Parts table

| # | Part (vendor link) | Role | Verdict |
|---|---|---|---|
| 1 | [Pi Hut **S-THP-01A** RS485 T/RH/P sensor](https://thepihut.com/products/rs485-air-temperature-humidity-and-barometric-pressure-sensor-s-thp-01a) | Replaces BME280 for ambient T/RH/P | Extends architecture — new RS485/Modbus bus. **Verify pressure range covers ≤10 mbar float**, else keep BME280 as backup. |
| 2 | AliExpress — PTFE/FEP silver-plated Teflon wire | High-temperature wiring for heater leads and hot-zone sensor cabling | Drop-in (no SW impact). Correct choice for the +70 °C plate zone. |
| 3 | [BigTreeTech **TMC5160** SPI stepper driver](https://biqu.equipment/products/bigtreetech-tmc5160-v1-0-driver-spi-mode-silent-high-precision-stepstick-stepper-motor-driver-with-heatsink-for-skr-v1-3-gen-v1-4-reprap) | Driver IC for the high-current stepper (Pololu 2851) | Extends architecture — adds optional SPI configuration path (current, µstep, stealthChop) on top of STEP/DIR/EN. Put on SPI1 to avoid contention with PT100 bus on SPI0. |
| 4 | [Pololu **#2851** NEMA-17 stepper](https://www.pololu.com/product/2851) | High-torque motor (1.68 A/phase, 200 steps/rev) — primary actuator | Drop-in motor. Must be paired with TMC5160. |
| 5 | [Adafruit **#2711** MAX31865 PT100 amplifier](https://www.adafruit.com/product/2711) | Drop-in equivalent of MIKROE-2815 | Drop-in — same driver code. Useful where Click form factor doesn't fit. |
| 6 | [MIKROE **RTD Click** (MIKROE-2815)](https://www.mikroe.com/rtd-click) | PT100 SPI amplifier (MAX31865) | Drop-in — unchanged from Rev A. |
| 7 | AliExpress — DRV8825/A4988 dual "42" stepper carrier | StepStick carrier with two driver sockets | Drop-in board. Accepts one TMC5160 + one A4988/DRV8825 side-by-side (pin-compatible StepStick pinout). Cleaner cabling than two bare breakouts. |
| 8 | [Adafruit **#1085** DS1307 RTC breakout](https://www.adafruit.com/product/1085) | Real-time clock | **Recommend swap to DS3231.** DS1307 has no temperature compensation and will drift visibly at −60 °C stratospheric cold-soak; DS3231 is pin/I2C-compatible and TCXO-backed. SW treats them identically. |
| 11 | [Labfacility **XF-931** PT100 RTD probe](https://ro.farnell.com/labfacility/xf-931-far/rtd-probe-w-lead-pt100-250deg/dp/2749460) | PT100 sensing element, −50 to +250 °C wired probe | Drop-in — compatible with MAX31865 / MIKROE-2815. Range covers the mission envelope. |
| 13 | AliExpress — 220 V polyimide film heater | Proposed sample heater (flexible Kapton) | **Blocker** — 220 V AC element will dissipate <2 % of rated power on the 28.8 V DC gondola rail. Must source a 24 V or 28 V DC polyimide variant instead. |
| 14 | [Adafruit **#2652** BME280 breakout](https://www.adafruit.com/product/2652) | Ambient T/RH/P | Drop-in Rev A part. Recommendation: keep it in parallel with the S-THP-01A as a redundant I2C backup if RS485 fails. |
| 15 | [Sigmanortec XT60 F/M connector](https://sigmanortec.ro/Conector-XT60-Mama-Tata-p148577270) | 28.8 V rail connector | Drop-in (mechanical only). |
| 16 | [Adafruit **#1918** NEMA-17 stepper (12 V / 350 mA)](https://www.adafruit.com/product/1918) | Low-current motor — second actuator, role TBD | Drop-in motor. Paired with A4988/DRV8825 on the dual carrier board. |
| 17 | [Rotorama XT60 Y-splitter](https://www.rotorama.com/product/xt60-rozdvojka) | Power Y-splitter to Pi PSU + heater bus | Drop-in (mechanical). |
| 18 | [Raspberry Pi 4 Model B](https://www.raspberrypi.com/products/raspberry-pi-4-model-b/) | OBC | Unchanged. |
| 19 | AliExpress — **4-channel RS485/Modbus PT100/PT1000 collector (−40…+500 °C)** | Alternative PT100 readout — 4 RTDs per module on Modbus | Extends architecture. Only 4 channels per unit, so cannot replace all 9 sample channels with a single collector. See [PT100 channel plan](#pt100-channel-plan-rev-b). |

### RS485 / Modbus bus (Rev B)

Rev B introduces a third field bus alongside I2C and SPI: **RS485 running
Modbus-RTU**. This bus is shared by the S-THP-01A ambient sensor and the
4-channel PT100 Modbus collector(s).

- **Transceiver:** Pi 4 has no native RS485. Options:
  - MAX485/MAX3485 transceiver on the Pi UART (`/dev/serial0`) — cheapest,
    but steals the console UART and requires direction-control GPIO.
  - USB-to-RS485 dongle (FTDI or CH340 based) — appears as `/dev/ttyUSB0`,
    no GPIO used, easier to swap.
  - RS485 HAT (Waveshare or similar) — plug-and-play, costs board real estate.
  - **Recommendation:** USB-RS485 dongle for bench work, decide on
    transceiver vs. HAT for flight after mechanical layout is settled.
- **Protocol:** Modbus-RTU (binary, CRC-16). Use a small embedded C++ Modbus
  client library (e.g. libmodbus) or a hand-rolled reader — the request set
  is trivial (`read_holding_registers`).
- **HAL class:** new `Rs485ModbusAdapter` alongside `I2cAdapter` and
  `SpiAdapter`. Exposes a `ReadHoldingRegisters(slave_id, addr, count)` API
  and tracks a `healthy_` flag that feeds a new `RS485_OK` status bit.
- **Config:** new `[rs485]` section — device path, baud rate (typ. 9600 or
  19200), parity, timeout. Per-device: Modbus slave address, register map
  identifier (`sthp01a`, `pt100_4ch`).

### Ambient-environment plan (Rev B)

Preferred path:

1. **Primary:** S-THP-01A on RS485. Populates `ambient_temp_c`,
   `ambient_pressure_mbar`, `ambient_humidity_pct` in the snapshot.
2. **Backup:** BME280 on I2C kept wired. If the RS485 read fails or falls
   outside a plausibility window (e.g. P > 1100 mbar, T outside −60…+50 °C),
   `SensorManager` falls back to the BME280 reading for that tick.
3. **Known gap:** BME280 lower pressure limit is 300 mbar; S-THP-01A
   coverage at stratospheric float (~5–10 mbar) **must be verified from
   vendor datasheet** before removing the BME280 entirely. If neither sensor
   is valid below 50 mbar, the `P_OK` status bit is cleared and `PHASE`
   transitions fall back to the time-based fallbacks in `StateManager`.

### PT100 channel plan (Rev B)

The single 4-channel Modbus collector cannot feed 9 sample heaters on its
own. Three configurations are on the table — decide by flight budget:

| Option | Sample PT100 sourcing | Box PT100 | Bus load |
|---|---|---|---|
| A — All Modbus | 3× 4-ch RS485 collectors (12 ch, 9 used) | One channel on a collector | RS485 only |
| B — Mixed (recommended) | 2× 4-ch RS485 collectors (8 ch) + 1× MAX31865 on SPI for specimen #9 | MAX31865 on SPI | RS485 + SPI |
| C — SPI retained | 9× MIKROE-2815 / MAX31865 on SPI (Rev A design) | MAX31865 on SPI | SPI only |

Option **B** minimises new driver surface (reuses the SPI RTD driver for the
last channel and the box) while keeping most of the RTD readout on the
cleaner industrial RS485 bus. `SensorManager` must iterate a mixed source
list — this is straightforward since `sample_temps_c` is already a vector of
channels, not a fixed struct.

### Two stepper motors (Rev B)

Rev B introduces two sample-pulling steppers, each owning four specimens.
Motors pull downward to induce microcracks. Only one motor pulls at a time
(MotionLock), and no heater duty is delivered while a pull is active (heater
scheduler interlock).

| Channel | Motor | Driver | Sample group | Purpose |
|---|---|---|---|---|
| Stepper 0 | Pololu 2851 NEMA-17 (1.68 A/phase) | **TMC5160** on SPI1 (`/dev/spidev1.0`) | samples 0–3 | Microcrack-pull actuator |
| Stepper 1 | Adafruit 1918 NEMA-17 (12 V / 350 mA) | **A4988 / DRV8825** (STEP/DIR/EN) | samples 4–7 | Microcrack-pull actuator |

**Motion envelope.** Max pull rate **100 full-steps/s (≈30 rpm)**, trapezoidal
accel/decel at **200 steps/s²** (0.5 s ramp from 0 → 100 Hz). One full
revolution = 200 full-steps = **1–2 mm** of downward travel. Default
microstepping **4× (800 µsteps/rev)**, configurable to 5× (1000 µsteps/rev).

**TMC5160 driver (motor 0).** Programmed at boot with run current 1.5 A RMS
(`IRUN`=21 on the 0.075 Ω sense-resistor scale), hold current 30 %
(`IHOLD`≈6), microstep 4× (`MRES`=6), stealthChop enabled (`GCONF` bit 2).
Register map lives in `onboard/src/tmc5160_driver.cpp`. After SPI
configuration, motion is identical STEP/DIR/EN to motor 1.

**A4988 driver (motor 1).** Plain STEP/DIR/EN, optional microstep pins
MS0/MS1/MS2 wired if the board exposes them.

**Default BCM assignments** (overridable via `onboard.ini`):

- Motor 0: `motor0.step_line`, `motor0.dir_line`, `motor0.enable_line` — the
  legacy single-stepper keys (`stepper.step_line=5`, `stepper.dir_line=6`,
  `stepper.enable_line=13`) remain as a Rev A fallback that routes to
  channel 0.
- Motor 1: `motor1.step_line`, `motor1.dir_line`, `motor1.enable_line`,
  `motor1.ms{0,1,2}_line` (free BCM pins on the HAT).

**Power.** Both steppers share the gondola 28.8 V rail through each driver's
VMOT input and share ground with the HAT. TMC5160 VM tolerates 8–60 V;
A4988 tolerates 8–35 V.

**Pulse generation.** `StepperChannel` issues pulses either from the main
tick loop (bench / CI) or from a dedicated near-RT `std::thread` that
sleep-spaces pulses via `std::chrono::steady_clock` (flight). The
trapezoidal ramp logic is identical on both paths.

**Software impact summary.**

- `StepperDriver` is the common STEP/DIR/EN base. `Tmc5160Driver` adds a
  boot-time SPI configuration pass, then operates via STEP/DIR/EN like any
  other driver.
- Config adds `motor0.*` and `motor1.*` sections plus a shared `pull.*`
  motion-envelope block. Legacy `stepper.*` / `bend.*` keys still honored
  (route to channel 0).
- Commands take a motor-id first argument: `STEPPER_MOVE <id> <steps>`,
  `STEPPER_BEND <id> <steps> [hold_s]`, `PULL_ARM <id>`, `PULL_EXECUTE <id>`,
  etc. `<id>` defaults to 0 when omitted.
- Telemetry emits `STEPPER0=pos:…|…` and `STEPPER1=pos:…|…` segments, plus
  an `EVT,PULL,…` event frame per completed pull cycle.
- Interlocks (enforced in software): `MotionLock::TryAcquire(motor_id)`
  around every pull; `HeaterScheduler` zeros all duty when the lock is
  active (new `HEATER_INHIBITED` STATUS bit).

### Open questions — Rev B blockers

These must be resolved before the Rev B implementation plan can be
finalised and the BOM committed:

1. **Heater voltage mismatch (part #13).** The polyimide film heater listing
   is rated 220 V AC. The BEXUS 28.8 V DC rail will not drive it. Need a
   24 V or 28 V DC variant, confirmed by listing or datasheet.
2. **S-THP-01A pressure range.** Must cover stratospheric float pressure
   (≤10 mbar) for the primary sensor path to be useful without the BME280
   backup. Datasheet value to be confirmed.
3. **RTC chip confirmation (part #8).** Confirm DS3231 is being purchased
   instead of DS1307. Same breakout footprint either way.
4. **Second stepper role.** What does Stepper 1 actuate? This drives the
   `stepper1.*` default configuration (step rate, microstep, soft limits,
   per-phase motion schedule) and the extended command vocabulary.
5. **RS485 transceiver form factor.** UART + MAX485 vs. USB-RS485 dongle vs.
   RS485 HAT. Affects `onboard.ini` device path and which GPIO lines stay
   free.
6. **PT100 channel option A / B / C.** Decides how many RS485 collectors are
   on the BOM and whether the SPI PT100 bus is retained.
7. **BME280 kept or removed.** Redundancy vs. I2C bus load vs. BOM cost.

Resolved open questions as of 2026-04-16: item 4 (second stepper role —
drives samples 4–7 for microcrack pulls) and items 1/2/3/5/6/7 remain open.
