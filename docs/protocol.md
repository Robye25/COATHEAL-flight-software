# Wire Protocol Specification

All messages are UTF-8 encoded, newline (`\n`) terminated, and transmitted over TCP or UDP.

---

## Telemetry Data Frame

Sent by the onboard over TCP (port 4000) once per tick.

### Format (Rev-B)

```
DATA,<session_id>,<seq>,<timestamp>,<rtc_valid>,<ambient_temp_c>,<ambient_pressure_mbar>,<ambient_humidity_pct>,<uv>,<box_temp_c>,<sample_0>,...,<sample_7>,HEATER_DUTY=<d0>|<d1>|...|<d8>,PHASE=<phase>,MODE=<mode>,STATUS=<flags>,STEPPER0=<kv>,STEPPER1=<kv>\n
```

Rev-B changes (vs. Rev-A):

- 8 `sample_i` columns instead of 9.
- `HEATER_DUTY=` carries **9** values (8 sample heaters + 1 electronics BOX heater).
- Two stepper segments per frame: `STEPPER0=` and `STEPPER1=` (one per motor).
  The single-segment legacy form `STEPPER=…` is still accepted by
  `parse_telemetry_csv` for old SD-card logs.
- New `PHASE` vocabulary (see below) and additional `STATUS` bits.

The parser locates `HEATER_DUTY=` by token name (it is not at a fixed column
index), so the number of sample temperatures is **inferred from the position
of `HEATER_DUTY=`**. Frames with any number of sample_i columns parse as long
as every column before `HEATER_DUTY=` is a float.

### Fields

| Index | Field | Type | Description |
|---|---|---|---|
| 0 | `DATA` | literal | Frame type marker |
| 1 | `session_id` | string | Unique session identifier (`<hostname>-<unix_s>-<mono_ns%1e6>`) |
| 2 | `seq` | uint64 | Monotonically increasing sequence number (per session) |
| 3 | `timestamp` | string | UTC ISO-8601 timestamp (`YYYY-MM-DDTHH:MM:SSZ`) |
| 4 | `rtc_valid` | 0/1 | Whether the RTC is synchronized and valid |
| 5 | `ambient_temp_c` | float | BME280 ambient temperature (°C) |
| 6 | `ambient_pressure_mbar` | float | BME280 ambient pressure (mbar) |
| 7 | `ambient_humidity_pct` | float | BME280 relative humidity (%) |
| 8 | `uv` | float | UV irradiance from BPW21/ADS1115 (normalized) |
| 9 | `box_temp_c` | float | Electronics box temperature (°C) |
| 10…17 | `sample_i` | float | PT100 sample temperatures (°C), 8 channels (Rev-B) |
| 18 | `HEATER_DUTY=…` | key=value | Pipe-separated heater duty cycles (0.0–1.0), **9 values** |
| 19 | `PHASE=…` | key=value | Current mission phase string |
| 20 | `MODE=…` | key=value | System mode (`STANDBY`, `RUN`, `SAFE`) |
| 21 | `STATUS=…` | key=value | Pipe-separated status flags (see below) |
| 22 | `STEPPER0=…` | key=value | Motor 0 snapshot |
| 23 | `STEPPER1=…` | key=value | Motor 1 snapshot |

### HEATER_DUTY Format

```
HEATER_DUTY=0.000|0.250|0.000|0.500|0.000|0.000|0.000|0.000|0.000
```

9 values. Indices 0–7 = sample heaters, index 8 = electronics box heater.

### PHASE Values (Rev-B placeholders)

Agent A is reshaping mission phases: thermal *targets* are gone — only a
floor (+5 °C) is enforced — so the old `_±XC` suffixes are dropped. The
string values are:

| String | Description |
|---|---|
| `BOOT`    | Onboard starting up; sensors warming / self-tests running |
| `ASCENT`  | Balloon ascending (floor-only control, +5 °C) |
| `FLOAT`   | At float altitude; sample pulls happen here |
| `DESCENT` | Descending (floor-only control, +5 °C) |
| `LANDED`  | Parachute deployed / on ground |
| `STOPPED` | Mission complete or safe shutdown |

Rev-A phase strings (`ASCENT_HOLD_-30C`, `ACTIVATION_RAMP_+70C`,
`FLOAT_HOLD_+70C`, `DESCENT_FLOOR_-20C`, `STOPPED`) remain parseable for
log replay.

### STATUS Flags

```
STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK|T_AMBIENT_OK|P_AMBIENT_OK|UNIFORMITY_OK|OVERTEMP_OK|ENERGY_OK|RS485_OK|HEATER_ACTIVE
```

Each flag is either `<NAME>_OK` or `<NAME>_FAIL`, except the Rev-B
`HEATER_INHIBITED` / `HEATER_ACTIVE` bit which is a state (not a health)
indicator.

| Flag | Description |
|---|---|
| `SD_OK` / `SD_FAIL` | Primary SD card CSV log healthy |
| `USB_OK` / `USB_FAIL` | Secondary USB mirror log healthy |
| `I2C_OK` / `I2C_FAIL` | I2C bus (BME280, ADS1115, RTC) healthy |
| `SPI_OK` / `SPI_FAIL` | SPI bus (MIKROE-2815 PT100 adapters) healthy |
| `LINK_OK` / `LINK_FAIL` | Telemetry link (last ACK received successfully) |
| `T_AMBIENT_OK` / `T_AMBIENT_FAIL` | BME280 temperature sample within expected range |
| `P_AMBIENT_OK` / `P_AMBIENT_FAIL` | BME280 pressure sample within expected range |
| `UNIFORMITY_OK` / `UNIFORMITY_FAIL` | Specimen bank uniformity within tolerance |
| `OVERTEMP_OK` / `OVERTEMP_FAIL` | No over-temperature latch tripped |
| `ENERGY_OK` / `ENERGY_FAIL` | Heater energy budget not exhausted |
| `RS485_OK` / `RS485_FAIL` | **Rev-B** — RS-485 bus (stepper link) healthy |
| `HEATER_INHIBITED` / `HEATER_ACTIVE` | **Rev-B** — set by the scheduler while a pull is active; the orchestrator (Agent D) holds heaters off for the pull duration |

### Example (Rev-B)

```
DATA,coatheal-1718000000-123456,42,2026-04-16T12:00:00Z,1,-10.23,140.12,12.4,0.00012,3.10,5.1,5.2,5.0,5.3,5.1,5.2,5.0,5.3,HEATER_DUTY=0.250|0.000|0.250|0.000|0.000|0.000|0.000|0.000|0.050,PHASE=FLOAT,MODE=RUN,STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK|T_AMBIENT_OK|P_AMBIENT_OK|UNIFORMITY_OK|OVERTEMP_OK|ENERGY_OK|RS485_OK|HEATER_ACTIVE,STEPPER0=pos:100|tgt:200|hz:400|us:16|en:1|mv:1|hold:0|hold_s:0|pulses:100|src:cmd:MOVE,STEPPER1=pos:-50|tgt:-50|hz:200|us:8|en:1|mv:0|hold:1|hold_s:3.5|pulses:50|src:phase:FLOAT
```

---

## Heating Cycle Event Frame (`EVT,CYCLE`)

Emitted by the onboard once per specimen at the `FLOAT_HOLD → DESCENT_FLOOR`
transition (i.e. when an activation → float → descent arc completes). Uses the
same newline-terminated framing and ACK flow as `DATA` frames.

### Format

```
EVT,CYCLE,<session_id>,<cycle_id>,<start_ts>,<peak_temp_c>,<hold_duration_s>,<cooldown_rate_c_per_s>,<specimen_index>\n
```

### Fields

| Index | Field | Type | Description |
|---|---|---|---|
| 0 | `EVT` | literal | Event frame marker |
| 1 | `CYCLE` | literal | Event subtype: heating cycle completion |
| 2 | `session_id` | string | Onboard session that produced the event |
| 3 | `cycle_id` | uint32 | Monotonic cycle counter (per session) |
| 4 | `start_ts` | string | UTC ISO-8601 timestamp at which `ACTIVATION_RAMP` began |
| 5 | `peak_temp_c` | float | Maximum specimen temperature observed during the cycle (°C) |
| 6 | `hold_duration_s` | float | Wall-clock seconds spent in `FLOAT_HOLD_+70C` |
| 7 | `cooldown_rate_c_per_s` | float | Initial cooldown rate at entry to `DESCENT_FLOOR` (positive = cooling) |
| 8 | `specimen_index` | uint | Zero-based specimen/heater index (0 … heater_count-1) |

Ground-station behaviour: the telemetry server parses these rows out of the
TCP stream, ACKs them like any other frame, and appends to a sibling
`<log>_events.csv` file for post-flight analysis.

### Example

```
EVT,CYCLE,coatheal-1718000000-123456,1,2026-04-13T12:34:56Z,71.42,3600.00,0.0812,3
```

---

## Pull-Cycle Event Frame (`EVT,PULL`) — Rev-B

Emitted once per completed *pull* (a single bend-and-hold actuation of a
motor) by the stepper subsystem. Uses the same framing and ACK flow as
`DATA` / `EVT,CYCLE` frames.

### Format

```
EVT,PULL,<session_id>,<pull_id>,<motor_id>,<start_ts>,<steps_moved>,<hold_s>,<samples>\n
```

### Fields

| Index | Field | Type | Description |
|---|---|---|---|
| 0 | `EVT` | literal | Event frame marker |
| 1 | `PULL` | literal | Event subtype: pull-cycle completion |
| 2 | `session_id` | string | Onboard session that produced the event |
| 3 | `pull_id` | uint32 | Monotonic pull counter (per session) |
| 4 | `motor_id` | uint | `0` = M0, `1` = M1 |
| 5 | `start_ts` | string | UTC ISO-8601 timestamp at which the pull began |
| 6 | `steps_moved` | int64 | Signed net step delta = `final_pos − start_pos` |
| 7 | `hold_s` | float | Wall-clock seconds the motor held at target before releasing |
| 8 | `samples` | string | Pipe-separated list of specimen indices touched by the pull (e.g. `0\|1\|2\|3`), or `-` when the pull touched no specimens |

### Emission site

The serializer lives in `onboard/src/telemetry.cpp` as
`SerializeTelemetryPullEventFrame(...)`. It is analogous to
`SerializeHeatingCycleEvent(...)` and emits the one-line newline-less body;
the caller is responsible for appending `\n` and enqueuing onto the
telemetry queue. Expected emission point: the stepper subsystem (Agent D)
at the completion of each `STEPPER_BEND` motion, after the hold timer
expires and the motor has released.

### Ground-station behaviour

`telemetry_server.py` and the GUI's `TelemetryReceiver` both intercept
`EVT,PULL,…` lines ahead of the DATA parser. The server appends to a
sibling `<log>_pulls.csv` file; the GUI populates the "Pull events" tab
in the bottom dock. Both ACK with cumulative `ACK,<session_id>,0\n` so the
onboard queue clears in order.

### Example

```
EVT,PULL,coatheal-1718000000-123456,3,1,2026-04-16T10:21:00Z,2400,12.00,0|1|2|3
```

---

## ACK Frame

Sent by the ground station to the onboard for each valid, non-duplicate telemetry packet. ACKs are cumulative by session — the ground station tracks the highest received seq per session.

### Format

```
ACK,<session_id>,<seq>\n
```

### Fields

| Field | Description |
|---|---|
| `session_id` | Must match the session_id from the DATA frame being acknowledged |
| `seq` | The sequence number being acknowledged |

### Example

```
ACK,coatheal-1718000000-123456,42
```

### Session Model

A **session** corresponds to one run of the onboard process (`session_id` is generated at startup). If the onboard restarts, a new session begins. The ground station tracks a cursor of `{session_id → last_seq}` in `logs/ground_ack_cursor.json`. Packets with `seq ≤ last_seq` for the same session are treated as duplicates: they are ACK'd (to allow the onboard to clear its queue) but not written to the CSV log again.

---

## UDP Discovery

Used to automatically resolve the ground station IP from the onboard, without manual configuration.

### Ground Station → Broadcast (GS_HELLO)

Sent by the ground station telemetry server periodically on UDP port 4100.

```
GS_HELLO,<nonce>,<telemetry_port>,<command_port>\n
```

| Field | Description |
|---|---|
| `nonce` | Millisecond timestamp string, used to correlate the reply |
| `telemetry_port` | Port the ground station is listening on for TCP telemetry |
| `command_port` | Port the ground station's command client will use |

### Onboard → Ground Station (ONBOARD_HELLO)

Sent by the onboard in reply to a matching GS_HELLO. Sent to the UDP source address of the GS_HELLO.

```
ONBOARD_HELLO,<nonce>,<session_id>,<hostname>,<command_port>,<telemetry_port>\n
```

| Field | Description |
|---|---|
| `nonce` | Must match the nonce from the GS_HELLO |
| `session_id` | Current onboard session identifier |
| `hostname` | Onboard hostname |
| `command_port` | Port the onboard command server is listening on |
| `telemetry_port` | Port the onboard will connect to |

### Discovery Flow

1. Ground station sends `GS_HELLO` broadcast every ~1 second on UDP :4100
2. Onboard receives it, validates nonce, sends `ONBOARD_HELLO` unicast reply
3. Ground station records the source IP and session
4. Ground station switches telemetry receiver IP accordingly
5. Cached result written to `logs/discovered_onboard.json` for reuse after restart

If discovery fails (no reply within timeout), both sides fall back to static IPs configured in `onboard.ini` (`static_ground_ip`, `static_pi_ip`).

> **Note:** The GUI (`gui_app.py`) does not currently implement the UDP discovery beacon. Use the `--host` flag or rely on the static IP.

---

## Command Protocol

Commands are sent from the ground station to the onboard over TCP (port 5000). Each command is a single newline-terminated line. The onboard replies with a single newline-terminated response and closes the connection.

### Request Format

```
<COMMAND> [<arg1> [<arg2> ...]]\n
```

### Response Format

**Success:**
```
ACK,<COMMAND>,<message>\n
```

**Failure:**
```
NACK,<COMMAND>,<reason>\n
```

### Commands Reference

#### Flight-Safe Commands (always available)

| Command | Args | Response | Description |
|---|---|---|---|
| `PING` | — | `ACK,PING,pong` | Liveness check |
| `STATUS` | — | `ACK,STATUS,phase=...;bench_mode=...;debug_armed=...;telemetry_target=...;queue_depth=...;tick_hz=...;energy_wh=...;energy_budget_wh=...;budget_exhausted=...` | System status (includes live tick rate and cumulative heater energy budget) |
| `FORCE_START` | — | `ACK,FORCE_START,override accepted` | Force transition to ACTIVATION_RAMP |
| `FORCE_STOP` | — | `ACK,FORCE_STOP,override accepted` | Force transition to STOPPED |
| `HEATERS_OFF` | — | `ACK,HEATERS_OFF,all heaters disabled` | Emergency: set all heater duties to 0 |
| `RESET_CTRL` | — | `ACK,RESET_CTRL,control loop reset queued` | Reset PID integrators |
| `SHUTDOWN_SAFE` | — | `ACK,SHUTDOWN_SAFE,safe shutdown queued` | Graceful process shutdown |
| `SET_TICK_HZ` | `<hz>` | `ACK,SET_TICK_HZ,tick_hz=<hz>` | Live downlink/main-loop rate change. Range `[0.1, 5.0]` Hz. Required by BEXUS User Manual §5.4 (operator-tunable downlink). Flight-safe; no debug arm needed. |
| `RADIO_SILENCE` | — | `ACK,RADIO_SILENCE,radio silent` | Stop pushing frames out the TX socket and close the connection (latency < 1 s). Frames continue to fill the on-disk queue and will replay after `RADIO_RESUME`. |
| `RADIO_RESUME` | — | `ACK,RADIO_RESUME,radio resumed` | Re-enable transmission. Queued frames are drained in order on the next tick. |
| `ARM_DEBUG` | `<token>` | `ACK,ARM_DEBUG,debug armed` | Arm extended debug commands (bench mode only) |

Aliases: `ON` = `FORCE_START`, `OFF` = `FORCE_STOP`, `RESET` = `RESET_CTRL`.

#### Extended Commands (bench mode + debug armed)

| Command | Args | Description |
|---|---|---|
| `DISARM_DEBUG` | — | Disarm debug mode |
| `SET_HEATER_DUTY` | `<index> <duty>` | Override single heater (0–9), duty 0.0–1.0 |
| `SET_ALL_DUTY` | `<duty>` | Override all heaters, duty 0.0–1.0 |
| `SET_PID` | `<kp> <ki> <kd>` | Override sample PID gains |
| `CLEAR_OVERRIDES` | — | Clear all heater and PID overrides |
| `SET_BENCH_MODE` | `<1\|0>` | Toggle bench mode |

#### Error Responses

| Reason | Description |
|---|---|
| `bench mode required` | Extended command sent outside bench mode |
| `debug arm required` | Extended command sent without ARM_DEBUG |
| `invalid arm token` | ARM_DEBUG token does not match `debug_arm_code` in config |
| `heater index out of range` | SET_HEATER_DUTY index ≥ heater_count |
| `invalid argument count for <CMD>` | Wrong number of arguments |
| `unknown command: <CMD>` | Unrecognized command name |
| `hz out of range [0.1,5.0]` | `SET_TICK_HZ` argument outside the safe band |
| `invalid hz value` | `SET_TICK_HZ` argument not parseable as a number |

### Example Session

```
→ PING\n
← ACK,PING,pong\n

→ STATUS\n
← ACK,STATUS,phase=ASCENT_HOLD_-30C;bench_mode=1;debug_armed=0;telemetry_target=169.254.251.200;queue_depth=0\n

→ ARM_DEBUG COATHEAL_DEBUG\n
← ACK,ARM_DEBUG,debug armed\n

→ SET_HEATER_DUTY 0 0.5\n
← ACK,SET_HEATER_DUTY,override applied\n
```

## Stepper motor (sample-bending actuator)

The on-board computer drives a stepper motor to bend coating specimens during
flight. The driver IC is not yet finalised; the software uses a driver-agnostic
STEP/DIR/EN interface that maps onto any A4988/DRV8825/TMC2209-class driver.

### Commands

| Command | Args | Description |
|---------|------|-------------|
| `STEPPER_MOVE <steps>` | signed int | Move relative by N microsteps (negative = reverse) |
| `STEPPER_MOVETO <steps> [hold_s]` | signed int, optional float | Move to absolute microstep position, optionally hold for `hold_s` |
| `STEPPER_ROTATE <revs>` | signed float | Rotate relative by N full revolutions (uses `steps_per_rev × microstep`) |
| `STEPPER_BEND <steps> [hold_s]` | signed int, optional float | Alias for MOVETO, semantically tagged as a bend |
| `STEPPER_HOME` | — | Return to position 0 |
| `STEPPER_STOP` | — | Abort current motion |
| `STEPPER_SET_SPEED <hz>` | positive float | Step rate in Hz (clamped to `stepper.max_step_hz`) |
| `STEPPER_SET_MICROSTEP <n>` | int ∈ [1,32] | Set microstep divisor |
| `STEPPER_ENABLE` / `STEPPER_DISABLE` | — | Toggle driver enable pin |

### Telemetry field

Every `DATA` frame appends one segment per motor so ground software can
plot bend state alongside temperature. Rev-B emits two segments
(`STEPPER0=…`, `STEPPER1=…`); the per-segment schema is unchanged from
Rev-A's single `STEPPER=` form.

```
STEPPER<n>=pos:<position_steps>|tgt:<target_steps>|hz:<step_hz>|us:<microstep>
          |en:<0|1>|mv:<0|1>|hold:<0|1>|hold_s:<remaining>|pulses:<total>|src:<tag>
```

Where `<n>` is the motor index (`0`, `1`, …). The Rev-A un-indexed
`STEPPER=` form remains parseable on the ground station for old SD-card
logs; it is mapped to `motor_id=0` in `TelemetryPacket.steppers`.

`src` records what last changed the setpoint (`phase:FLOAT_HOLD`, `cmd:MOVE`,
`cmd:BEND`, `cmd:HOME`, `cmd:STOP`, `init`).

### Configuration (`onboard.ini`)

Key takeaways — see `config/onboard.example.ini` for the full list:

- `stepper.steps_per_rev`, `stepper.microstep`, `stepper.default_step_hz`,
  `stepper.max_step_hz`, `stepper.max_position_steps`
- `stepper.step_line`, `stepper.dir_line`, `stepper.enable_line` (BCM GPIO on
  the Pi-EzConnect HAT terminal block), `stepper.invert_direction`,
  `stepper.enable_active_low`, `stepper.enable_on_boot`
- Per-phase bend schedule: `bend.{ascent,activation,float,descent}_steps` and
  matching `_hold_s`. Applied on phase entry.
