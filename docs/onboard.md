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
./build/onboard/coatheal_onboard --config config/onboard.local.ini
```

## Main Loop

`SystemController::Run` runs at `runtime.tick_hz` and performs this sequence:

1. Apply queued command overrides.
2. Read `SensorManager` snapshot.
3. Track link state and activate fallback only after configured link loss.
4. Update pressure phase tracking only during link-loss fallback.
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
| Established link lost past timeout | Continue active bend sequences and manual PID targets; stop non-sequence motion; apply +5 C floor only to channels without targets |
Rev C configuration requires `manual.manual_first=true`. Phase changes never
start motor motion. Operators use explicit jog, pull, or `BENDSEQ_*` commands.

## Final Hardware Model

| Subsystem | Software model |
|---|---|
| Sample temperatures | 8 PT100 probes through a Sequent Microsystems RTD HAT over I2C, one per channel |
| Ambient pressure / temperature | DPS310 over I2C |
| UV | GUVA-S12SD analog output through ADS1115 over I2C |
| Heaters | 6 polyimide heaters through EKM014/UCC27524 MOSFET inputs |
| Motors | 2 NEMA 17 ball-screw actuators through TMC5160 carriers, SPI-only position dribble (no STEP/DIR) |
| Sample resistance | 2 MAX31865 clicks, 4-wire Kelvin per coating specimen, on SPI0 native CE — drives the compatibility `RESISTANCE=` field by default (`max31865_click`) |
| RTD element resistance | PT100 element resistance read by the Sequent RTD HAT alongside temperature; available as the `sequent_rtd` `RESISTANCE=` source |

Real backends are implemented for libgpiod heater PWM, software-CS TMC5160
SPI-only motion, MAX31865 click SPI reads, the Sequent RTD HAT through Linux
`i2c-dev`, and DPS310/ADS1115 through Linux `i2c-dev`. They still require
bench validation against the exact boards, wiring, current limits, and
powered loads. The RTD HAT's register map is derived from vendor source and
gated on bench verification; see
[Sequent RTD Bench Bring-Up](sequent-rtd-bring-up.md). The TMC5160 current
model and SPI0 topology are gated on bench verification too; see
[TMC5160 Commissioning](tmc5160-commissioning.md).

## Thermal Control

`ThermalController` owns six PID controllers, one per heated sample.

| Parameter | Default |
|---|---|
| Heated samples | 0..5 |
| Unheated pulled samples | 6..7 |
| Manual target range | `heater.target_min_c..heater.target_max_c` (`0..80 C`) |
| Fallback floor | `phase.sample_floor_c=5.0` |
| Hysteresis | 0.5 C |
| Duty range | `0.0..1.0` |

Manual targets and per-channel PID gains are runtime controls. A duty override
clears the same channel's target; a target clears its duty override. Invalid or
stale mapped temperature data forces that heater off even in open-loop duty
mode. Fallback keeps existing targets and applies the floor controller only to
untargeted channels.

## Heater Scheduler

`HeaterScheduler` enforces:

| Limit | Default |
|---|---|
| `power.max_active_heaters` | 3 |
| `power.max_thermal_w` | 15 W |
| `power.heater_nominal_w` | 5 W |
| `power.energy_budget_wh` | 130 Wh |

When a motor holds `MotionLock`, every heater duty is forced to zero and
telemetry reports `HEATER_INHIBITED`.

## SensorManager

`SensorManager` runs DPS310, ADS1115, the Sequent RTD HAT, and the MAX31865
click pair (`Max31865Loop`, one worker polling both clicks) in separate
bounded polling threads. The 1 Hz main loop only copies the thread-safe
cache, so missing or timed-out sensors cannot delay commands, logging, or
telemetry.

`SensorManager` returns `SensorSnapshot`:

| Field | Final source |
|---|---|
| `ambient_temp_c` | DPS310 |
| `ambient_pressure_mbar` | DPS310 |
| `uv` | GUVA-S12SD through ADS1115 |
| `sample_temps_c` | Sequent RTD HAT, one card channel per logical sample |
| `sample_resistance_ohm` | MAX31865 click coating-specimen resistance by default (`max31865_click`, two monitored indices only); Sequent RTD HAT per-channel PT100 element resistance under `sequent_rtd`; `-`/simulated per `sensor.resistance_source` otherwise |

Simulation is used only when `runtime.use_simulated_sensors=true`. Real mode
does not replace failed reads with synthetic data and reports `SIMULATED` or
`REAL_SENSORS` in telemetry.

Each reading also carries current validity and last-success age. Never-valid
values are `nan`; failed readings retain their last value with validity false.

## Motion

The two motor channels are configured from `[motor0]`, `[motor1]`, and `[pull]`
INI keys. **There is no STEP or DIR GPIO — motion is SPI-only.**

| Motor | Default samples | Default SPI | CS (soft) | EN |
|---|---|---|---:|---:|
| M0 | 0,1,2,3 | `/dev/spidev0.0`, `SPI_NO_CS` | BCM 22 | BCM 20 |
| M1 | 4,5,6,7 | `/dev/spidev0.0`, `SPI_NO_CS` | BCM 27 | BCM 21 |

`Tmc5160Driver` uses SPI mode 3 with `SPI_NO_CS` (mandatory: SPI0's native
chip-selects are wired to the MAX31865 clicks, not the motors), drives the
configured CS GPIO in software, performs pipelined register reads, and
verifies IOIN/`VERSION==0x30` and written registers. Each `Step()` call maps
to one `ΔXTARGET` write into the chip's ramp generator ("position dribble")
rather than a STEP pulse — see
[TMC5160 Commissioning](tmc5160-commissioning.md) for the full contract.
Every absolute motion requires a
software zero established by `SET_POSITION_ZERO`; there are no limit switches.
`MotionLock` serializes both manual moves and runtime bend sequences.

## Telemetry

`SerializeTelemetryDataFrame` emits:

```text
DATA,<session>,<seq>,<ts>,<rtc_valid>,<ambient_temp_c>,<ambient_pressure_mbar>,<uv>,<sample_0>..<sample_7>,HEATER_DUTY=..,RESISTANCE=..,PHASE=..,MODE=..,STATUS=..,SENSOR_VALID=..,SENSOR_AGE_MS=..,COMPONENT_STATE=..,STEPPER0=..,STEPPER1=..
```

`RESISTANCE=` remains on the wire for parser compatibility. By default
(`sensor.resistance_source=max31865_click`) only the two monitored
`sensor.max31865_sample_indices` slots carry coating-specimen resistance from
the MAX31865 clicks; every other slot serializes `-`. With
`sensor.resistance_source=sequent_rtd`, all eight slots instead carry the
Sequent RTD HAT's per-channel PT100 element resistance; with
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
CHECK
COMPONENTS
ARM
DISARM
SET_PHASE <phase>
SET_HEATER_DUTY <index> <duty>
SET_ALL_DUTY <duty>
SET_TEMP_TARGET <index> <temp_c>
SET_PID <index|ALL> <kp> <ki> <kd>
GET_THERMAL
HEATERS_OFF
SET_POSITION_ZERO <id>
BENDSEQ_LOAD <id> <name> <target>:<hold>[:<speed>] ...
BENDSEQ_RUN <id> <name>
STEPPER_STOP <id>
SHUTDOWN_SAFE
```

See `docs/protocol.md` for the complete command list.

## HAL Status

| Adapter | Status |
|---|---|
| `Tmc5160Driver` | SPI-only position dribble: register writes and software CS/EN GPIO implemented; no STEP/DIR (retired with `GpioStepDirStepperDriver`, deleted); current model bench validation required |
| `LibgpiodPwmController` | Real 1 Hz software PWM thread with zero-on-start/stop |
| `I2cAdapter` | DPS310, ADS1115, and Sequent RTD HAT reads implemented |
| `Max31865Adapter` | Real one-shot SPI reads (native CE, mode 1) implemented for both clicks; reference resistor and coating-resistance range bench validation required |
| `SpiBus` (`LinuxSpiBus`) | Health boundary; shared by both TMC5160 drivers and, via native CE, both MAX31865 clicks |
| `RtcAdapter` | System-clock fallback |
| `Ina3221Adapter` | Retired stub; I2C addresses `0x40`/`0x41` are now the Sequent RTD card's stack-0/1 addresses and the INA3221 must not be re-enabled without re-addressing |

See `docs/hardware.md` and `docs/configuration.md` for the pin and bus
configuration.
