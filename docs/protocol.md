# Wire Protocol Specification (Rev B.1)

All messages are UTF-8 encoded, newline (`\n`) terminated, and transmitted over TCP or UDP.

---

## Telemetry DATA frame

Sent by the onboard over TCP (port 4000) once per tick.

### Format (Rev B.1)

```
DATA,<session_id>,<seq>,<timestamp>,<rtc_valid>,<ambient_temp_c>,<ambient_pressure_mbar>,<uv>,<sample_0>,...,<sample_7>,HEATER_DUTY=<d0>|...|<d5>,RESISTANCE=<r0>|...|<r7>,PHASE=<phase>,MODE=<mode>,STATUS=<flags>,STEPPER0=<kv>,STEPPER1=<kv>\n
```

Rev B.1 deltas vs. Rev B:

- **Humidity column removed** (MS5803-01BA has no humidity output).
- **`box_temp_c` column removed** (no box temperature sensor; the 2 × 4-ch Modbus collectors cover the 8 samples exactly).
- `HEATER_DUTY=` carries **6** values (samples 0..5), down from 9. Samples 6 and 7 are pulled but unheated and have no heater column.
- **New `RESISTANCE=` segment** after `HEATER_DUTY=`. Carries one value per sample (8 values total, same index space as `sample_temps_c`). Unmeasured samples — samples 6 and 7 plus any sample the INA3221 stub returns zero for — are emitted as `-` rather than a number.
- **STATUS gains a trailing `RESISTANCE_OK`/`RESISTANCE_FAIL` bit.** Bit is driven by `Ina3221Adapter::healthy()`.
- `STEPPER0=…` / `STEPPER1=…` unchanged from Rev B. The legacy single-`STEPPER=` form is **retired in the onboard serializer** (Rev B.1 is a breaking wire change); the ground-side parser may still accept it for old log replay.
- `EVT,PULL,…` event frame unchanged from Rev B.

The parser locates `HEATER_DUTY=` by token name (not column index), so sample count is inferred from the position of `HEATER_DUTY=`. Frames with any number of `sample_i` columns parse as long as every column before `HEATER_DUTY=` is a float.

### Fields

| Index | Field | Type | Description |
|---|---|---|---|
| 0 | `DATA` | literal | Frame type marker |
| 1 | `session_id` | string | `<hostname>-<unix_s>-<mono_ns%1e6>` |
| 2 | `seq` | uint64 | Monotonically increasing sequence number per session |
| 3 | `timestamp` | string | UTC ISO-8601 (`YYYY-MM-DDTHH:MM:SSZ`) |
| 4 | `rtc_valid` | 0/1 | Whether the RTC is synchronised |
| 5 | `ambient_temp_c` | float | MS5803-01BA ambient temperature (°C) |
| 6 | `ambient_pressure_mbar` | float | MS5803-01BA ambient pressure (mbar) |
| 7 | `uv` | float | UV irradiance from GUVA-S12SD through ADS1015 (normalised) |
| 8…15 | `sample_i` | float | 8 PT100 sample temperatures (°C) |
| 16 | `HEATER_DUTY=…` | key=value | Pipe-separated heater duty cycles, **6 values** (indices 0..5) |
| 17 | `RESISTANCE=…` | key=value | Pipe-separated sample resistance (Ω), **8 values**; unmeasured = `-` |
| 18 | `PHASE=…` | key=value | `BOOT` / `ASCENT` / `FLOAT` / `DESCENT` / `LANDED` / `STOPPED` |
| 19 | `MODE=…` | key=value | `STANDBY` / `RUN` / `SAFE` |
| 20 | `STATUS=…` | key=value | 13 pipe-separated status tokens (see below) |
| 21 | `STEPPER0=…` | key=value | Motor 0 snapshot |
| 22 | `STEPPER1=…` | key=value | Motor 1 snapshot |

### `HEATER_DUTY` format

```
HEATER_DUTY=0.000|0.250|0.000|0.500|0.000|0.000
```

6 values. Index 0..5 = sample heaters (5 W @ 24 V polyimide film each).

### `RESISTANCE` format

```
RESISTANCE=420.1|418.7|415.2|410.0|408.3|405.9|-|-
```

8 values, pipe-separated. A numeric value is the last resistance measurement from the INA3221 pair for that sample (ohms). `-` means the sample has no assigned INA3221 channel or the reading is unavailable; in the Rev B.1 BOM samples 6 and 7 always render as `-` because only 6 INA3221 channels exist (2 chips × 3 channels).

### `PHASE` values (Rev B)

| String | Description |
|---|---|
| `BOOT` | Onboard starting up; sensors warming. |
| `ASCENT` | Balloon ascending; floor-only control (+5 °C). |
| `FLOAT` | At float altitude; pulls happen here. |
| `DESCENT` | Descending; floor-only control (+5 °C). |
| `LANDED` | Parachute deployed / on ground. |
| `STOPPED` | Mission complete / safe shutdown. |

Rev A phase strings (`ASCENT_HOLD_-30C`, `ACTIVATION_RAMP_+70C`, `FLOAT_HOLD_+70C`, `DESCENT_FLOOR_-20C`) remain parseable on the ground station for log replay only.

### `STATUS` flags (Rev B.1)

```
STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK|T_AMBIENT_OK|P_AMBIENT_OK|UNIFORMITY_OK|OVERTEMP_OK|ENERGY_OK|RS485_OK|HEATER_ACTIVE|RESISTANCE_OK
```

Each token flips between `<NAME>_OK` and `<NAME>_FAIL`, except `HEATER_ACTIVE` / `HEATER_INHIBITED` which is a state (not a health) bit.

| Flag | Description |
|---|---|
| `SD_OK` / `SD_FAIL` | Primary SD-card CSV log healthy. |
| `USB_OK` / `USB_FAIL` | Secondary USB mirror log healthy. |
| `I2C_OK` / `I2C_FAIL` | I2C bus (MS5803, ADS1015, INA3221 ×2, RTC) healthy. |
| `SPI_OK` / `SPI_FAIL` | SPI bus (TMC2240 configuration) healthy. |
| `LINK_OK` / `LINK_FAIL` | Telemetry link (last ACK received successfully). |
| `T_AMBIENT_OK` / `T_AMBIENT_FAIL` | MS5803 temperature inside `sensor.ambient_temp_*` window. |
| `P_AMBIENT_OK` / `P_AMBIENT_FAIL` | MS5803 pressure inside `sensor.ambient_pressure_*` window. |
| `UNIFORMITY_OK` / `UNIFORMITY_FAIL` | Spread across the **6 heated** samples within `phase.uniformity_tolerance_c`. |
| `OVERTEMP_OK` / `OVERTEMP_FAIL` | No per-channel over-temperature latch tripped. |
| `ENERGY_OK` / `ENERGY_FAIL` | Heater energy budget not exhausted (`power.energy_budget_wh`). |
| `RS485_OK` / `RS485_FAIL` | RS-485 bus (USB-RS485 + Modbus PT100 collectors) healthy. |
| `HEATER_ACTIVE` / `HEATER_INHIBITED` | **State bit.** `HEATER_INHIBITED` while the `MotionLock` is held (any pull in progress); `HEATER_ACTIVE` otherwise. |
| `RESISTANCE_OK` / `RESISTANCE_FAIL` | **Rev B.1.** `Ina3221Adapter::healthy()` — the sample-resistance science instrument is usable. |

### Example (Rev B.1)

```
DATA,coatheal-1718000000-123456,42,2026-04-16T12:00:00Z,1,-10.23,140.12,0.00012,5.1,5.2,5.0,5.3,5.1,5.2,5.0,5.3,HEATER_DUTY=0.250|0.000|0.250|0.000|0.000|0.050,RESISTANCE=420.1|418.7|415.2|410.0|408.3|405.9|-|-,PHASE=FLOAT,MODE=RUN,STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK|T_AMBIENT_OK|P_AMBIENT_OK|UNIFORMITY_OK|OVERTEMP_OK|ENERGY_OK|RS485_OK|HEATER_ACTIVE|RESISTANCE_OK,STEPPER0=pos:100|tgt:200|hz:100.00|us:4|en:1|mv:1|hold:0|hold_s:0.00|pulses:100|src:cmd:MOVE,STEPPER1=pos:0|tgt:0|hz:0.00|us:4|en:1|mv:0|hold:0|hold_s:0.00|pulses:0|src:init
```

---

## Heating Cycle Event frame (`EVT,CYCLE`) — legacy

Legacy Rev A frame retained in the serializer for log-replay compatibility but **not emitted at Rev B.1** (the activation ramp that produced these events is gone — microcrack formation is now mechanical, not thermal). Format unchanged from Rev B.

```
EVT,CYCLE,<session_id>,<cycle_id>,<start_ts>,<peak_temp_c>,<hold_duration_s>,<cooldown_rate_c_per_s>,<specimen_index>\n
```

---

## Pull-Cycle Event frame (`EVT,PULL`)

Emitted once per completed pull by `SystemController::Run` on the falling edge of a channel's `moving` flag. Uses the same framing and ACK flow as `DATA`.

### Format

```
EVT,PULL,<session_id>,<pull_id>,<motor_id>,<start_ts>,<steps_moved>,<hold_s>,<samples>\n
```

### Fields

| Index | Field | Type | Description |
|---|---|---|---|
| 0 | `EVT` | literal | Event frame marker |
| 1 | `PULL` | literal | Event subtype |
| 2 | `session_id` | string | Onboard session that produced the event |
| 3 | `pull_id` | uint32 | Monotonic pull counter per session |
| 4 | `motor_id` | uint | `0` = M0, `1` = M1 |
| 5 | `start_ts` | string | UTC ISO-8601 at pull start |
| 6 | `steps_moved` | int64 | Signed net step delta (`final_pos − start_pos`) |
| 7 | `hold_s` | float | Wall-clock seconds the motor held at target before releasing |
| 8 | `samples` | string | Pipe-separated specimen indices the pull touched (e.g. `0|1|2|3`), or `-` if empty |

### Emission site

Serializer: [`SerializeTelemetryPullEventFrame`](../onboard/src/telemetry.cpp). Driver: the falling-edge detector in [`system_controller.cpp`](../onboard/src/system_controller.cpp) (`pull_state_`, which also fires `SensorManager::NotePullCompleted(motor_id)` so the resistance simulator steps).

### Ground-side behaviour

`telemetry_server.py` and the GUI's `TelemetryReceiver` intercept `EVT,PULL,…` lines ahead of the DATA parser. The server appends to a sibling `<log>_pulls.csv`; the GUI populates the "Pull events" bottom-dock tab. Both ACK with cumulative `ACK,<session_id>,0\n`.

### Example

```
EVT,PULL,coatheal-1718000000-123456,3,1,2026-04-16T10:21:00Z,200,5.00,4|5|6|7
```

200 full-steps = 1 revolution = **1 mm linear pull** on the OMC ball screw. Motor 1 pulls samples 4–7 (samples 6 and 7 are unheated but still part of the pulled group).

---

## ACK frame

Sent by the ground station to the onboard for each valid, non-duplicate telemetry packet. ACKs are cumulative per session — the ground station tracks the highest received `seq` per session.

### Format

```
ACK,<session_id>,<seq>\n
```

### Session model

A session = one run of the onboard process (`session_id` generated at startup). If the onboard restarts a new session begins. The ground station tracks a cursor of `{session_id → last_seq}` in `logs/ground_ack_cursor.json`. Packets with `seq ≤ last_seq` for the same session are ACK'd (so the onboard clears its queue) but not rewritten to CSV.

---

## UDP discovery

### `GS_HELLO` (ground → broadcast)

```
GS_HELLO,<nonce>,<telemetry_port>,<command_port>\n
```

### `ONBOARD_HELLO` (onboard → unicast reply)

```
ONBOARD_HELLO,<nonce>,<session_id>,<hostname>,<command_port>,<telemetry_port>\n
```

Flow: ground sends `GS_HELLO` every ~1 s on UDP 4100 → onboard validates nonce → onboard replies `ONBOARD_HELLO` → ground caches source IP in `logs/discovered_onboard.json`.

If discovery fails, both sides fall back to the static IPs in `onboard.ini` (`static_ground_ip`, `static_pi_ip`).

> **Note:** `gui_app.py` does not currently implement the UDP discovery beacon. Use `--host` or rely on the static IP.

---

## Command protocol

Commands are sent from the ground station to the onboard over TCP (port 5000). Each command is one newline-terminated line. The onboard replies with one newline-terminated line and closes the connection.

### Request / response

```
<COMMAND> [<arg1> [<arg2> ...]]\n
```

- Success: `ACK,<COMMAND>,<message>\n`
- Failure: `NACK,<COMMAND>,<reason>\n`

### Flight-safe commands (always available)

| Command | Args | Response | Description |
|---|---|---|---|
| `PING` | — | `ACK,PING,pong` | Liveness check |
| `STATUS` | — | `ACK,STATUS,phase=...;bench_mode=...;debug_armed=...;telemetry_target=...;queue_depth=...;tick_hz=...;energy_wh=...;energy_budget_wh=...;budget_exhausted=...` | System status |
| `FORCE_START` | — | `ACK,FORCE_START,override accepted` | Force transition into `ASCENT` (Rev B) |
| `FORCE_STOP` | — | `ACK,FORCE_STOP,override accepted` | Force into `DESCENT` / `STOPPED` |
| `HEATERS_OFF` | — | `ACK,HEATERS_OFF,all heaters disabled` | Emergency: zero all 6 heater duties |
| `RESET_CTRL` | — | `ACK,RESET_CTRL,control loop reset queued` | Reset per-sample PID integrators |
| `SHUTDOWN_SAFE` | — | `ACK,SHUTDOWN_SAFE,safe shutdown queued` | Graceful process shutdown |
| `SET_TICK_HZ` | `<hz>` | `ACK,SET_TICK_HZ,tick_hz=<hz>` | Live loop-rate change, `[0.1, 5.0]` Hz |
| `RADIO_SILENCE` | — | `ACK,RADIO_SILENCE,radio silent` | Stop pushing frames over the TX socket (< 1 s). Queue keeps filling. |
| `RADIO_RESUME` | — | `ACK,RADIO_RESUME,radio resumed` | Re-enable transmission; drain in order on next tick. |
| `ARM_DEBUG` | `<token>` | `ACK,ARM_DEBUG,debug armed` | Arm extended debug commands (bench mode only) |

Aliases: `ON` = `FORCE_START`, `OFF` = `FORCE_STOP`, `RESET` = `RESET_CTRL`.

### Extended commands (bench mode + debug armed)

| Command | Args | Description |
|---|---|---|
| `DISARM_DEBUG` | — | Disarm debug mode |
| `SET_HEATER_DUTY` | `<index> <duty>` | Override single heater. `index` in `0..5`; `duty` in `0.0..1.0`. |
| `SET_ALL_DUTY` | `<duty>` | Override all 6 heaters. |
| `SET_PID` | `<kp> <ki> <kd>` | Override per-sample PID gains. |
| `CLEAR_OVERRIDES` | — | Clear all heater and PID overrides. |
| `SET_BENCH_MODE` | `<1\|0>` | Toggle bench mode. |

### Error responses

| Reason | Description |
|---|---|
| `bench mode required` | Extended command sent outside bench mode |
| `debug arm required` | Extended command sent without `ARM_DEBUG` |
| `invalid arm token` | `ARM_DEBUG` token does not match `debug_arm_code` |
| `heater index out of range` | `SET_HEATER_DUTY` index ≥ `heater_count` |
| `invalid argument count for <CMD>` | Wrong number of arguments |
| `unknown command: <CMD>` | Unrecognised command name |
| `hz out of range [0.1,5.0]` | `SET_TICK_HZ` out of band |
| `invalid hz value` | `SET_TICK_HZ` not parseable |

### Example session

```
→ PING\n
← ACK,PING,pong\n

→ STATUS\n
← ACK,STATUS,phase=FLOAT;bench_mode=1;debug_armed=0;telemetry_target=169.254.251.200;queue_depth=0;tick_hz=1.00;energy_wh=3.21;energy_budget_wh=130.00;budget_exhausted=0\n

→ ARM_DEBUG COATHEAL_DEBUG\n
← ACK,ARM_DEBUG,debug armed\n

→ SET_HEATER_DUTY 0 0.5\n
← ACK,SET_HEATER_DUTY,override applied\n
```

---

## Stepper motors (sample-pulling actuators)

Two OMC 17E19S2504BSM5-150RS integrated ball-screw NEMA-17 motors (1 mm lead), each driven by a **TMC2240**. Motor 0 owns samples 0–3, motor 1 owns samples 4–7. Only one motor may pull at a time — `MotionLock` enforces this, and the heater scheduler zeros all duty while the lock is held (`HEATER_INHIBITED`).

### Motion envelope

- 200 full-steps / revolution.
- **1 mm linear per revolution** (OMC integrated ball screw, 1 mm lead).
- Max pull rate 100 full-steps/s (≈30 rpm ≈ 0.5 mm/s linear).
- Default accel 200 full-steps/s² — ramps 0 → 100 Hz in 0.5 s.
- Microstep 4× default (800 µsteps/rev = 800 µsteps/mm), configurable to 5×.

### Commands

All commands accept an **optional leading motor id** (integer 0 or 1). If omitted, id defaults to 0.

| Command | Args | Description |
|---|---|---|
| `STEPPER_MOVE <id> <steps>` | int, signed int | Relative move |
| `STEPPER_MOVETO <id> <abs_steps> [hold_s]` | int, signed int, optional float | Absolute move with optional hold |
| `STEPPER_ROTATE <id> <revs>` | int, signed float | Rotate by N full revolutions (= N mm linear) |
| `STEPPER_BEND <id> <steps> [hold_s]` | int, signed int, optional float | Alias for MOVETO, tagged as a bend |
| `STEPPER_HOME <id>` | int | Return to position 0 |
| `STEPPER_STOP <id>` | int | Abort motion, release `MotionLock` |
| `STEPPER_SET_SPEED <id> <hz>` | int, positive float | Full-step Hz, clamped to `pull.max_step_hz` |
| `STEPPER_SET_MICROSTEP <id> <n>` | int, int | 4 or 5 by default |
| `STEPPER_ENABLE <id>` / `STEPPER_DISABLE <id>` | int | Gate the driver /EN line |
| `PULL_ARM <id>` | int | Acquire `MotionLock` and queue a pull cycle (non-blocking) |
| `PULL_EXECUTE <id>` | int | Acquire lock and run one full pull+hold+retract synchronously |

A pull cycle = move to `pull.travel_full_steps × microstep` (default 200 full-steps = 1 rev = 1 mm) → hold for `pull.hold_s` (default 5 s) → retract to position 0 → release the lock. Completion emits `[pull] cycle complete id=<id> samples=<list>` in the log and an `EVT,PULL,…` frame on the wire.

### Per-motor telemetry segment

```
STEPPER<n>=pos:<position_steps>|tgt:<target_steps>|hz:<step_hz>|us:<microstep>|en:<0|1>|mv:<0|1>|hold:<0|1>|hold_s:<remaining>|pulses:<total>|src:<tag>
```

`src` records what last changed the setpoint (`phase:FLOAT`, `cmd:MOVE`, `cmd:BEND`, `cmd:HOME`, `cmd:STOP`, `cmd:PULL`, `init`).

### Configuration

See [docs/configuration.md](configuration.md). The `[stepper]` block is honored as a Rev A fallback that routes to channel 0; the `[pull]`, `[motor0]`, and `[motor1]` blocks are parsed and ignored at Rev B.1 (compiled-in defaults cover both channels) until the multi-channel config schema is wired through `config.cpp`.
