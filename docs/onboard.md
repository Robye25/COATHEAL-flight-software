# Onboard Software Reference (Rev B.1)

The onboard software is a C++17 application (`coatheal_onboard`) running on a Raspberry Pi 4. It autonomously controls the COATHEAL experiment from power-on through mission completion.

## Entry point

**[`onboard/src/main.cpp`](../onboard/src/main.cpp)**

Parses `--config <path>`, calls `LoadConfigFromIni`, constructs `SystemController`, calls `Initialize`, then `Run`. Exits with the return value of `Run`.

```bash
./build/onboard/coatheal_onboard --config config/onboard.example.ini
```

---

## SystemController

**[`onboard/src/system_controller.cpp`](../onboard/src/system_controller.cpp)** | **[`onboard/include/coatheal/system_controller.hpp`](../onboard/include/coatheal/system_controller.hpp)**

Master orchestrator. Owns and coordinates all subsystems. Main loop runs at `tick_hz` (default 1 Hz).

### Key methods

| Method | Description |
|---|---|
| `Initialize(error*)` | Initialise subsystems; start command server thread. |
| `Run()` | Blocking main loop; returns exit code. |
| `DrainTelemetryQueue(link_ok*, error*)` | Send pending frames to ground station; update link status. |
| `HandleCommandLine(line)` | Dispatch an incoming command string. Called from the command server thread. |

### Override flags

Commands from the ground station set thread-safe flags (`std::mutex overrides_mu_`) consumed at the top of each tick, so commands never interrupt a sensor read or heater update mid-tick.

| Flag | Set by | Effect |
|---|---|---|
| `force_start` | `FORCE_START` | Transition from `BOOT` into `ASCENT` next tick. |
| `force_stop` | `FORCE_STOP` | Transition toward `DESCENT` / `STOPPED`. |
| `reset_control` | `RESET_CTRL` | `ThermalController::Reset()` next tick. |
| `shutdown_safe` | `SHUTDOWN_SAFE` | Set `running_ = false`, stop loop. |
| `heaters_off` | `HEATERS_OFF` | Zero all heater duties via `ControlOverrides`. |
| `single_heater_override` | `SET_HEATER_DUTY` | Duty for one heater index (0..5). |
| `all_heaters_override` | `SET_ALL_DUTY` | Duty for all heaters. |
| `pid_override` | `SET_PID` | Override per-sample PID gains. |

### Per-tick loop order

1. Apply queued state/control overrides.
2. `SensorManager::ReadSnapshot` — 8-sample temperatures, ambient P + T, UV, sample resistance.
3. `StateManager::Update` — pressure-driven FSM.
4. `ThermalController::ComputeRequestedDuty` — 6 per-sample PIDs.
5. `HeaterScheduler::Schedule` — 4-heater / 20 W / 130 Wh caps, MotionLock interlock.
6. `PwmController::SetDuty` — apply to GPIO or simulated backend.
7. `StepperChannel::Tick` × 2.
8. Build `TelemetryRecord` + status flags (`SPI_OK`, `I2C_OK`, `LINK_OK`, `T_AMBIENT_OK`, `P_AMBIENT_OK`, `UNIFORMITY_OK`, `OVERTEMP_OK`, `ENERGY_OK`, `RS485_OK`, `HEATER_INHIBITED`/`HEATER_ACTIVE`, `RESISTANCE_OK`).
9. `SerializeTelemetryDataFrame` → storage CSV + telemetry queue.
10. `DrainTelemetryQueue` → TCP send + ACK reconciliation.
11. `EVT,PULL` edge detection (falling edge of `moving` on any channel) → `SerializeTelemetryPullEventFrame` → storage + queue; also calls `SensorManager::NotePullCompleted(motor_id)` so the simulator steps the resistance decay.
12. Toggle heartbeat LED; feed `sd_notify(WATCHDOG=1)`.

---

## StateManager

**[`onboard/src/state_manager.cpp`](../onboard/src/state_manager.cpp)** | **[`onboard/include/coatheal/state_manager.hpp`](../onboard/include/coatheal/state_manager.hpp)**

Pressure-driven Rev B FSM (`BOOT → ASCENT → FLOAT → DESCENT → LANDED`, plus `STOPPED`). No timed expiry; every transition is a pure function of the latest pressure reading and override flags.

- `ASCENT → FLOAT` at `transition.ascent_to_float_mbar` (100 mbar default)
- `FLOAT → DESCENT` at `transition.float_to_descent_mbar` (300 mbar)
- `DESCENT → LANDED` at `transition.descent_to_landed_mbar` (800 mbar)
- `FORCE_START` / `FORCE_STOP` / `SHUTDOWN_SAFE` can override from any state.

---

## ThermalController

**[`onboard/src/thermal_controller.cpp`](../onboard/src/thermal_controller.cpp)** | **[`onboard/include/coatheal/thermal_controller.hpp`](../onboard/include/coatheal/thermal_controller.hpp)**

Rev B.1: **6-channel floor controller**. There is no box PID.

### PID topology

| Controller | Count | Target | Feedback | Drives |
|---|---|---|---|---|
| Per-sample PID | **6** | `phase.sample_floor_c` (+5 °C) | `sample_temps_c[i]` | `heater_duty[i]` for i ∈ 0..5 |

Samples 6 and 7 are pulled but unheated; they have no PID and no heater. Sample resistance (INA3221) is a passive instrument — it does not feed any control loop.

### Setpoint schedule (Rev B floor-only)

| Phase | Sample policy |
|---|---|
| `BOOT` / `LANDED` / `STOPPED` | 0.0 (heaters off) |
| `ASCENT` / `FLOAT` / `DESCENT` | Floor = `phase.sample_floor_c` (+5 °C), 0.5 °C hysteresis |

Each PID engages only when `T_sample < (floor − 0.5 °C)` and disengages at/above the floor. While disengaged, the integrator is frozen.

### `ComputeRequestedDuty(phase, snapshot, dt, overrides)`

Returns a `std::vector<double>` of size `heater_count` (= 6). Overrides (`heaters_off`, `single_heater_override`, `all_heaters_override`, `pid_override`) are applied before returning.

### `Reset()`

Resets all 6 PID integrators and clears the overtemp/uniformity latches. Called by `RESET_CTRL`.

---

## HeaterScheduler

**[`onboard/src/heater_scheduler.cpp`](../onboard/src/heater_scheduler.cpp)** | **[`onboard/include/coatheal/heater_scheduler.hpp`](../onboard/include/coatheal/heater_scheduler.hpp)**

Enforces power and count constraints on the requested duties.

### Constraints (Rev B.1 defaults)

| Parameter | Default | Description |
|---|---|---|
| `power.max_active_heaters` | 4 | Maximum simultaneous active heaters. |
| `power.max_thermal_w` | 20 W | Maximum combined thermal power. |
| `power.heater_nominal_w` | 5 W | Nominal per-heater power at 100 % duty. |
| `power.energy_budget_wh` | 130 Wh | Cumulative heater energy; latches off on exhaustion. |

### Interlocks

- `MotionLock` holder test: when any motor holds the lock, all duties are forced to zero and `heater_inhibited() == true` → wire-format `HEATER_INHIBITED` bit.
- `prioritize_samples` flag: historically de-prioritised the electronics box heater during flying phases. At Rev B.1 there is no box heater, so the flag is effectively a no-op.

### Algorithm

1. Rank heaters by requested duty (highest first).
2. Accept heaters greedily until `max_active_heaters` or `max_thermal_w` is reached.
3. Clamp accepted duties to the remaining power headroom.
4. Zero all rejected heaters.
5. Integrate cumulative energy; latch off when `energy_budget_wh` is exhausted.

---

## PidController

**[`onboard/src/pid_controller.cpp`](../onboard/src/pid_controller.cpp)** | **[`onboard/include/coatheal/pid_controller.hpp`](../onboard/include/coatheal/pid_controller.hpp)**

Standard discrete PID with anti-windup and output clamping.

```
output = kp × error + ki × integral + kd × derivative
```

- **Integral anti-windup:** clamped to `[-10, 10]`.
- **Output clamping:** `[0.0, 1.0]` (duty cycle).
- `Update(setpoint, measurement, dt)` returns the clamped duty.
- `Reset()` zeros integral and derivative state.

---

## SensorManager

**[`onboard/src/sensor_manager.cpp`](../onboard/src/sensor_manager.cpp)** | **[`onboard/include/coatheal/sensor_manager.hpp`](../onboard/include/coatheal/sensor_manager.hpp)**

Reads all sensors and returns a `SensorSnapshot`. In bench/simulation mode, uses a physics-based model rather than real hardware.

### Constructor

```cpp
SensorManager(const OnboardConfig& config,
              SpiAdapter* spi,
              I2cAdapter* i2c,
              RtcAdapter* rtc,
              Ina3221Adapter* ina = nullptr);
```

`SystemController` passes `&spi_`, `&i2c_`, `&rtc_`, `&ina_` — all HAL adapters are owned by `SystemController`.

### `SensorSnapshot` fields (Rev B.1)

| Field | Source | Units |
|---|---|---|
| `timestamp_utc` | RTC or system clock | ISO-8601 UTC string |
| `rtc_valid` | `RtcAdapter` | bool |
| `ambient_temp_c` | MS5803-01BA (I2C) | °C |
| `ambient_pressure_mbar` | MS5803-01BA (I2C) | mbar |
| `uv` | GUVA-S12SD via ADS1015 (I2C) | normalised float |
| `sample_temps_c` | 8 × PT100 through 2 × 4-ch Modbus RTD collectors | °C (vector, size 8) |
| `sample_resistance_ohm` | 2 × INA3221 (I2C 0x40 / 0x41, ch 1..3) | Ω (vector, size ≤ 8; unmeasured samples serialize as `-`) |

**Humidity and box temperature are gone.** There is no `ambient_humidity_pct` and no `box_temp_c` at Rev B.1.

### Simulation model

When bench mode is active (or hardware is absent):

- Samples start at simulated ambient and converge toward the PID setpoint at a rate proportional to applied heater duty. Samples 6 and 7 receive no heat (no heater) and drift with ambient.
- Ambient pressure decreases linearly (simulated ascent), then rises (descent).
- Sample resistance starts at a nominal base and decays ~5 % per observed pull via `NotePullCompleted(motor_id)` edge fed from the `EVT,PULL` serializer in `system_controller.cpp`.

### Status accessors

- `t_ambient_ok()` — ambient-T inside `[sensor.ambient_temp_min_c, sensor.ambient_temp_max_c]`.
- `p_ambient_ok()` — ambient-P inside `[sensor.ambient_pressure_min_mbar, sensor.ambient_pressure_max_mbar]`.
- `resistance_ok()` — drives the `RESISTANCE_OK` wire bit. True iff the INA3221 instrument is present and its `healthy_` flag is set.

---

## TelemetryClient

**[`onboard/src/telemetry_client.cpp`](../onboard/src/telemetry_client.cpp)** | **[`onboard/include/coatheal/telemetry_client.hpp`](../onboard/include/coatheal/telemetry_client.hpp)**

Outbound TCP connection to the ground station.

### Session ID

Generated at startup: `<hostname>-<unix_seconds>-<monotonic_ns % 1000000>`. Uniquely identifies one run of the onboard process. The ground station uses this for deduplication.

### `SendFrameAwaitAck(frame, ack*)`

1. If not connected, `ConnectLocked()`.
2. Send `frame + '\n'`.
3. Wait up to `reconnect_ms` for an ACK line.
4. Parse ACK; reject on session_id or seq mismatch.
5. On any failure, `CloseLocked()` and return false.

### `ConnectLocked()`

1. If `discovery_enabled`, try `DiscoverGroundHostLocked()` (3 attempts × `reconnect_ms` each).
2. Fall back to `static_ground_ip`.
3. TCP `connect()` to resolved host and `telemetry_port`.

### `DiscoverGroundHostLocked()`

Bind UDP to `discovery_port` (4100) and listen for `GS_HELLO` broadcasts. Reply with `ONBOARD_HELLO`; return the sender's IP.

---

## TelemetryQueue

**[`onboard/src/telemetry_queue.cpp`](../onboard/src/telemetry_queue.cpp)** | **[`onboard/include/coatheal/telemetry_queue.hpp`](../onboard/include/coatheal/telemetry_queue.hpp)**

Persistent, disk-backed FIFO queue for telemetry frames. Survives process restarts.

| Property | Default |
|---|---|
| Max age | `queue_retention_hours` (72 h) |
| Max size | `queue_max_bytes` (8 GiB) |
| On startup | prune aged + over-size frames |

### Key methods

| Method | Description |
|---|---|
| `Enqueue(frame, error*)` | Persist frame to disk queue. |
| `PendingFrames()` | Return all unacked frames in order. |
| `Acknowledge(session_id, seq, error*)` | Mark frames up to `seq` acknowledged; delete their files. |
| `size()` | Count of pending frames. |

---

## StorageManager

**[`onboard/src/storage_manager.cpp`](../onboard/src/storage_manager.cpp)** | **[`onboard/include/coatheal/storage_manager.hpp`](../onboard/include/coatheal/storage_manager.hpp)**

Writes telemetry lines to two independent CSV paths (primary: SD card, secondary: USB drive).

- Writes `# COATHEAL telemetry log` header on first write.
- Continues writing if one path fails; tracks `sd_ok` / `usb_ok` status flags.
- `wrote_header_` is only set after at least one successful header write, preventing header-less CSVs after partial failures.

---

## CommandServer / CommandParser

**[`onboard/src/command_server.cpp`](../onboard/src/command_server.cpp)** — TCP server on `command_port` (5000). Accepts one client at a time, loops: read one line → `handler(line)` → send response + close.

**[`onboard/src/command_parser.cpp`](../onboard/src/command_parser.cpp)** — trims whitespace, replaces commas with spaces, uppercases the first token, validates argument count, and returns a `CommandParseResult`.

The `ARM_DEBUG <token>` → extended command set is documented in [docs/protocol.md](protocol.md).

---

## HAL adapters

Hardware Abstraction Layer in [`onboard/src/hal/`](../onboard/src/hal/) and [`onboard/include/coatheal/hal/`](../onboard/include/coatheal/hal/).

### Current status

| Adapter | Status | Description |
|---|---|---|
| `SpiAdapter` | Stub | Tracks `healthy_`; no real SPI reads. Shared with `Tmc2240Driver`. |
| `I2cAdapter` | Stub | Shared by MS5803-01BA, ADS1015, RTC. |
| `RtcAdapter` | Stub (system clock) | DS3231 driver pending. |
| `Ina3221Adapter` | Stub — returns zeros | Two chips at I2C 0x40 / 0x41, channels 1..3. `SensorSnapshot::sample_resistance_ohm` is filled from the INA3221 instrument; in stub mode SensorManager synthesises resistance decay from `NotePullCompleted` edges. |
| `LibgpiodPwmController` | Implemented | Real PWM via `libgpiod`; GPIO pin mapping needed for the 6-ch MOSFET module. |
| `SimulatedPwmController` | Implemented | In-memory duty array for bench testing. |
| `GpioStatusLed` | Implemented | `hal.status_led_line` / `hal.mode_led_line`. |
| `SimulatedStatusLed` | Implemented | Logs state transitions to stderr (bench). |

### Pending implementation

- **MS5803-01BA I2C driver** — ambient P + T.
- **ADS1015 I2C driver** — UV ADC.
- **INA3221 I2C driver** — real reads for the sample-resistance instrument.
- **DS3231 I2C driver** — RTC sync.
- **`Rs485ModbusAdapter`** — 2 × 4-ch PT100 collectors over USB-RS485 (`/dev/ttyUSB0`).
- **TMC2240 SPI configuration pass** — `Tmc2240Driver` currently falls back to plain `GpioStepDirStepperDriver` outside simulation.
- **GPIO pin mapping** — heater MOSFET gates and per-motor STEP/DIR/EN pins.

See [docs/hardware.md](hardware.md) for interface specifications.

---

## Telemetry serializer

**[`onboard/src/telemetry.cpp`](../onboard/src/telemetry.cpp)** | **[`onboard/include/coatheal/telemetry.hpp`](../onboard/include/coatheal/telemetry.hpp)**

`SerializeTelemetryDataFrame(record, session_id)` produces the Rev B.1 DATA frame:

```
DATA,<session>,<seq>,<ts>,<rtc_valid>,<ambient_temp_c>,<ambient_pressure_mbar>,<uv>,<sample_0>..<sample_7>,HEATER_DUTY=d0|..|d5,RESISTANCE=r0|..|r7,PHASE=..,MODE=..,STATUS=..,STEPPER0=..,STEPPER1=..
```

Humidity and box_temp columns are **not** emitted. `RESISTANCE=` is new; unmeasured samples render as `-`. See [docs/protocol.md](protocol.md) for full schema.

`SerializeTelemetryPullEventFrame(event, session_id)` produces `EVT,PULL,...` frames; `SerializeHeatingCycleEvent(...)` (`EVT,CYCLE,...`) is retained for legacy compatibility but is not emitted under Rev B.1 since the activation ramp is gone.

---

## StatusFlags

**[`onboard/src/status_flags.cpp`](../onboard/src/status_flags.cpp)** | **[`onboard/include/coatheal/status_flags.hpp`](../onboard/include/coatheal/status_flags.hpp)**

`ToStatusBitfield(flags)` emits 13 pipe-separated tokens:

```
SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK|T_AMBIENT_OK|P_AMBIENT_OK|UNIFORMITY_OK|OVERTEMP_OK|ENERGY_OK|RS485_OK|HEATER_ACTIVE|RESISTANCE_OK
```

Each token flips between `_OK` and `_FAIL` except `HEATER_ACTIVE` / `HEATER_INHIBITED` which is a state bit, not a health bit.
