# Onboard Software Reference (Rev C)

The onboard application is a C++17 program (`coatheal_onboard`) for a Raspberry
Pi 4. Rev C is manual-first: while the ground link is healthy, operators command
phase, heater duties, and motor movement from the ground station. The onboard
keeps safety interlocks, telemetry/logging, watchdog recovery, and link-loss
fallback.

## Entry Point

`onboard/src/main.cpp` parses `--config <path>`, loads the INI file, constructs
`SystemController`, calls `Initialize`, then `Run`.

```bash
./build/onboard/coatheal_onboard --config config/onboard.example.ini
```

## Main Loop

`SystemController::Run` runs at `runtime.tick_hz` and performs this sequence:

1. Apply queued command overrides.
2. Read `SensorManager` snapshot.
3. Track link state and activate fallback only after configured link loss.
4. Update `StateManager` only in fallback or legacy autonomous mode.
5. Compute requested heater duties.
6. Apply `HeaterScheduler` limits: max active heaters, thermal power, energy, and `MotionLock`.
7. Apply duties to `PwmController`.
8. Tick both `StepperChannel` instances.
9. Serialize telemetry and write it to CSV plus durable queue.
10. Drain queued telemetry to the ground station and process ACKs.
11. Emit `EVT,PULL` after a motor finishes a pull.
12. Toggle status LEDs and feed the systemd watchdog.

## Manual-First Behavior

When `manual.manual_first=true`:

| Condition | Behavior |
|---|---|
| Ground link healthy | Operator commands phase, heaters, and pulls |
| Link not yet seen | System stays conservative; no fallback automation |
| Established link lost past timeout | Pressure FSM and +5 C sample floor controller may run |
| `manual.manual_first=false` | Legacy autonomous phase/fatigue behavior can run |

Automatic fatigue pulls are disabled for normal Rev C operation. Operators use
`PULL_ARM` and `PULL_EXECUTE`.

## Final Hardware Model

| Subsystem | Software model |
|---|---|
| Sample temperatures | 8 PT100 channels from DAQ132M over USB-RS485 Modbus |
| Ambient pressure / temperature | DPS310 over I2C |
| UV | GUVA-S12SD analog output through ADS1115 over I2C |
| Heaters | 6 polyimide heaters through EKM014/UCC27524 MOSFET inputs |
| Motors | 2 NEMA 17 ball-screw actuators through FYSETC TMC5160 drivers |
| Resistance | Disabled in final BOM; telemetry field retained for compatibility |

The current code has the final-BOM configuration schema and the TMC5160 SPI
configuration path. The generic GPIO PWM/pulse boundary and device-specific
DPS310, ADS1115, and DAQ132M reads still require bench validation and driver
completion before powered flight hardware tests.

## Thermal Control

`ThermalController` owns six floor controllers, one per heated sample.

| Parameter | Default |
|---|---|
| Heated samples | 0..5 |
| Unheated pulled samples | 6..7 |
| Floor target | `phase.sample_floor_c=5.0` |
| Hysteresis | 0.5 C |
| Duty range | `0.0..1.0` |

Manual heater overrides are applied while connected. PID floor control is used
only in fallback or legacy autonomous mode.

## Heater Scheduler

`HeaterScheduler` enforces:

| Limit | Default |
|---|---|
| `power.max_active_heaters` | 4 |
| `power.max_thermal_w` | 20 W |
| `power.heater_nominal_w` | 5 W |
| `power.energy_budget_wh` | 130 Wh |

When a motor holds `MotionLock`, every heater duty is forced to zero and
telemetry reports `HEATER_INHIBITED`.

## SensorManager

`SensorManager` returns `SensorSnapshot`:

| Field | Final source |
|---|---|
| `ambient_temp_c` | DPS310 |
| `ambient_pressure_mbar` | DPS310 |
| `uv` | GUVA-S12SD through ADS1115 |
| `sample_temps_c` | 8 DAQ132M PT100 channels |
| `sample_resistance_ohm` | Disabled final-BOM compatibility vector |

In bench/simulation mode, the manager synthesizes pressure, temperature, UV,
and optional resistance changes so the ground station can be tested without
hardware.

## Motion

The two motor channels are configured from `[motor0]`, `[motor1]`, and `[pull]`
INI keys.

| Motor | Default samples | Default SPI | STEP / DIR / EN |
|---|---|---|---|
| M0 | 0,1,2,3 | `/dev/spidev0.0` | BCM 5 / 6 / 13 |
| M1 | 4,5,6,7 | `/dev/spidev0.1` | BCM 19 / 26 / 16 |

`Tmc5160Driver` performs a boot-time SPI configuration pass. The step pulse
backend still must be bench-verified with the final wiring, motor current, and
enable polarity before powered motion.

## Telemetry

`SerializeTelemetryDataFrame` emits:

```text
DATA,<session>,<seq>,<ts>,<rtc_valid>,<ambient_temp_c>,<ambient_pressure_mbar>,<uv>,<sample_0>..<sample_7>,HEATER_DUTY=d0|..|d5,RESISTANCE=r0|..|r7,PHASE=..,MODE=..,STATUS=..,STEPPER0=..,STEPPER1=..
```

`RESISTANCE=` remains on the wire for parser compatibility. With
`sensor.resistance_source=disabled`, every slot serializes as `-`.

`SerializeTelemetryPullEventFrame` emits:

```text
EVT,PULL,<session_id>,<pull_id>,<motor_id>,<start_ts>,<steps_moved>,<hold_s>,<samples>
```

## Command Surface

The command server listens on TCP port `5000`. Commands are parsed on a
background thread and applied through synchronized overrides at the next main
loop tick.

Primary flight commands:

```text
PING
STATUS
ARM
DISARM
SET_PHASE <phase>
SET_HEATER_DUTY <index> <duty>
SET_ALL_DUTY <duty>
HEATERS_OFF
PULL_ARM <id>
PULL_EXECUTE <id>
STEPPER_STOP <id>
SHUTDOWN_SAFE
```

See `docs/protocol.md` for the complete command list.

## HAL Status

| Adapter | Status |
|---|---|
| `Tmc5160Driver` | SPI register writes implemented; bench validation required |
| `GpioStepDirStepperDriver` | Interface present; real pulse timing backend pending |
| `LibgpiodPwmController` | Configured output mapping; real PWM backend pending |
| `I2cAdapter` | Health boundary; DPS310/ADS1115 device reads pending |
| `SpiAdapter` | Health boundary; shared with TMC5160 and optional RTD Click |
| `RtcAdapter` | System-clock fallback |
| `Ina3221Adapter` | Historical compatibility stub; final BOM disables resistance |

See `docs/hardware.md` and `docs/configuration.md` for the pin and bus
configuration.
