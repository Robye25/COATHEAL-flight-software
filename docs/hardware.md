# Hardware Reference (Rev B.1)

---

## Status

Rev B.1 (April 2026) is the BOM committed in [`lista_cumparaturri.xlsx`](../lista_cumparaturri.xlsx) and implemented in the current source tree. It narrows Rev B in three directions: 6 heaters instead of 9, no electronics-box heater / sensor / PID, no humidity output. The INA3221 chips — originally scoped as power-monitor housekeeping — are repurposed as a **sample-resistance science instrument** that detects microcrack formation during pulls. Two stratospheric-grade MS5803-01BA sensors replace the BME280 for ambient P + T with no humidity output. Both steppers become OMC integrated ball-screw motors with a 1 mm lead driven by a TMC2240 each.

Driver-level bring-up (real I2C / SPI / Modbus-RTU transactions) is still pending; HAL classes are stubs that track a `healthy_` flag and feed the `*_OK` status bits. Firmware logic, wire format, and tests already run against the Rev B.1 data model.

---

## System Overview

COATHEAL runs on a **Raspberry Pi 4 Model B (8 GB)** and interfaces with the following hardware:

| Subsystem | Component | Interface | Count | Status |
|---|---|---|---|---|
| Ambient P + T | GY-MS5803-01BA | I2C | 1 | Stub — I2C driver pending |
| Sample temperatures | Labfacility XF-931 PT100 probe through 4-ch Modbus RTD collector | Modbus-RTU over USB-RS485 | 2 collectors × 4 ch = 8 | Stub — Modbus HAL pending |
| Sample resistance | INA3221 V/I monitor (I2C 0x40 and 0x41) | I2C | 2 chips × 3 ch = 6 | `Ina3221Adapter` stub (zeros) |
| UV irradiance | GUVA-S12SD analog photodiode through ADS1015 ADC (12-bit) | I2C | 1 | Stub — ADS1015 driver pending |
| Real-time clock | External RTC (DS3231 carried from Rev B) | I2C | 1 | Stub (system clock fallback) |
| Sample heaters | 5 W @ 24 V DC polyimide film (Kapton) through 6-ch MOSFET module | GPIO PWM | 6 (samples 0–5) | `LibgpiodPwmController` implemented; pin mapping TBD |
| Steppers | OMC 17E19S2504BSM5-150RS integrated ball-screw NEMA-17 (1 mm lead) | STEP/DIR/EN + TMC2240 SPI | 2 | `StepperChannel` implemented; `Tmc2240Driver` SPI pass stubbed |
| Stepper drivers | TinyTronics QHV5160 (TMC2240) | SPI1 (`/dev/spidev1.0`) + STEP/DIR/EN | 2 | Stub |
| HAT | Adafruit Pi-EzConnect Terminal Block Breakout | pass-through | 1 | — |
| 5 V rail | Pololu D24V50F5 step-down (5 V / 5 A) from 28.8 V gondola rail | — | 1 | HW |
| Connectors | XT60U | — | 8 | HW |
| Heartbeat status LED | Discrete LED + 330 Ω | GPIO (libgpiod) | 1 | BCM 17 (`hal.status_led_line`) |
| Mode indicator LED | Discrete LED + 330 Ω | GPIO (libgpiod) | 1 | BCM 27 (`hal.mode_led_line`) |

There are **no BME280, no MIKROE-2815 / MAX31865 SPI PT100, no box heater, and no humidity output** in Rev B.1.

---

## Sensors

### MS5803-01BA — Ambient Pressure + Box Temperature (I2C)

Single I2C sensor (GY-MS5803-01BA module on a breakout) mounted **inside the electronics enclosure**. It serves a dual role at Rev B.1:

- **Pressure** reading = true ambient. The BEXUS gondola is unpressurised, so enclosure pressure equals external ambient pressure. The 10–1300 mbar part range natively covers stratospheric float (~5–10 mbar is out of spec but usable with derated accuracy; the `P_AMBIENT_OK` status bit flags out-of-range reads).
- **Temperature** reading = **box / enclosure interior temperature** (the sensor is inside the box). There is no separate box temperature sensor in Rev B.1; the MS5803 covers both roles. The on-wire field is still `ambient_temp_c` and must be collected / stored / sent on every tick so the ground operator can watch enclosure thermal conditions.

There is **no humidity channel** — `SensorSnapshot::ambient_humidity_pct` was removed at Rev B.1.

| Parameter | Value |
|---|---|
| Temperature range | −40 °C to +85 °C |
| Pressure range | 10–1300 mbar |
| Interface | I2C |
| HAL class | `I2cAdapter` (shared with ADS1015 and the INA3221 chips) |
| Fields populated | `ambient_temp_c`, `ambient_pressure_mbar` |

**Driver status:** stub. `I2cAdapter` tracks a `healthy_` flag; a real driver must issue MS5803 reset + coefficient load + D1/D2 ADC conversions and populate the snapshot.

### PT100 × 8 via 2 × 4-channel Modbus RTD collectors (USB-RS485)

Eight Labfacility XF-931 PT100 probes feed two 4-channel Modbus-RTU PT100 collectors. Both collectors share a single USB-to-RS485 converter (`/dev/ttyUSB0`) on a shared Modbus bus with distinct slave addresses. Channel count matches sample count exactly — there is no free channel for a box temperature, which is why the box-temperature field was removed.

| Parameter | Value |
|---|---|
| Probes | 8 × Labfacility XF-931 PT100, −50 °C to +250 °C |
| Collectors | 2 × AliExpress 4-channel RS485/Modbus PT100/PT1000 (−40 °C to +500 °C) |
| Bus | RS485 + Modbus-RTU (binary, CRC-16) |
| Transceiver | USB-to-RS485 serial converter (`/dev/ttyUSB0`) |
| HAL class | `Rs485ModbusAdapter` (planned; not yet created) |
| Fields populated | `sample_temps_c[0..7]` |

**Driver status:** stub. No `Rs485ModbusAdapter` yet. `SensorManager` currently runs the simulation path (temperature converges toward a simulated setpoint based on applied duty).

### INA3221 × 2 — Sample-Resistance Science Instrument (I2C)

Rev B.1 repurposes the two INA3221 3-channel V/I monitors as a **sample-resistance instrument**, not as a power monitor. Each chip exposes channels 1..3, covering 6 of the 8 samples. The instrument detects the resistance step that accompanies microcrack formation during a mechanical pull. Samples 6 and 7 have no INA3221 channel assigned at Rev B.1 and are emitted as `-` in the wire frame.

| Parameter | Value |
|---|---|
| I2C addresses | 0x40 (chip A), 0x41 (chip B) |
| Channels per chip | 3 (`channel` = 1..3 per the INA3221 register map) |
| HAL class | [`Ina3221Adapter`](../onboard/include/coatheal/hal/ina3221_adapter.hpp) |
| Fields populated | `sample_resistance_ohm[0..5]`, wire-format `RESISTANCE=` segment |
| Status bit | `RESISTANCE_OK` / `RESISTANCE_FAIL` (from `Ina3221Adapter::healthy()`) |

**Driver status:** stub. `Ina3221Adapter::ReadChannel()` returns zeros with `healthy_ = true`; real I2C bring-up is deferred to flight-hardware rehearsal. `SensorManager` synthesises a per-sample resistance decay (~5 % per observed pull) so the ground plotter sees motion during bench runs. `NotePullCompleted(motor_id)` is driven from the `EVT,PULL` edge in [`system_controller.cpp`](../onboard/src/system_controller.cpp).

### GUVA-S12SD + ADS1015 — UV irradiance (I2C)

The GUVA-S12SD analog UV photodiode output is read through an **ADS1015** 12-bit ADC over I2C (not ADS1115, and not BPW21). The ADC result is normalised to a float in `SensorSnapshot::uv`.

| Parameter | Value |
|---|---|
| Resolution | 12-bit |
| Interface | I2C (ADS1015 default address 0x48) |
| HAL class | `I2cAdapter` |
| Fields populated | `uv` |

**Driver status:** stub. Driver must configure the ADS1015 multiplexer and gain, trigger a conversion, and normalise to 0–1.

### External RTC (I2C)

DS3231-class external RTC (TCXO-backed, survives stratospheric cold-soak). Carried unchanged from Rev B.

| Parameter | Value |
|---|---|
| Interface | I2C |
| HAL class | `RtcAdapter` |

**Driver status:** stub — returns `std::chrono::system_clock` and sets `rtc_valid = false`.

---

## Heaters (Rev B.1)

### Mapping

| Heater index | Sample index | Type |
|---|---|---|
| 0 | 0 | 5 W polyimide film |
| 1 | 1 | 5 W polyimide film |
| 2 | 2 | 5 W polyimide film |
| 3 | 3 | 5 W polyimide film |
| 4 | 4 | 5 W polyimide film |
| 5 | 5 | 5 W polyimide film |
| — | 6 | **Pulled by motor 1 but UNHEATED** — PT100 read only |
| — | 7 | **Pulled by motor 1 but UNHEATED** — PT100 read only |

`hardware.heater_count = 6`. Heater index `i` maps 1:1 to sample index `i`. There is **no electronics-box heater**; `hardware.electronics_heater_index` defaults to `SIZE_MAX` as the sentinel "no box heater".

### PWM control

Heater duty cycles are output through a 6-channel 5 V MOSFET module driven by GPIO PWM through `libgpiod` (`LibgpiodPwmController`). The MOSFET module gates the 24 V DC supply to the polyimide film elements.

**Pending:** map each heater channel index (0..5) to a specific BCM GPIO on the Pi 40-pin header and record the pin assignment here.

### Power budget

| Parameter | Value |
|---|---|
| Heater nominal power | 5 W at 100 % duty |
| Max active heaters | 4 simultaneously (`power.max_active_heaters=4`) |
| Max thermal power | 20 W (`power.max_thermal_w=20.0`) |
| Max system power | 48.23 W (`power.max_system_w=48.23`, informational) |
| Energy budget | 130 Wh (`power.energy_budget_wh=130.0`; BEXUS 150 Wh allocation minus ~20 Wh avionics reserve) |

`HeaterScheduler` enforces both the 4-heater and 20 W limits. There is no box heater to de-prioritise; the `prioritize_samples` flag is now effectively a no-op in the scheduler because every controllable heater is a sample heater.

---

## Motion

### Steppers

Both motors at Rev B.1 are **OMC 17E19S2504BSM5-150RS NEMA-17 integrated ball-screw stepper motors** with a **1 mm lead** on a 150 mm screw. One full revolution = 200 full-steps = **1 mm linear pull** (exact). Both motors are driven by a **TinyTronics QHV5160 (TMC2240)** stepper driver configured via SPI and operated via STEP/DIR/EN after setup.

| Channel | Motor | Driver | Samples | Role |
|---|---|---|---|---|
| 0 | OMC 17E19S2504BSM5-150RS (1 mm lead) | TMC2240 on SPI1 (`/dev/spidev1.0`, CS on `motor0.cs_line`) | 0, 1, 2, 3 | Pull actuator for samples 0–3 (heated + measured) |
| 1 | OMC 17E19S2504BSM5-150RS (1 mm lead) | TMC2240 on SPI (separate CS line) | 4, 5, 6, 7 | Pull actuator for samples 4–7 (samples 4–5 heated+measured, 6–7 measured only) |

Both channels construct a `Tmc2240Driver` with distinct SPI chip-select lines. The previous Rev B plan to drive motor 1 with an A4988/DRV8825 is retired — `motor1.driver=a4988` strings in old INI snippets are vestigial.

### Motion envelope

| Parameter | Value |
|---|---|
| Full steps / revolution | 200 |
| Lead (ball screw) | **1 mm / rev** |
| Linear per revolution | **1 mm** (exact) |
| Max pull rate | 100 full-steps/s (≈30 rpm ≈ 0.5 mm/s) |
| Accel / decel | 200 full-steps/s² trapezoidal (0.5 s ramp 0 → 100 Hz) |
| Microstepping | 4× default (800 µsteps/rev = 800 µsteps/mm); 5× accepted |
| Pull cycle | forward `pull.travel_full_steps × microstep` → hold `pull.hold_s` (default 5 s) → retract to 0 |

### Interlocks

- [`MotionLock`](../onboard/include/coatheal/motion_lock.hpp): only one motor may pull at a time. `TryAcquire(motor_id)` fails if any motor (including the same id re-entering) already holds the lock.
- [`HeaterScheduler`](../onboard/include/coatheal/heater_scheduler.hpp): while the MotionLock is held, all heater duties are forced to zero and the `HEATER_INHIBITED` STATUS bit is raised.

### Default BCM assignments (overridable via `onboard.ini`)

- Shared legacy fallback (channel 0): `stepper.step_line=5`, `stepper.dir_line=6`, `stepper.enable_line=13`.
- Rev B per-motor keys — `motor0.step_line`, `motor0.dir_line`, `motor0.enable_line`, `motor0.cs_line=8`, `motor1.step_line`, `motor1.dir_line`, `motor1.enable_line` — are **parsed and ignored** at Rev B.1; the hard-coded defaults in `system_controller.cpp` drive both channels (see open tickets in [CHANGELOG-RevB.md](CHANGELOG-RevB.md)).

---

## Power

| Item | Value |
|---|---|
| Gondola rail | 28.8 V DC from the BEXUS power umbilical |
| 5 V rail | Pololu D24V50F5 step-down, 5 V / 5 A (supplies Pi + sensors) |
| Heater voltage | 24 V DC directly from the gondola rail through the 6-channel MOSFET module |
| Stepper VM | 28.8 V gondola rail into each TMC2240 (VM tolerates 8–60 V) |
| Connectors | XT60 (gondola rail in, Pi 5 V out, heater bus) |
| Energy budget | 150 Wh team allocation (BEXUS User Manual §5.2); firmware latches heaters at 130 Wh |
| Peak current | ≤3 A (SED §C.3); scheduler cap of 4 × 5 W = 20 W combined thermal draw leaves generous margin |

---

## Pi pinout table

The Pi-EzConnect HAT is a straight pass-through — terminal block numbers match BCM GPIO numbers.

| Function | BCM line | Config key | Notes |
|---|---|---|---|
| Heartbeat LED | 17 | `hal.status_led_line` | 330 Ω to LED |
| Mode LED | 27 | `hal.mode_led_line` | 330 Ω to LED |
| Motor 0 STEP | 5 | `motor0.step_line` (default; legacy `stepper.step_line`) | TMC2240 STEP |
| Motor 0 DIR | 6 | `motor0.dir_line` (default; legacy `stepper.dir_line`) | TMC2240 DIR |
| Motor 0 /EN | 13 | `motor0.enable_line` (default; legacy `stepper.enable_line`) | Active-low |
| Motor 0 CS (SPI1) | 8 | `motor0.cs_line` | SPI1 CE0 |
| Motor 1 STEP | TBD | `motor1.step_line` | Currently aliased to motor 0 on bench — real pin pending |
| Motor 1 DIR | TBD | `motor1.dir_line` | pending |
| Motor 1 /EN | TBD | `motor1.enable_line` | pending |
| Motor 1 CS (SPI) | TBD | (no key yet) | pending |
| Heater PWM 0..5 | TBD × 6 | (no keys yet) | 6-channel MOSFET module gates |
| I2C (MS5803, ADS1015, INA3221 ×2, RTC) | BCM 2 / 3 | — | Pi native `/dev/i2c-1` |
| SPI1 (TMC2240 config) | BCM 19/20/21 | — | `/dev/spidev1.0` |
| USB-RS485 (PT100 collectors) | USB | — | `/dev/ttyUSB0` |

---

## Visual status LEDs

| LED | Default GPIO | Config key | Behaviour |
|---|---|---|---|
| Heartbeat | BCM 17 | `hal.status_led_line` | Toggles once per main-loop tick (1 Hz default). If it stops, the main loop is stalled. |
| Mode | BCM 27 | `hal.mode_led_line` | Pattern reflects `SystemMode` (SOLID = boot, HEARTBEAT = running, FAST_BLINK = warn, SOS = fault). |

Wire each LED anode through a ~330 Ω series resistor to the listed BCM GPIO, cathode to ground. Both lines are requested as outputs by `GpioStatusLed` (libgpiod). When `runtime.use_simulated_pwm=true`, a `SimulatedStatusLed` logs transitions to stderr instead of touching GPIO.

---

## HAL status summary

| Component | File | Status | Pending work |
|---|---|---|---|
| `SpiAdapter` | [`hal/spi_adapter.hpp`](../onboard/include/coatheal/hal/spi_adapter.hpp) | Stub | TMC2240 SPI configuration pass |
| `I2cAdapter` | [`hal/i2c_adapter.hpp`](../onboard/include/coatheal/hal/i2c_adapter.hpp) | Stub | MS5803, ADS1015, RTC drivers |
| [`Ina3221Adapter`](../onboard/include/coatheal/hal/ina3221_adapter.hpp) | [`hal/ina3221_adapter.cpp`](../onboard/src/hal/ina3221_adapter.cpp) | Stub — reads zero, `healthy_ = true` | Real I2C read of bus+shunt voltage per channel, 2 chips (0x40 / 0x41), 3 channels each |
| `RtcAdapter` | [`hal/rtc_adapter.hpp`](../onboard/include/coatheal/hal/rtc_adapter.hpp) | Stub (system clock) | DS3231 I2C driver |
| `LibgpiodPwmController` | [`hal/pwm_controller.hpp`](../onboard/include/coatheal/hal/pwm_controller.hpp) | Implemented | GPIO pin map to 6-ch MOSFET module |
| `SimulatedPwmController` | [`hal/pwm_controller.hpp`](../onboard/include/coatheal/hal/pwm_controller.hpp) | Implemented (bench) | — |
| `GpioStatusLed` / `SimulatedStatusLed` | [`hal/status_led.hpp`](../onboard/include/coatheal/hal/status_led.hpp) | Implemented | — |
| `Rs485ModbusAdapter` | *not yet created* | Planned | MS5803 + 2 × 4-ch PT100 collector drivers |
| `Tmc2240Driver` | [`tmc2240_driver.hpp`](../onboard/include/coatheal/tmc2240_driver.hpp) | Partial — SPI config falls back to plain `GpioStepDirStepperDriver` off-sim | Real SPI register load |
| `StepperChannel` × 2 | [`stepper_channel.hpp`](../onboard/include/coatheal/stepper_channel.hpp) | Implemented | RT pulse thread opt-in once wiring is confirmed |

### Writing a HAL driver

All sensor data is collected by `SensorManager` in [`onboard/src/sensor_manager.cpp`](../onboard/src/sensor_manager.cpp). To implement a real driver:

1. Add the read logic inside `SensorManager::ReadSnapshot()` using the shared `spi_adapter_`, `i2c_adapter_`, or `ina_` pointer.
2. Populate the corresponding fields in `SensorSnapshot` (`ambient_temp_c`, `ambient_pressure_mbar`, `uv`, `sample_temps_c`, `sample_resistance_ohm`).
3. Set `..._adapter_.set_healthy(true)` on success and `set_healthy(false)` on bus errors. The `SPI_OK` / `I2C_OK` / `RESISTANCE_OK` STATUS bits are derived from these flags.

---

## Simulation model

When `runtime.use_simulated_pwm=true` (or when real hardware is absent), `SensorManager` runs a simplified model:

- Sample temperatures start at simulated ambient and converge toward the PID setpoint at a rate proportional to applied heater duty.
- Ambient pressure decreases linearly over time (simulated ascent), then rises back toward surface pressure.
- Sample resistance starts at a nominal base and decays ~5 % per observed pull on that motor's sample set, fed by `SensorManager::NotePullCompleted(motor_id)` from the `EVT,PULL` edge.

This allows full end-to-end software testing — phase transitions, pull cycles, resistance decay, telemetry bandwidth — without any physical hardware attached.

---

## Open questions (Rev B.1)

Resolved since Rev B:

1. Heater voltage — **24 V DC** polyimide film heaters confirmed (5 W each).
2. Box temperature sensor — **absent by design**; the 2 × 4-ch Modbus PT100 collectors cover the 8 samples exactly and no channel remains for a box probe.
3. Humidity — removed; MS5803-01BA has no humidity output and BME280 is not carried.
4. Second stepper role — motor 0 owns samples 0–3, motor 1 owns samples 4–7 (samples 6–7 unheated).
5. Both steppers use TMC2240 — no A4988/DRV8825 in the Rev B.1 BOM.
6. RTC — DS3231-class part retained.
7. Stepper linear calibration — **1 mm / revolution exactly** (OMC integrated ball screw, 1 mm lead). The Rev B "1–2 mm" wording is retired.

Still open:

1. **16 polyimide heaters on the BOM are spares.** Only 6 are wired for flight; the rest are bench + replacements. Confirm spare-handling plan before flight-unit integration.
2. **6 INA3221 channels for 8 samples.** Samples 6 and 7 have no resistance-measurement channel; they are pulled but their microcrack formation is not instrumented. Accepted at Rev B.1 — if deemed insufficient, a third INA3221 at 0x42/0x43 would cover them.
3. **`motor0.*` / `motor1.*` / `pull.*` INI keys** are parsed but not yet wired into `StepperChannelConfig`. `SystemController::Initialize()` uses compiled-in defaults.
4. **RS485 transceiver form factor** — Rev B.1 BOM carries 2 × USB-to-RS485 dongles, so `/dev/ttyUSB0` is the committed path. A UART + MAX485 option is no longer being tracked.
5. **Real TMC2240 SPI bring-up** — register load is currently bypassed.
