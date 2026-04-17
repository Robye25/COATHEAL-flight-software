# Configuration Reference (Rev B.1)

All onboard configuration is loaded from an INI file at startup via `--config <path>`. See [`config/onboard.example.ini`](../config/onboard.example.ini) for the flight profile and [`config/onboard.debug.ini`](../config/onboard.debug.ini) for the bench/simulation profile.

## Format

Keys use the form `<section>.<key>=<value>`. Boolean values are `true`/`false`/`1`/`0`/`on`/`off`. Strings are unquoted. Unknown keys are rejected at load time, except the Rev B stepper keys (`motor0.*`, `motor1.*`, `pull.*`) which are currently parsed-and-ignored pending the multi-channel config-schema plumbing.

```ini
runtime.tick_hz=1.0
comms.telemetry_host=192.168.50.1
```

---

## Removed keys (Rev B.1)

These Rev A / early-Rev B keys are **no longer accepted** and must be deleted from any existing `onboard.ini`. Leaving them in place causes `LoadConfigFromIni` to return `unknown config key`.

| Key | Reason |
|---|---|
| `phase.ascent_target_c`, `phase.activation_target_c`, `phase.float_target_c`, `phase.descent_floor_c` | Rev B floor-only thermal policy — no per-phase setpoint schedule |
| `phase.activation_ramp_c_per_s`, `phase.float_hold_minutes` | Rev B removed the activation ramp and timed float |
| `phase.box_target_c` | Rev B.1 removed the electronics-box heater |
| `pid.box_kp`, `pid.box_ki`, `pid.box_kd` | Rev B.1 removed the box PID |
| `hardware.electronics_heater_index` | Accepted-but-vestigial; defaults to `SIZE_MAX` ("no box heater"). Supplying it is still legal but has no effect unless it points at a valid heater index |
| `heater.max_box_temp_c` | Rev B.1 removed box overtemp handling |
| `transition.ascent_to_activation_mbar` | Renamed `transition.ascent_to_float_mbar` when the activation-ramp phase was removed |

---

## Added / changed defaults (Rev B.1)

| Key | Old default | New default | Notes |
|---|---|---|---|
| `power.heater_nominal_w` | 10.0 | **5.0** | 5 W polyimide film heaters |
| `power.max_thermal_w` | 40.0 | **20.0** | 4 × 5 W ceiling |
| `hardware.heater_count` | 10, then 9 at Rev B | **6** | Samples 6 and 7 are unheated |
| `hardware.electronics_heater_index` | 9 (Rev A) / 8 (Rev B) | `SIZE_MAX` sentinel | "No box heater" |
| `pull.travel_full_steps` | 200 | 200 | Unchanged (one full revolution = **1 mm linear** with OMC ball-screw, 1 mm lead) |

---

## `[runtime]`

General process behaviour.

| Key | Type | Default | Description |
|---|---|---|---|
| `runtime.tick_hz` | float | `1.0` | Main-loop frequency in Hz. 1 Hz = one telemetry frame per second. |
| `runtime.bench_mode` | bool | `false` | Enable bench/simulation mode. Required to use extended debug commands. |
| `runtime.debug_arm_code` | string | `COATHEAL_DEBUG` | Token required by `ARM_DEBUG <token>` to unlock extended commands. Change before flight. |
| `runtime.use_simulated_pwm` | bool | `false` | Use in-memory `SimulatedPwmController` instead of real GPIO PWM. Automatically true in bench mode if no GPIO hardware is present. |
| `runtime.gpio_chip` | string | `/dev/gpiochip0` | `libgpiod` GPIO chip device path. Only used when `use_simulated_pwm=false`. |

---

## `[comms]`

| Key | Type | Default | Description |
|---|---|---|---|
| `comms.telemetry_host` | string | `127.0.0.1` | Ground station telemetry server IP the Pi **connects** to on `telemetry_port`. |
| `comms.static_ground_ip` | string | `192.168.50.1` | Static fallback ground station IP when UDP discovery fails. |
| `comms.static_pi_ip` | string | `192.168.50.2` | Static onboard IP (reference only; not bound directly). |
| `comms.telemetry_port` | int | `4000` | TCP port the Pi connects to for telemetry. |
| `comms.command_port` | int | `5000` | TCP port the Pi **listens on** for incoming commands. |
| `comms.reconnect_ms` | int | `2000` | ACK timeout in ms. Must exceed the ground station `accept()` cycle time. |
| `comms.discovery_enabled` | bool | `true` | Enable UDP discovery on `discovery_port`. |
| `comms.discovery_port` | int | `4100` | UDP port for discovery beacons. |
| `comms.discovery_period_ms`, `comms.rediscover_period_s`, `comms.failover_grace_s`, `comms.priority` | int | — | Discovery/failover tuning. |

---

## `[storage]`

| Key | Type | Default | Description |
|---|---|---|---|
| `storage.primary_log_path` | string | `logs/onboard_primary.csv` | Primary CSV telemetry log path. |
| `storage.secondary_log_path` | string | `logs/onboard_usb_mirror.csv` | Secondary CSV mirror (USB). |
| `storage.queue_dir` | string | `logs/telemetry-queue` | Durable disk-backed telemetry queue directory. |
| `storage.queue_retention_hours` | float | `72.0` | Maximum age of queued frames (hours). |
| `storage.queue_max_bytes` | uint64 | `8589934592` | Maximum queue size (bytes, default 8 GiB). |

---

## `[phase]` (Rev B.1 — floor-only, no box)

A single cold-protection floor is shared across `ASCENT`, `FLOAT`, and `DESCENT`. There is **no box PID** and no `phase.box_target_c` at Rev B.1.

| Key | Type | Default | Description |
|---|---|---|---|
| `phase.sample_floor_c` | float | `5.0` | Minimum sample temperature enforced during any flying phase (°C). Per-sample PIDs engage when `T_sample < (floor − 0.5 °C)` and disengage at/above the floor (0.5 °C hysteresis). |
| `phase.uniformity_tolerance_c` | float | `2.0` | Maximum allowed sample-to-sample spread (°C) across the **6 heated** samples. Exceeding this trips the `UNIFORMITY_FAIL` status bit (soft flag; heaters keep running). |

---

## `[transition]`

Pressure thresholds that drive the Rev B FSM (`BOOT → ASCENT → FLOAT → DESCENT → LANDED`). `SystemMode = RUN` is required for any transition; in `STANDBY` / `SAFE` the onboard stays in `BOOT`.

| Key | Type | Default | Description |
|---|---|---|---|
| `transition.ascent_to_float_mbar` | float | `100.0` | Pressure at/below which `ASCENT → FLOAT` triggers (mbar). |
| `transition.float_to_descent_mbar` | float | `300.0` | Pressure at/above which `FLOAT → DESCENT` triggers. |
| `transition.descent_to_landed_mbar` | float | `800.0` | Pressure at/above which `DESCENT → LANDED` triggers. |

---

## `[power]` (Rev B.1 — 5 W heaters)

Enforced by [`HeaterScheduler`](../onboard/include/coatheal/heater_scheduler.hpp).

| Key | Type | Default | Description |
|---|---|---|---|
| `power.max_active_heaters` | size_t | `4` | Maximum simultaneous active heaters. |
| `power.max_thermal_w` | float | `20.0` | Maximum combined thermal power (W). 4 × 5 W. |
| `power.max_system_w` | float | `48.23` | Total system power budget (informational). |
| `power.heater_nominal_w` | float | `5.0` | Nominal per-heater power at 100 % duty (W). |
| `power.energy_budget_wh` | float | `130.0` | Cumulative heater-energy budget (Wh). BEXUS User Manual §5.2 allocates 150 Wh per team; ~20 Wh is reserved for the Pi + sensors. Heaters latch off when the budget is consumed. `0` disables enforcement. |

---

## `[pid]` (Rev B.1 — sample PID only)

Rev B.1 has a single set of PID gains shared across all 6 per-sample PIDs. The Rev B box PID is gone.

| Key | Type | Default | Description |
|---|---|---|---|
| `pid.kp` | float | `0.20` | Proportional gain, per-sample PID. |
| `pid.ki` | float | `0.02` | Integral gain. |
| `pid.kd` | float | `0.03` | Derivative gain. |

All PIDs clamp the integral to `[-10, 10]` and output to `[0.0, 1.0]` (duty cycle). Gains can be overridden live via `SET_PID` (requires `ARM_DEBUG`).

---

## `[hardware]`

| Key | Type | Default | Description |
|---|---|---|---|
| `hardware.heater_count` | size_t | `6` | Number of heater channels (Rev B.1: 6 = samples 0..5). Must be ≥ 1. |
| `hardware.electronics_heater_index` | size_t | `SIZE_MAX` | Optional box-heater index. Default sentinel means "no box heater"; Rev B.1 flight config omits this key. If supplied, must be a valid index `< heater_count` or `SIZE_MAX`. |

Note: `SensorManager` samples-per-snapshot is a compile-time constant of 8 (`SensorManager::kSampleCount`), independent of `heater_count`. Samples 6 and 7 are pulled but carry no heater and no INA3221 channel.

---

## `[heater]` / `[sensor]`

| Key | Type | Default | Description |
|---|---|---|---|
| `heater.max_sample_temp_c` | float | `85.0` | Per-channel over-temperature latch threshold (°C). |
| `sensor.ambient_temp_min_c` / `_max_c` | float | `-90.0` / `50.0` | Accepted ambient-T window; outside → `T_AMBIENT_FAIL`. |
| `sensor.ambient_pressure_min_mbar` / `_max_mbar` | float | `5.0` / `1050.0` | Accepted ambient-P window; outside → `P_AMBIENT_FAIL`. |

---

## `[hal]` — Visual status LEDs

| Key | Type | Default | Description |
|---|---|---|---|
| `hal.status_led_line` | size_t | `17` | BCM GPIO for heartbeat LED. |
| `hal.mode_led_line` | size_t | `27` | BCM GPIO for mode-indicator LED. |

---

## `[stepper]` — legacy single-channel keys (motor 0 fallback)

Honored as a Rev B fallback. `SystemController` builds both channels from these values plus compiled-in Rev B defaults until the `motor0.*` / `motor1.*` keys below are plumbed end-to-end.

| Key | Type | Default | Description |
|---|---|---|---|
| `stepper.steps_per_rev` | int | `200` | NEMA-17 full-step count. |
| `stepper.microstep` | int | `16` (code) / `4` (INI) | Microstepping divisor. |
| `stepper.default_step_hz`, `stepper.max_step_hz` | float | `400.0` / `4000.0` (code); INI caps at `100.0` | Full-step pulse rate / ceiling. |
| `stepper.max_position_steps` | int64 | `200000` | Absolute travel limit (µsteps). |
| `stepper.step_line`, `stepper.dir_line`, `stepper.enable_line` | size_t | `5`, `6`, `13` | BCM GPIOs for STEP/DIR/EN. |
| `stepper.invert_direction` | bool | `false` | — |
| `stepper.enable_active_low` | bool | `true` | Most STEP/DIR drivers have active-low /EN. |
| `stepper.enable_on_boot` | bool | `false` | Keep driver de-energised until commanded. |

---

## `[pull]` — shared motion envelope (parsed, not yet wired)

Rev B.1 accepts and ignores these keys; `StepperChannelConfig` uses compiled-in defaults that match the values below. Next config-schema ticket will wire them through.

| Key | Value in compiled defaults | Description |
|---|---|---|
| `pull.max_step_hz` | `100.0` | Ceiling full-step rate during a pull (≈30 rpm ≈ 0.5 mm/s on the 1 mm-lead ball screw). |
| `pull.accel_steps_per_s2` | `200.0` | Trapezoidal accel/decel (0.5 s ramp 0 → 100 Hz). |
| `pull.microstep` | `4` | 4× microstepping (800 µsteps/rev = 800 µsteps/mm). |
| `pull.travel_full_steps` | `200` | One pull cycle = 200 full-steps forward (= **1 rev = 1 mm** on the OMC ball screw). |
| `pull.hold_s` | `5.0` | Hold time at target before retracting. |

---

## `[motor0]`, `[motor1]` — dual-stepper (parsed, not yet wired)

Both motors are OMC 17E19S2504BSM5-150RS integrated ball-screw NEMA-17 (1 mm lead) driven by a TMC2240. The Rev B `motor1.driver=a4988` wording is retired — both channels are TMC2240.

| Key | Typical value | Description |
|---|---|---|
| `motor0.driver` | `tmc2240` | Always TMC2240 at Rev B.1 (SPI config + STEP/DIR/EN). |
| `motor0.spi_device` | `/dev/spidev1.0` | SPI bus for TMC2240 register load. |
| `motor0.cs_line` | `8` | BCM GPIO for SPI1 CE0. |
| `motor0.step_line` / `dir_line` / `enable_line` | `5` / `6` / `13` | STEP/DIR/EN pins (distinct per motor on flight wiring). |
| `motor0.run_current_a_rms` | `1.5` | TMC2240 run current (A RMS). |
| `motor0.hold_current_frac` | `0.30` | Hold current as a fraction of run. |
| `motor0.stealth_chop` | `1` | StealthChop on. |
| `motor0.samples` | `0,1,2,3` | Specimen indices owned by motor 0. |
| `motor1.driver` | `tmc2240` | Same driver as motor 0 at Rev B.1. |
| `motor1.step_line` / `dir_line` / `enable_line` | TBD | Distinct pins; compiled-in bench defaults alias to motor 0 pins today. |
| `motor1.samples` | `4,5,6,7` | Motor 1 owns samples 4–7 (6 and 7 unheated). |

The legacy `motor1.ms{0,1,2}_line` keys for A4988 microstep strapping are still parsed-and-ignored for backward compatibility, but Rev B.1 doesn't use A4988.

---

## `[bend]` — per-phase bend schedule

Optional per-mission-phase bend move applied on phase entry to motor 0. Set to 0 until calibrated.

| Key | Type | Default |
|---|---|---|
| `bend.ascent_steps`, `bend.ascent_hold_s` | int64, float | `0`, `0` |
| `bend.activation_steps`, `bend.activation_hold_s` | int64, float | `0`, `0` |
| `bend.float_steps`, `bend.float_hold_s` | int64, float | `0`, `0` |
| `bend.descent_steps`, `bend.descent_hold_s` | int64, float | `0`, `0` |

---

## Notes

- All paths are resolved relative to the **working directory** at launch, unless they are absolute. The systemd service sets `WorkingDirectory=/bexus/code/coatheal`.
- `bench_mode=true` disables hardware I/O (sensors return simulated data) and enables extended debug commands after `ARM_DEBUG`.
- The `debug_arm_code` token is transmitted in plaintext over the command TCP connection — change it from the default before flight.
- `runtime.tick_hz` is the **boot-time** rate. During flight the operator can change it live with `SET_TICK_HZ <hz>` (range `[0.1, 5.0]`, no debug arm required).
- The systemd unit (`deploy/coatheal-onboard.service`) uses `Type=notify` and `WatchdogSec=10`. The main loop pings the systemd watchdog every tick via `sd_notify(WATCHDOG=1)`; if the loop hangs for >10 s systemd restarts the process.
