# Wire Protocol Specification

All messages are UTF-8 encoded, newline (`\n`) terminated, and transmitted over TCP or UDP.

---

## Telemetry Data Frame

Sent by the onboard over TCP (port 4000) once per tick.

### Format

```
DATA,<session_id>,<seq>,<timestamp>,<rtc_valid>,<ambient_temp_c>,<ambient_pressure_mbar>,<ambient_humidity_pct>,<uv>,<box_temp_c>,<sample_0>,...,<sample_N>,HEATER_DUTY=<d0>|<d1>|...|<d9>,PHASE=<phase>,STATUS=<flags>\n
```

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
| 10…N | `sample_i` | float | PT100 sample temperatures (°C), one per heater channel |
| N+1 | `HEATER_DUTY=…` | key=value | Pipe-separated heater duty cycles (0.0–1.0), 10 values |
| N+2 | `PHASE=…` | key=value | Current mission phase string |
| N+3 | `STATUS=…` | key=value | Pipe-separated status flags (see below) |

### HEATER_DUTY Format

```
HEATER_DUTY=0.000|0.250|0.000|0.500|0.000|0.000|0.000|0.000|0.000|0.000
```

10 values, indices 0–8 = sample heaters, index 9 = electronics box heater.

### PHASE Values

| String | Description |
|---|---|
| `ASCENT_HOLD_-30C` | Holding −30 °C during ascent |
| `ACTIVATION_RAMP_+70C` | Ramping to +70 °C |
| `FLOAT_HOLD_+70C` | Holding +70 °C at float altitude |
| `DESCENT_FLOOR_-20C` | Descent with −20 °C floor |
| `STOPPED` | Mission complete or safe shutdown |

### STATUS Flags

```
STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK
```

Each flag is either `<NAME>_OK` or `<NAME>_FAIL`.

| Flag | Description |
|---|---|
| `SD_OK` / `SD_FAIL` | Primary SD card CSV log healthy |
| `USB_OK` / `USB_FAIL` | Secondary USB mirror log healthy |
| `I2C_OK` / `I2C_FAIL` | I2C bus (BME280, ADS1115, RTC) healthy |
| `SPI_OK` / `SPI_FAIL` | SPI bus (MIKROE-2815 PT100 adapters) healthy |
| `LINK_OK` / `LINK_FAIL` | Telemetry link (last ACK received successfully) |

### Example

```
DATA,coatheal-1718000000-123456,42,2024-06-10T12:00:00Z,1,-55.23,140.12,12.4,0.00012,-5.10,-30.1,-30.2,-30.0,-30.3,-30.1,-30.2,-30.0,-30.3,-30.1,HEATER_DUTY=0.250|0.000|0.250|0.000|0.000|0.000|0.000|0.000|0.000|0.000,PHASE=ASCENT_HOLD_-30C,STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK
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

## Stepper motors (sample-pulling actuators)

REV-B: the on-board computer drives **two** stepper motors that pull samples
downward to induce microcracks. Motor 0 (TMC5160 on SPI1, Pololu 2851
NEMA-17) owns samples 0–3; motor 1 (A4988/DRV8825 on STEP/DIR/EN, Adafruit
1918 NEMA-17) owns samples 4–7. Only one motor may pull at a time — the
`MotionLock` interlock enforces this, and the heater scheduler also gates
duty while the lock is held.

### Motion envelope

- Full-step motors: **200 full-steps / revolution**.
- Max pull rate: **100 full-steps / second** (≈30 rpm). Commands that ask for
  more are clamped.
- Default acceleration: **200 full-steps / s²** — ramps 0 → 100 Hz in 0.5 s.
- Microstep: **4×** default (= 800 µstep / rev), configurable to **5×** (=
  1000 µstep / rev). All linear distances in the command surface below are
  in microsteps unless noted.
- 1 revolution ≈ 1–2 mm of downward pull (mechanical calibration, see
  `docs/hardware.md`).

### Commands

All commands accept an **optional leading motor id** (integer 0 or 1). If
omitted, id defaults to 0 — legacy REV-A operator scripts keep working.

| Command | Args | Description |
|---------|------|-------------|
| `STEPPER_MOVE <id> <steps>` | int id, signed int steps | Relative move, id-addressed |
| `STEPPER_MOVETO <id> <abs_steps> [hold_s]` | int, signed int, optional float | Absolute move with optional hold |
| `STEPPER_ROTATE <id> <revs>` | int, signed float | Rotate relative by N full revolutions |
| `STEPPER_BEND <id> <steps> [hold_s]` | int, signed int, optional float | Alias for MOVETO, tagged as a bend |
| `STEPPER_HOME <id>` | int id | Return to position 0 |
| `STEPPER_STOP <id>` | int id | Abort motion, release MotionLock |
| `STEPPER_SET_SPEED <id> <hz>` | int, positive float | Full-step Hz, clamped to `stepper.max_step_hz` |
| `STEPPER_SET_MICROSTEP <id> <n>` | int, int | 4 or 5 by default (extensible per-motor via config) |
| `STEPPER_ENABLE <id>` / `STEPPER_DISABLE <id>` | int id | Gate the driver /EN line |
| `PULL_ARM <id>` | int id | Acquire MotionLock and queue a pull cycle (non-blocking) |
| `PULL_EXECUTE <id>` | int id | Acquire lock and run one full pull+hold+retract synchronously |

Legacy forms (no id) route to motor 0: `STEPPER_MOVE 400` is equivalent to
`STEPPER_MOVE 0 400`.

A pull cycle is: move to `pull.travel_full_steps × microstep` (default 200
full-steps = 1 rev), hold for `pull.hold_s` (default 5 s), retract to
position 0, release the MotionLock. Completion emits the log line
`[pull] cycle complete id=<id> samples=<list>`.

### Telemetry field

Every `DATA` frame appends a `STEPPER=` segment of pipe-separated key:value
pairs so ground software can plot motion state alongside temperature. The
frame currently reflects motor 0 (channel 0) — motor 1's status is exposed
via per-channel snapshot (`StepperController::Snapshot(int)`). A follow-up
telemetry rev will add a parallel `STEPPER1=` segment:

```
STEPPER=pos:<position_steps>|tgt:<target_steps>|hz:<step_hz>|us:<microstep>
       |en:<0|1>|mv:<0|1>|hold:<0|1>|hold_s:<remaining>|pulses:<total>|src:<tag>
```

`src` records what last changed the setpoint (`phase:FLOAT_HOLD`, `cmd:MOVE`,
`cmd:BEND`, `cmd:HOME`, `cmd:STOP`, `cmd:PULL`, `init`).

### Configuration (`onboard.ini`)

Legacy keys (REV-A, still honoured for the channel-0 fallback path):

- `stepper.steps_per_rev`, `stepper.microstep`, `stepper.default_step_hz`,
  `stepper.max_step_hz`, `stepper.max_position_steps`
- `stepper.step_line`, `stepper.dir_line`, `stepper.enable_line`,
  `stepper.invert_direction`, `stepper.enable_active_low`,
  `stepper.enable_on_boot`
- Per-phase bend schedule: `bend.{ascent,activation,float,descent}_steps` and
  matching `_hold_s`. Applied to motor 0 on phase entry.

REV-B keys expected by the multi-channel controller (Agent D / orchestrator
must plumb these through `config.cpp`):

- `motor0.driver=tmc5160`, `motor0.spi_device=/dev/spidev1.0`,
  `motor0.cs_line=<gpio>`, `motor0.step_line`, `motor0.dir_line`,
  `motor0.enable_line`, `motor0.run_current_a_rms=1.5`,
  `motor0.hold_current_frac=0.30`, `motor0.stealth_chop=1`,
  `motor0.samples=0,1,2,3`
- `motor1.driver=a4988`, `motor1.step_line`, `motor1.dir_line`,
  `motor1.enable_line`, `motor1.ms0_line`, `motor1.ms1_line`,
  `motor1.ms2_line`, `motor1.samples=4,5,6,7`
- Shared motion envelope: `pull.max_step_hz=100`,
  `pull.accel_steps_per_s2=200`, `pull.microstep=4`,
  `pull.travel_full_steps=200`, `pull.hold_s=5`
