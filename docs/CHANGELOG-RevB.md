# Rev B Changelog — Thermal, Motor, Telemetry Rework

**Date:** 2026-04-17
**Branch:** `rev-b-integration`
**Scope:** Flight-software changes landed after the Rev A +70 °C mission was retired in favour of a floor-only thermal policy and a dual-stepper mechanical-pull experiment. The Rev B.1 delta below narrows the design further: 6 heaters, no box, INA3221 science path.

---

## Rev B.1 delta (2026-04-17)

Committed as `89393b7` on `rev-b-integration`. Summary of changes vs. Rev B:

1. **Heaters: 6, not 9.** 5 W @ 24 V DC polyimide film heaters on samples 0–5. Samples 6 and 7 are pulled by motor 1 but **unheated** (PT100 read only). `hardware.heater_count=6`.
2. **No electronics-box heater.** `hardware.electronics_heater_index` defaults to `SIZE_MAX` sentinel meaning "no box heater"; `deprioritize_electronics` is a no-op in that branch. `phase.box_target_c`, `pid.box_*`, and `heater.max_box_temp_c` were removed from the config schema.
3. **No box temperature sensor.** The two 4-channel Modbus PT100 collectors give exactly 8 channels = 8 samples, with none left for a box probe. `SensorSnapshot::box_temp_c` is deleted.
4. **No humidity.** MS5803-01BA replaces BME280; the new sensor has no humidity output. `SensorSnapshot::ambient_humidity_pct` is deleted.
5. **Power budget:** nominal heater 5 W (was 10 W), max thermal 20 W (was 40 W). `power.max_active_heaters=4` unchanged. `power.energy_budget_wh=130.0` unchanged.
6. **Stepper lead screw.** Both motors are now OMC 17E19S2504BSM5-150RS — integrated ball-screw, **1 mm lead**. One revolution = 200 full-steps = **1 mm linear** exactly (the previous "1–2 mm" wording is retired).
7. **Both motors are TMC2240.** Motor 1 no longer uses A4988/DRV8825; both channels construct a `Tmc2240Driver` with distinct CS lines. `motor1.driver=a4988` strings in INI/docs are vestigial.
8. **UV sensor: analog GUVA-S12SD through an ADS1015 (12-bit) ADC.** Not ADS1115, not BPW21.
9. **Ambient env: MS5803-01BA** on I2C, range 10–1300 mbar (covers stratospheric float natively — no BME280 backup needed and BME280 is not carried).
10. **INA3221 science role.** The two INA3221 chips are repurposed as the **sample-resistance instrument** used to detect microcrack formation during pulls. 2 chips × 3 channels = 6 of the 8 samples monitored. New `SensorSnapshot::sample_resistance_ohm` vector, new wire-format `RESISTANCE=` segment, new `RESISTANCE_OK`/`RESISTANCE_FAIL` STATUS bit driven from `Ina3221Adapter::healthy()`.
11. **Wire-format change:** DATA frame drops humidity + box_temp, adds `RESISTANCE=` segment after `HEATER_DUTY=`, STATUS bitfield gains trailing `RESISTANCE_OK`. The legacy single-`STEPPER=` fallback in the onboard serializer is retired (Rev B.1 is a breaking wire change); `STEPPER0=…` / `STEPPER1=…` indexed form is always emitted. Downlink bandwidth test is re-baselined against the slimmer frame.

### Files touched (see commit `89393b7`)

- `config/onboard.example.ini`, `config/onboard.debug.ini` — removed box/humidity keys; heater_count=6; heater_nominal_w=5; max_thermal_w=20; new stepper/pull envelope comments.
- `onboard/include/coatheal/config.hpp` — new defaults and `electronics_heater_index = SIZE_MAX` sentinel.
- `onboard/include/coatheal/hal/ina3221_adapter.hpp` + `onboard/src/hal/ina3221_adapter.cpp` — new HAL stub.
- `onboard/include/coatheal/sensor_manager.hpp` + `onboard/src/sensor_manager.cpp` — `sample_resistance_ohm`, `NotePullCompleted(motor_id)`, removal of `ambient_humidity_pct` / `box_temp_c`.
- `onboard/include/coatheal/status_flags.hpp` + `onboard/src/status_flags.cpp` — new `resistance_ok` bit.
- `onboard/include/coatheal/telemetry.hpp` + `onboard/src/telemetry.cpp` — `RESISTANCE=` segment, dropped humidity / box_temp columns, legacy `STEPPER=` serializer path retired.
- `onboard/include/coatheal/thermal_controller.hpp` + `onboard/src/thermal_controller.cpp` — 6-channel floor controller, box PID deleted.
- `onboard/src/heater_scheduler.cpp` — 5 W / 20 W defaults; box-index sentinel handling.
- `onboard/src/system_controller.cpp` — `ina_` HAL instance wired to `SensorManager`, `record.status.resistance_ok = ina_.healthy()`, `NotePullCompleted` fed from the `EVT,PULL` edge.
- `onboard/src/config.cpp` — schema changes; `SIZE_MAX` sentinel accepted.
- `tests/unit/*` + `tests/downlink_bandwidth_test.cpp` — retuned to 6/8/5 W semantics and the slimmer frame.

### Known follow-ups (open tickets)

1. Real I2C driver for INA3221 (stub today).
2. Real I2C drivers for MS5803-01BA, ADS1015, DS3231.
3. `Rs485ModbusAdapter` + driver for the 2 × 4-ch PT100 collectors over USB-RS485.
4. TMC2240 SPI configuration pass — falls back to plain `GpioStepDirStepperDriver` on non-simulated builds today.
5. Wire `motor0.*` / `motor1.*` / `pull.*` INI keys through `config.cpp` into `StepperChannelConfig`. Today these keys are parsed-and-ignored and `SystemController::Initialize()` uses compiled-in defaults.
6. GPIO mapping of the 6-channel MOSFET module.
7. SED revision request (see [SED-Compliance-Report.md §5](SED-Compliance-Report.md)).

---

## 1. Mission-level changes

| Aspect | Rev A | Rev B |
|---|---|---|
| Sample count | 9 + 1 electronics | **8** + 1 electronics |
| Sample target | Ramp to +70 °C, hold 90 min, descent floor −20 °C | **Floor ≥ +5 °C** at all times |
| Activation ramp | 0.85 °C/s to +70 °C | Removed |
| Float hold | 90 min timed hold at +70 °C | Removed (FSM now pressure-only) |
| Stepper motors | 1 | **2** (motor 0 → samples 0–3, motor 1 → samples 4–7) |
| Mechanical action | None in flight | **Downward pull** to induce microcracks |
| Heater ↔ motor | Independent | **Mutex interlock** — no duty while pulling |
| Motor ↔ motor | N/A | Only one motor pulls at a time (`MotionLock`) |

## 2. Mission phase FSM

Rev A phases removed: `ASCENT_HOLD`, `ACTIVATION_RAMP`, `FLOAT_HOLD`, `DESCENT_FLOOR`.
Rev B phases (`MissionPhase` in [onboard/include/coatheal/phase.hpp](../onboard/include/coatheal/phase.hpp)):

```
BOOT  →  ASCENT  →  FLOAT  →  DESCENT  →  LANDED  →  STOPPED
               P<100 mbar   P>300 mbar  P>800 mbar
```

All three flying phases (`ASCENT` / `FLOAT` / `DESCENT`) share the same thermal policy: each per-sample PID is **only active when sample temperature < sample_floor_c − 0.5 °C** (hysteresis), deactivates once at/above the floor, and freezes its integrator while off. Box PID continues to track its setpoint unchanged.

## 3. Motion envelope

| Parameter | Value |
|---|---|
| Full steps / revolution | 200 (NEMA-17) |
| Max pull rate | **100 full-steps/s** (≈30 rpm) |
| Accel / decel | **200 steps/s²** trapezoidal (0.5 s ramp 0→100 Hz) |
| Microstepping | **4× (800 µsteps/rev)** default; 5× (1000 µsteps/rev) accepted |
| One pull cycle | 200 full-steps forward (1 rev ≈ 1–2 mm), hold `pull.hold_s` (5 s default), retract to 0 |

Motor 0: Pololu 2851 NEMA-17, driven by **TMC2240** on SPI1 (`/dev/spidev1.0`) with run current 1.5 A RMS, hold current 30 %, stealthChop on. Motor 1: Adafruit 1918 NEMA-17, driven by A4988/DRV8825 (plain STEP/DIR/EN). See [`onboard/src/tmc2240_driver.cpp`](../onboard/src/tmc2240_driver.cpp) for the exact register values.

## 4. Software surface

### New headers / sources

- `onboard/include/coatheal/motion_lock.hpp` + `onboard/src/motion_lock.cpp` — heater↔motor mutex.
- `onboard/include/coatheal/stepper_channel.hpp` + `onboard/src/stepper_channel.cpp` — per-motor owner with trapezoidal ramp and optional RT pulse thread.
- `onboard/include/coatheal/tmc2240_driver.hpp` + `onboard/src/tmc2240_driver.cpp` — boot-time SPI configuration pass wrapping the STEP/DIR/EN base driver.

### New commands (all reversible, all newline-terminated)

| Command | Description |
|---|---|
| `STEPPER_MOVE <id> <steps>` | id defaults to 0 |
| `STEPPER_MOVETO <id> <abs> [hold_s]` | absolute position in µsteps |
| `STEPPER_ROTATE <id> <revs>` | |
| `STEPPER_BEND <id> <steps> [hold_s]` | alias for MOVETO, tagged |
| `STEPPER_HOME <id>` | |
| `STEPPER_STOP <id>` | |
| `STEPPER_SET_SPEED <id> <hz>` | full-step Hz, clamped to `pull.max_step_hz` |
| `STEPPER_SET_MICROSTEP <id> <n>` | 4 or 5 by default |
| `STEPPER_ENABLE <id>` / `STEPPER_DISABLE <id>` | |
| **`PULL_ARM <id>`** | acquire MotionLock, queue one forward pull |
| **`PULL_EXECUTE <id>`** | synchronous pull+hold+retract, releases lock |

Legacy single-motor forms (no id) continue to work and route to motor 0.

### Interlocks

- `MotionLock::TryAcquire(motor_id)` returns `false` if any motor (including the same id re-entering) already holds the lock. Enforced around every pull.
- `HeaterScheduler::Schedule()` forces **all duties to zero** while the lock is held and sets `heater_inhibited()` → reflected on the wire as the `HEATER_INHIBITED` STATUS bit. There is no soft clamp; zero is zero.

### Telemetry wire-format deltas

| Field | Rev A | Rev B |
|---|---|---|
| `sample_i` columns | 9 | **8** |
| `HEATER_DUTY=` | 10 values | **9** values (8 samples + 1 box) |
| Stepper segment(s) | `STEPPER=pos:…` | **`STEPPER0=…` + `STEPPER1=…`** |
| `PHASE=` values | `ASCENT_HOLD_-30C` etc. | `BOOT` / `ASCENT` / `FLOAT` / `DESCENT` / `LANDED` / `STOPPED` |
| New STATUS bits | — | `RS485_OK`, `HEATER_INHIBITED` |
| New event frame | `EVT,CYCLE,…` | **`EVT,PULL,<session>,<pull_id>,<motor_id>,<ts>,<steps>,<hold_s>,<samples>`** |

`parse_telemetry_csv` (ground station) still accepts the Rev A legacy single `STEPPER=` segment and 9-sample `HEATER_DUTY=` for replaying old logs — sample count is inferred from the position of `HEATER_DUTY=`, not hardcoded.

### Ground-station GUI

- 8 sample temperature traces (was 9), 9 heater-duty bars (was 10) — H0–H7 amber + BOX cyan.
- New **Motors** right-dock panel (M0 and M1 each: position, target, hz, microstep, enable/move indicators).
- New **Pull events** bottom-dock panel logging `EVT,PULL` frames with specimen membership.
- `<log>_pulls.csv` sibling file appended alongside `<log>_events.csv`.

### Configuration

Removed keys (all obsolete with the +70 °C mission):
```
phase.ascent_target_c       phase.activation_target_c
phase.float_target_c        phase.descent_floor_c
phase.activation_ramp_c_per_s
phase.float_hold_minutes
transition.ascent_to_activation_mbar
```

Added:
```
phase.sample_floor_c=5.0
transition.ascent_to_float_mbar=100.0
transition.descent_to_landed_mbar=800.0

hardware.heater_count=9
hardware.electronics_heater_index=8

pull.max_step_hz=100.0
pull.accel_steps_per_s2=200.0
pull.microstep=4
pull.travel_full_steps=200
pull.hold_s=5.0

motor0.driver=tmc2240
motor0.spi_device=/dev/spidev1.0
motor0.cs_line=8
motor0.step_line, motor0.dir_line, motor0.enable_line
motor0.run_current_a_rms=1.5
motor0.hold_current_frac=0.30
motor0.stealth_chop=1
motor0.samples=0,1,2,3

motor1.driver=a4988
motor1.step_line, motor1.dir_line, motor1.enable_line
motor1.ms{0,1,2}_line
motor1.samples=4,5,6,7
```

**Known follow-up:** `motor0.*` / `motor1.*` / `pull.*` keys are currently accepted-and-ignored by `LoadConfigFromIni`; `SystemController` uses hard-coded Rev B defaults for both channels. Plumbing them through `config.cpp` → `StepperChannelConfig` is the next config-schema ticket.

## 5. Agent / commit trail

The Rev B rework was parallelised across four isolated git worktrees, merged sequentially into `rev-b-integration`:

| Agent | Role | Merge commit |
|---|---|---|
| A | Thermal / phase refactor — floor controller, new enum, 9-channel duty vector | `merge: Agent A — thermal/phase refactor to +5C floor, 8 samples` |
| D | Safety interlocks — `MotionLock`, `HeaterScheduler` interlock, paranoid test set | `merge: Agent D — MotionLock + heater interlock` |
| B | Motion systems — dual `StepperChannel`, TMC2240 driver, trapezoidal ramp, PULL commands | `merge: Agent B — dual stepper channels + TMC2240 + pull commands` |
| C | Telemetry / ground-station — 8-sample DATA frame, dual STEPPER segments, EVT,PULL, GS UI | `merge: Agent C — 8-sample telemetry, dual-stepper segments, PULL events, GS UI` |
| Orchestrator | Integration — `system_controller.cpp` wiring, config accepts new keys, legacy tests updated | `integration: wire Rev B — MotionLock, dual stepper, PULL events, 8 samples` |

Each agent branch built and passed its own test target in isolation (the separate `coatheal_*_rev_b_tests` CMake targets) on the Pi 4 before merge.

## 6. Deferred work (open tickets)

1. **Hardware-config schema** — plumb `motor0.*` / `motor1.*` / `pull.*` keys from `config.cpp` into `StepperChannelConfig` so `SystemController::Initialize()` no longer hard-codes the Rev B envelope.
2. **RS485 HAL** — `Rs485ModbusAdapter` class + driver for the Pi Hut S-THP-01A ambient sensor and the 4-channel Modbus PT100 collector. See [docs/hardware.md](hardware.md) Rev B open questions.
3. **Heater voltage** — confirm replacement polyimide heaters are 24/28 V DC, not 220 V AC.
4. **DS3231 swap** — drop DS1307 (no temperature compensation; drifts at −60 °C cold-soak).
5. **S-THP-01A pressure-range validation** — must cover ≤10 mbar float before BME280 can be removed.
6. **Bench → flight pulse-thread switch** — `StepperChannel::use_pulse_thread` is off by default (tick-driven); enable per motor once wiring is confirmed.

## 7. Verification

Unit-test targets on the Pi (CMake target names):

```
coatheal_unit_tests
coatheal_state_machine_tests
coatheal_safety_tests
coatheal_downlink_bandwidth
coatheal_status_led_test
coatheal_stepper_tests
coatheal_phase_rev_b_tests          # Agent A
coatheal_safety_rev_b_tests         # Agent D
coatheal_stepper_rev_b_tests        # Agent B
coatheal_telemetry_rev_b_tests      # Agent C
```

Each Rev B target covers its agent's scope end-to-end (floor hysteresis, MotionLock mutex, trapezoidal ramp, dual-STEPPER segment serialisation, PULL event round-trip). Legacy targets were updated to the Rev B API without changing their intent. A full Pi build (`cmake --build build --parallel && ctest --test-dir build --output-on-failure`) is the next verification gate before flight-unit rehearsal.
