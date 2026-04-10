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
