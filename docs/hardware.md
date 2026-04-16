# Hardware Reference

---

## System Overview

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

---

## Sensors

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

One MIKROE-2815 click board per sample heater zone. Each board reads one PT100 resistance temperature detector (RTD) and returns a temperature value via SPI.

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

An external real-time clock (e.g. DS3231) provides accurate UTC time even after power cycling.

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
| `SpiAdapter` | `hal/spi_adapter.hpp` | **Stub** | MIKROE-2815 SPI RTD driver |
| `I2cAdapter` | `hal/i2c_adapter.hpp` | **Stub** | BME280 + ADS1115 I2C drivers |
| `RtcAdapter` | `hal/rtc_adapter.hpp` | **Stub** (system clock) | External RTC I2C driver |
| `LibgpiodPwmController` | `hal/pwm_controller.hpp` | **Implemented** | GPIO pin mapping |
| `SimulatedPwmController` | `hal/pwm_controller.hpp` | **Implemented** | (bench testing only) |
| `GpioStatusLed` | `hal/status_led.hpp` | **Implemented** | libgpiod output on `hal.status_led_line` / `hal.mode_led_line` |
| `SimulatedStatusLed` | `hal/status_led.hpp` | **Implemented** | (bench testing only; logs transitions to stderr) |

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

## Stepper motors + Pi-EzConnect HAT

REV-B: **two** sample-pulling steppers, interlocked so only one pulls at a
time. The heater scheduler also gates duty while a pull is in progress.

- **HAT:** Adafruit Pi-EzConnect Terminal Block Breakout. All GPIO lines pass
  through unchanged — wire both driver boards' STEP/DIR/EN (and MS0..2 for
  motor 1) to the terminal block next to the matching BCM number.
- **Motor 0 (samples 0–3):** Pololu 2851 NEMA-17 high-torque, driven by a
  **TMC5160** on **SPI1** (/dev/spidev1.0). The IC is programmed at boot
  with run current 1.5 A RMS (`IRUN`=21 on the 0.075 Ω sense resistor
  scale), hold current 30 % (`IHOLD`≈6), microstep 4× (MRES=6), stealthChop
  enabled (`GCONF` bit 2). See `onboard/src/tmc5160_driver.cpp` for the
  exact register map. STEP/DIR/EN share the same GPIO pattern as motor 1
  so pulse generation is identical.
- **Motor 1 (samples 4–7):** Adafruit 1918 NEMA-17, driven by an
  **A4988/DRV8825**. Plain STEP/DIR/EN, optional microstep pins MS0/MS1/MS2
  wired if the board exposes them.
- **Default BCM assignments** (overridable via `onboard.ini` once Agent D's
  config schema lands):
  - motor 0: `motor0.step_line`, `motor0.dir_line`, `motor0.enable_line`
    — the legacy single-stepper keys (`stepper.step_line=5`,
    `stepper.dir_line=6`, `stepper.enable_line=13`) are kept as a fallback
    for the REV-A single-motor build.
  - motor 1: `motor1.step_line`, `motor1.dir_line`, `motor1.enable_line`,
    `motor1.ms{0,1,2}_line` (free BCM pins on the HAT).
- **Power:** drive both steppers from the gondola 28.8 V rail through each
  driver's VMOT input; share ground with the HAT. Motor 0's TMC5160 has
  VM input tolerant to 8–60 V. Motor 1's A4988 is 8–35 V.
- **Motion envelope:** max pull rate 100 full-step/s (≈30 rpm), trapezoidal
  accel/decel at 200 full-step/s² (0.5 s ramp from 0 to 100 Hz). 1
  revolution (200 full-steps) ≈ 1–2 mm of downward pull.
- **Pulse generation:** `StepperChannel` issues pulses either from the main
  tick loop (bench / CI) or from a dedicated near-RT std::thread that
  sleep-spaces pulses via `std::chrono::steady_clock` (flight). The
  trapezoidal ramp logic is identical on both paths.
- **Interlock:** before pulling, each channel calls
  `MotionLock::TryAcquire(motor_id)`; on failure the command is rejected.
  The heater scheduler must consult the same lock (and the channel's
  `samples()` mapping) to refuse duty while a pull is active — Agent D owns
  the implementation, the stepper side ships the API stub.
