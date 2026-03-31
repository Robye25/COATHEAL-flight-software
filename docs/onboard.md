# Onboard Software Reference

The onboard software is a C++17 application (`coatheal_onboard`) running on a Raspberry Pi 4. It autonomously controls the COATHEAL experiment from power-on through mission completion.

## Entry Point

**`onboard/src/main.cpp`**

Parses `--config <path>` argument, calls `LoadConfigFromIni`, constructs `SystemController`, calls `Initialize`, then `Run`. Exits with the return value of `Run`.

```cpp
./build/onboard/coatheal_onboard --config config/onboard.example.ini
```

---

## SystemController

**`onboard/src/system_controller.cpp`** | **`onboard/include/coatheal/system_controller.hpp`**

The master orchestrator. Owns and coordinates all subsystems. The main loop runs at `tick_hz` (default 1 Hz).

### Key Methods

| Method | Description |
|---|---|
| `Initialize(error*)` | Initializes all subsystems; starts command server thread |
| `Run()` | Blocking main loop; returns exit code |
| `DrainTelemetryQueue(link_ok*, error*)` | Sends all pending frames to ground station, updates link status |
| `HandleCommandLine(line)` | Dispatches incoming command strings; called from command server thread |

### Override Flags

Commands from the ground station set thread-safe flags (`std::mutex overrides_mu_`) that are read and cleared at the top of each tick. This ensures commands never interrupt sensor reads or heater updates mid-tick.

| Flag | Set by | Effect |
|---|---|---|
| `force_start` | FORCE_START | Transitions to ACTIVATION_RAMP next tick |
| `force_stop` | FORCE_STOP | Transitions to STOPPED next tick |
| `reset_control` | RESET_CTRL | Calls `ThermalController::Reset()` next tick |
| `shutdown_safe` | SHUTDOWN_SAFE | Sets `running_ = false`, stops loop |
| `heaters_off` | HEATERS_OFF | Zeroes all heater duties via ControlOverrides |
| `single_heater_override` | SET_HEATER_DUTY | Sets duty for one heater index |
| `all_heaters_override` | SET_ALL_DUTY | Sets duty for all heaters |
| `pid_override` | SET_PID | Overrides sample PID gains |

---

## StateManager

**`onboard/src/state_manager.cpp`** | **`onboard/include/coatheal/state_manager.hpp`**

Implements the mission phase finite state machine. Transitions are based on:

- **Pressure** (from BME280): ascent→activation at `ascent_to_activation_mbar` (140 mbar), float→descent at `float_to_descent_mbar` (300 mbar)
- **Float duration**: descent triggered after `float_hold_minutes` (90 min) at float altitude
- **Overrides**: `force_start`, `force_stop`, `shutdown_safe` from command handler

### `Update(pressure, sample_temps, overrides, now)`

Called every tick. Returns the current `MissionPhase`. Manages internal timers for float hold duration.

---

## ThermalController

**`onboard/src/thermal_controller.cpp`** | **`onboard/include/coatheal/thermal_controller.hpp`**

Computes requested heater duty cycles using two PID controllers.

### PID Controllers

| Controller | Target | Feedback | Controls |
|---|---|---|---|
| Sample PID | Phase-dependent setpoint | Sample temperatures (avg or per-sensor) | Heaters 0–8 |
| Box PID | `phase.box_target_c` | `box_temp_c` | Heater 9 (electronics) |

### Setpoint Schedule

| Phase | Sample setpoint |
|---|---|
| ASCENT_HOLD | `ascent_target_c` (−30 °C) |
| ACTIVATION_RAMP | Ramping at `activation_ramp_c_per_s` (0.85 °C/s) |
| FLOAT_HOLD | `float_target_c` (+70 °C) |
| DESCENT_FLOOR | `descent_floor_c` (−20 °C) |
| STOPPED | 0.0 (all off) |

### `ComputeRequestedDuty(phase, snapshot, dt, overrides)`

Returns `std::vector<double>` of requested duties (0.0–1.0) for all heaters. Overrides from the command handler (`heaters_off`, `single_heater_override`, `all_heaters_override`, `pid_override`) are applied before returning.

### `Reset()`

Resets both PID integrators. Called by RESET_CTRL command.

---

## HeaterScheduler

**`onboard/src/heater_scheduler.cpp`** | **`onboard/include/coatheal/heater_scheduler.hpp`**

Enforces power and heater count constraints on the requested duties.

### Constraints

| Parameter | Default | Description |
|---|---|---|
| `max_active_heaters` | 4 | Maximum simultaneous active heaters |
| `max_thermal_w` | 40 W | Maximum combined thermal power |
| `heater_nominal_w` | 10 W | Nominal power per heater at 100% duty |

### Algorithm

1. Rank heaters by requested duty (highest first), with electronics heater de-prioritized
2. Accept heaters greedily until `max_active_heaters` or `max_thermal_w` is reached
3. Clamp accepted duties to the remaining power headroom
4. Zero all rejected heaters

### `Schedule(requested_duty, prioritize_samples)`

Returns constrained `std::vector<double>`. During `ACTIVATION_RAMP`, `prioritize_samples = true` which further de-prioritizes the electronics heater.

---

## PidController

**`onboard/src/pid_controller.cpp`** | **`onboard/include/coatheal/pid_controller.hpp`**

Standard discrete PID with anti-windup and output clamping.

```
output = kp × error + ki × integral + kd × derivative
```

- **Integral anti-windup:** Integral is clamped to `[-10, 10]`
- **Output clamping:** Output is clamped to `[0.0, 1.0]` (duty cycle)
- **`Update(setpoint, measurement, dt)`:** Returns clamped duty
- **`Reset()`:** Zeroes integral and derivative state

---

## SensorManager

**`onboard/src/sensor_manager.cpp`** | **`onboard/include/coatheal/sensor_manager.hpp`**

Reads all sensors and returns a `SensorSnapshot`. In bench/simulation mode, uses a physics-based simulation model rather than real hardware.

### SensorSnapshot Fields

| Field | Source | Units |
|---|---|---|
| `timestamp` | RTC or system clock | ISO-8601 UTC string |
| `rtc_valid` | RTC adapter | bool |
| `ambient_temp_c` | BME280 (I2C) | °C |
| `ambient_pressure_mbar` | BME280 (I2C) | mbar |
| `ambient_humidity_pct` | BME280 (I2C) | % |
| `uv` | BPW21 via ADS1115 (I2C) | normalized float |
| `box_temp_c` | Local thermistor or simulation | °C |
| `sample_temps_c` | PT100 × N via MIKROE-2815 (SPI) | °C (vector) |

### Simulation Model

When `use_simulated_pwm = false` and hardware is absent, `SensorManager` uses a simplified thermal model: samples are initialized at ambient temperature and heated toward the setpoint based on applied duty. Pressure decreases linearly over time (simulating ascent). This allows full software testing without hardware.

---

## TelemetryClient

**`onboard/src/telemetry_client.cpp`** | **`onboard/include/coatheal/telemetry_client.hpp`**

Manages the outbound TCP connection to the ground station.

### Session ID

Generated at startup: `<hostname>-<unix_seconds>-<monotonic_ns % 1000000>`. Uniquely identifies one run of the onboard process. The ground station uses this to distinguish sessions for deduplication.

### `SendFrameAwaitAck(frame, ack*)`

1. If not connected, calls `ConnectLocked()`
2. Sends the frame string + `\n`
3. Waits up to `reconnect_ms` for ACK line
4. Parses ACK; if session_id or seq mismatches, treats as failure
5. On any failure, calls `CloseLocked()` and returns false

### `ConnectLocked()`

1. If `discovery_enabled`, calls `DiscoverGroundHostLocked()` (3 attempts × `reconnect_ms` timeout each)
2. Falls back to `static_ground_ip` if discovery fails
3. Performs TCP `connect()` to resolved host and `telemetry_port`

### `DiscoverGroundHostLocked()`

Binds a UDP socket to `discovery_port` (4100) and listens for `GS_HELLO` broadcasts from the ground station. On receipt, replies with `ONBOARD_HELLO` and returns the sender's IP.

---

## TelemetryQueue

**`onboard/src/telemetry_queue.cpp`** | **`onboard/include/coatheal/telemetry_queue.hpp`**

Persistent, disk-backed FIFO queue for telemetry frames. Survives process restarts.

### Storage Format

Each frame is written as a JSON-like record file in `queue_dir`. Files are named by sequence number and session ID for ordered replay.

### Retention Policy

- Maximum age: `queue_retention_hours` (default 72 hours)
- Maximum size: `queue_max_bytes` (default 8 GiB)
- Old or excess frames are pruned on startup and periodically

### Key Methods

| Method | Description |
|---|---|
| `Enqueue(frame, error*)` | Persists frame to disk queue |
| `PendingFrames()` | Returns all unacked frames in order |
| `Acknowledge(session_id, seq, error*)` | Marks frames up to seq as acknowledged; deletes their files |
| `size()` | Returns count of pending frames |

---

## StorageManager

**`onboard/src/storage_manager.cpp`** | **`onboard/include/coatheal/storage_manager.hpp`**

Writes telemetry lines to two independent CSV paths (primary: SD card, secondary: USB drive).

### Behavior

- Writes a `# COATHEAL telemetry log` header on the first write
- If one path fails to open, continues writing to the other
- Tracks `sd_ok` and `usb_ok` status flags for telemetry
- The `wrote_header_` flag is only set true if at least one path successfully wrote the header, preventing header-less CSV files after partial failures

### `WriteLine(line)`

Appends `line + '\n'` to both paths. Updates status flags accordingly.

---

## CommandServer

**`onboard/src/command_server.cpp`** | **`onboard/include/coatheal/command_server.hpp`**

TCP server on `command_port` (default 5000). Accepts one client at a time.

### `Start(handler, error*)`

Starts a background thread. On each accepted connection:
1. Reads one line (the command string)
2. Passes it to `handler(line)` → returns response string
3. Sends response + `\n`
4. Closes connection

### Thread Safety

The handler is called from the command server background thread. SystemController's `HandleCommandLine` is the registered handler and uses `std::lock_guard` on `overrides_mu_` when setting override flags.

---

## CommandParser

**`onboard/src/command_parser.cpp`** | **`onboard/include/coatheal/command_parser.hpp`**

Parses a raw command string into a `Command` struct.

### `ParseLine(line)`

1. Trims whitespace
2. Replaces commas with spaces (allows comma-separated args)
3. Splits into tokens, uppercases the first token (command name)
4. Looks up command type in map (including aliases: ON, OFF, RESET)
5. Validates argument count
6. Returns `CommandParseResult {ok, command, error}`

---

## HAL Adapters

Hardware Abstraction Layer stubs in `onboard/src/hal/` and `onboard/include/coatheal/hal/`.

### Current Status

| Adapter | Status | Description |
|---|---|---|
| `SpiAdapter` | **Stub** | Tracks healthy/unhealthy state; no real SPI reads |
| `I2cAdapter` | **Stub** | Tracks healthy/unhealthy state; no real I2C reads |
| `RtcAdapter` | **Stub** | Returns system clock time; no external RTC chip |
| `LibgpiodPwmController` | **Implemented** | Real PWM via `libgpiod`; GPIO pin mapping needed |
| `SimulatedPwmController` | **Implemented** | In-memory duty array for bench testing |

### Pending Implementation

The following drivers need to be written before flight:

- **MIKROE-2815 SPI RTD driver** — read 10× PT100 temperatures via `spi_adapter`
- **BME280 I2C driver** — ambient temperature, pressure, humidity via `i2c_adapter`
- **ADS1115 I2C driver** — UV sensor ADC via `i2c_adapter`
- **External RTC I2C driver** — RTC synchronization via `rtc_adapter`
- **GPIO pin mapping** — assign libgpiod GPIO lines to heater MOSFET channels

See [docs/hardware.md](hardware.md) for interface specifications.

---

## Telemetry Serializer

**`onboard/src/telemetry.cpp`** | **`onboard/include/coatheal/telemetry.hpp`**

`SerializeTelemetryDataFrame(record, session_id)` produces the DATA frame string. See [docs/protocol.md](protocol.md) for the full format.

---

## StatusFlags

**`onboard/src/status_flags.cpp`** | **`onboard/include/coatheal/status_flags.hpp`**

`ToStatusBitfield(flags)` serializes a `StatusFlags` struct to `SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK` (or `_FAIL` variants). Used in telemetry frame STATUS field.
