# Wire Protocol Specification (Rev C)

All messages are UTF-8 encoded, newline-terminated, and sent over TCP or UDP.
Rev C keeps the existing wire shape for ground-station compatibility, but the
hardware meaning is now the schematic v3 final BOM: 8-channel PT100/PT1000
sample temperatures through a Sequent Microsystems 8-channel RTD HAT over
I2C, pressure from DPS310, UV from GUVA-S12SD through ADS1115, and two
TMC5160 motor channels driven SPI-only (no STEP/DIR). The legacy MAX31865
(RTD Click) **temperature** path and DAQ-132M (RS485/Modbus) sample-
temperature path have been fully retired; the Sequent card is the only
sample-temperature source. A MAX31865 pair returns in v3 in an unrelated
role — the sample-**resistance** instrument described under `RESISTANCE`
and the `CHECK` command below — this is new hardware and a new function, not
a revival of the retired temperature path.

> **Breaking wire change.** The `COMPONENT_STATE` field and the `STATUS`
> field both changed shape in this revision: `DAQ132M`/`RTD_CLICK` collapsed
> into a single `SEQUENT_RTD` term, and `RS485_OK`/`RS485_FAIL` was removed
> outright. There is no mixed-version compatibility window — onboard and
> ground station must be deployed together.

## Telemetry DATA Frame

Sent by the onboard to the ground station over TCP port `4000`.

```text
DATA,<session_id>,<seq>,<timestamp>,<rtc_valid>,<ambient_temp_c>,<ambient_pressure_mbar>,<uv>,<sample_0>,...,<sample_7>,HEATER_DUTY=<d0>|...|<d5>,RESISTANCE=<r0>|...|<r7>,PHASE=<phase>,MODE=<mode>,STATUS=<flags>,SENSOR_VALID=<kv>,SENSOR_AGE_MS=<kv>,COMPONENT_STATE=<kv>,CTRL=<kv>,STEPPER0=<kv>,STEPPER1=<kv>
```

| Field | Meaning |
|---|---|
| `ambient_temp_c` | DPS310 ambient temperature value |
| `ambient_pressure_mbar` | DPS310 pressure value |
| `uv` | GUVA-S12SD analog output through ADS1115 |
| `sample_0..sample_7` | PT100/PT1000 sample values, one per Sequent RTD HAT channel; disabled or missing channels serialize as `nan` |
| `HEATER_DUTY` | Six polyimide heater duty values, H0..H5 |
| `RESISTANCE` | Sample-resistance value in Ω; which physical quantity and slots it carries depends on `sensor.resistance_source` — see [configuration.md#sensors](configuration.md#sensors). A `-` also appears for any individual slot with no positive value yet |
| `SENSOR_VALID` | Current validity for ambient temperature (`AT`), pressure (`AP`), UV, and `S0..S7` |
| `SENSOR_AGE_MS` | Monotonic age of each last successful reading; `-1` means never valid |
| `COMPONENT_STATE` | Independent state for DPS310, ADS1115, SEQUENT_RTD, both motors, and PWM |
| `CTRL` | Controller state the ground station cannot derive from the other fields (added 2026-08-28, see below) |
| `STEPPER0`, `STEPPER1` | TMC5160-driven NEMA 17 ball-screw motor snapshots (SPI-only position dribble; no STEP/DIR); keys below |

The parser locates `HEATER_DUTY=` by token name, so sample count is inferred
from the position of that token. Frames with any number of sample columns parse
as long as every column before `HEATER_DUTY=` is numeric. Every `<kv>` field
is pipe-separated `key:value` pairs; parsers must ignore keys they do not
know, which is how additions stay backward compatible.

### `STEPPERn` keys

| Key | Meaning |
|---|---|
| `pos`, `tgt` | Current and target position, absolute microsteps |
| `hz` | Configured step rate, full-step Hz (2 decimals) |
| `us` | Microstep divisor |
| `ok`, `en`, `mv`, `hold` | Driver healthy / power stage enabled / pulses being issued / at target with a hold countdown running (`0`/`1`) |
| `hold_s` | Remaining hold time, seconds |
| `pulses`, `missed` | Pulses issued since boot, missed pulse deadlines |
| `src` | Origin of the last motion: `init`, `cmd:MOVE`, `cmd:BEND`, `cmd:HOME`, `cmd:ZERO`, `cmd:STOP`, `cmd:PULL`; `-` when empty |
| `zeroed` | `1` once `SET_POSITION_ZERO <id>` has run since the onboard started (absolute moves, homing, pulls and sequences need it) — added 2026-08-28 |
| `seq` | Name of the bend sequence active on this motor, `-` when none — added 2026-08-28 |
| `seqst` | `idle`, `run`, or `pause` — added 2026-08-28 |

### `CTRL` keys (added 2026-08-28)

Emitted after `COMPONENT_STATE` and before `STEPPER0`, every frame.

| Key | Meaning |
|---|---|
| `fallback` | `1` while link-loss fallback is active (`manual.link_loss_fallback_*`) |
| `link_loss_s` | Seconds since an established link was last seen (1 decimal; `0.0` while healthy) |
| `energy_wh` | Cumulative heater energy this session (2 decimals) |
| `budget_wh` | `power.energy_budget_wh` (1 decimal; `0.0` means unlimited) |
| `budget_exhausted` | `1` once the energy latch has tripped (heaters stay off until `RESET_CTRL`) |
| `heaters_active` | Number of heaters with a non-zero scheduled duty this tick (owner cap: 3) |
| `queue` | Frames waiting in the durable telemetry queue before this one was enqueued (a backlog draining after a link outage) |
| `plan` | Fallback bend-plan state: `none` until the plan feature ships, then `armed`, `running`, `done`, `failed`, or `disarmed` |

Example:

```text
CTRL=fallback:0|link_loss_s:0.0|energy_wh:12.40|budget_wh:130.0|budget_exhausted:0|heaters_active:2|queue:0|plan:none
```

Never-valid values serialize as `nan`. After a failure, the last good value is
retained, its validity becomes `0`, and its age increases. Component states are
`DISABLED`, `DISCOVERING`, `OK`, `DEGRADED`, `STALE`, or `FAILED`.

### Example

```text
DATA,coatheal-1718000000-123456,42,2026-04-16T12:00:00Z,1,-10.23,140.12,0.00012,5.1,5.2,5.0,5.3,5.1,5.2,5.0,5.3,HEATER_DUTY=0.250|0.000|0.250|0.000|0.000|0.050,RESISTANCE=-|-|-|-|-|-|-|-,PHASE=FLOAT,MODE=RUN,STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK|T_AMBIENT_OK|P_AMBIENT_OK|UNIFORMITY_OK|OVERTEMP_OK|ENERGY_OK|PWM_OK|STEPPER_OK|SAMPLE_TEMP_OK|REAL_SENSORS|SEQ_READY|HEATER_ACTIVE|RESISTANCE_OK,SENSOR_VALID=AT:1|AP:1|UV:1|S0:1|S1:1|S2:1|S3:1|S4:1|S5:1|S6:1|S7:1,SENSOR_AGE_MS=AT:120|AP:120|UV:250|S0:900|S1:900|S2:900|S3:900|S4:900|S5:900|S6:900|S7:900,COMPONENT_STATE=DPS310:OK|ADS1115:OK|SEQUENT_RTD:OK|MOTOR0:OK|MOTOR1:OK|PWM:OK,CTRL=fallback:0|link_loss_s:0.0|energy_wh:12.40|budget_wh:130.0|budget_exhausted:0|heaters_active:3|queue:0|plan:none,STEPPER0=pos:100|tgt:200|hz:100.00|us:4|ok:1|en:1|mv:1|hold:0|hold_s:0.00|pulses:100|missed:0|src:cmd:MOVE|zeroed:1|seq:-|seqst:idle,STEPPER1=pos:0|tgt:0|hz:0.00|us:4|ok:1|en:1|mv:0|hold:0|hold_s:0.00|pulses:0|missed:0|src:init|zeroed:0|seq:-|seqst:idle
```

## Status Flags

```text
STATUS=SD_OK|USB_OK|I2C_OK|SPI_OK|LINK_OK|T_AMBIENT_OK|P_AMBIENT_OK|UNIFORMITY_OK|OVERTEMP_OK|ENERGY_OK|PWM_OK|STEPPER_OK|SAMPLE_TEMP_OK|REAL_SENSORS|SEQ_READY|HEATER_ACTIVE|RESISTANCE_OK
```

| Flag | Meaning |
|---|---|
| `SD_OK` / `SD_FAIL` | Primary SD-card CSV log health |
| `USB_OK` / `USB_FAIL` | Secondary USB mirror log health |
| `I2C_OK` / `I2C_FAIL` | Latest DPS310 and ADS1115 read health, ANDed with Sequent RTD HAT bus health (did the last `Probe`/`ReadAll` conversation succeed). Bus-level only — a card that answers but has one open/short channel still reports `I2C_OK`; per-channel detail is in `sample_temp_valid` and `SEQUENT_RTD` |
| `SPI_OK` / `SPI_FAIL` | TMC5160 SPI setup/check health |
| `LINK_OK` / `LINK_FAIL` | Last telemetry drain/ACK status |
| `T_AMBIENT_OK` / `T_AMBIENT_FAIL` | Ambient temperature in configured range |
| `P_AMBIENT_OK` / `P_AMBIENT_FAIL` | Ambient pressure in configured range |
| `UNIFORMITY_OK` / `UNIFORMITY_FAIL` | Heated sample spread within tolerance |
| `OVERTEMP_OK` / `OVERTEMP_FAIL` | No sample over-temperature latch |
| `ENERGY_OK` / `ENERGY_FAIL` | Heater energy budget not exhausted |
| `PWM_OK` / `PWM_FAIL` | Heater GPIO/PWM backend health |
| `STEPPER_OK` / `STEPPER_FAIL` | Both motor backends healthy |
| `SAMPLE_TEMP_OK` / `SAMPLE_TEMP_FAIL` | Every sample channel a heater controls (`heater.temperature_channels`) is valid and fresh; unheated channels are ignored, and an empty or out-of-range mapping fails closed |
| `SIMULATED` / `REAL_SENSORS` | Explicit sensor mode |
| `SEQ_PAUSED` / `SEQ_READY` | At least one bend sequence is paused/faulted, or no sequence fault is active |
| `HEATER_ACTIVE` / `HEATER_INHIBITED` | Heaters are inhibited while a motor holds `MotionLock` |
| `RESISTANCE_OK` / `RESISTANCE_FAIL` | Under the default `sensor.resistance_source=max31865_click`, tracks both clicks' bus health — OK while both clicks' last conversation succeeded, FAIL when either did not. Under `sequent_rtd`, tracks RTD-card bus health instead. Always OK under `disabled` and `simulated`. Bus-level, not per-channel: a saturated (out-of-range) specimen reading, or a card answering with one open probe, still reports OK here — that is a valid measurement of an out-of-range channel, not a bus failure, and shows up on `SENSOR_VALID`/`COMPONENT_STATE` instead |

## Pull-Cycle Event Frame

Emitted once per completed pull cycle.

```text
EVT,PULL,<session_id>,<pull_id>,<motor_id>,<start_ts>,<steps_moved>,<hold_s>,<samples>
```

| Field | Meaning |
|---|---|
| `motor_id` | `0` = samples 0..3, `1` = samples 4..7 |
| `steps_moved` | Signed final position minus start position |
| `hold_s` | Time held at the target |
| `samples` | Pipe-separated sample indices, for example `0|1|2|3` |

Example:

```text
EVT,PULL,coatheal-1718000000-123456,3,1,2026-04-16T10:21:00Z,200,5.00,4|5|6|7
```

## ACK Frame

The ground station ACKs each accepted telemetry or event frame.

```text
ACK,<session_id>,<seq>
```

DATA ACKs are cumulative per session. The onboard durable queue deletes all DATA
frames up to the ACKed sequence number for the matching session. Queued
`EVT,*` frames also accept the ground station's `ACK,<session>,0`; that removes
the exact queued event frame and does not fail later DATA telemetry draining.

## UDP Discovery

Ground station broadcast:

```text
GS_HELLO,<nonce>,<telemetry_port>,<command_port>
```

Onboard reply:

```text
ONBOARD_HELLO,<nonce>,<session_id>,<hostname>,<command_port>,<telemetry_port>
```

If UDP discovery fails, the GUI and CLI also probe `169.254.10.10:5000` with
`PING`. Any successful command connection lets the onboard retarget telemetry
to that command peer IP.

## Command Protocol

Commands are sent from the ground station to the onboard over TCP port `5000`.
Each command is one newline-terminated line. The onboard replies once and closes
the connection.

Success:

```text
ACK,<COMMAND>,<message>
```

Failure:

```text
NACK,<COMMAND>,<reason>
```

### Flight-Safe Commands

| Command | Args | Description |
|---|---|---|
| `PING` | none | Liveness check |
| `STATUS` | none | Lightweight live state: phase/mode, fallback, queue, current hardware flags, and sequence state |
| `COMPONENTS` | none | Non-invasive cached component state, error, and channel summary |
| `CHECK` | `[ALL\|DPS310\|ADS1115\|SEQUENT_RTD\|DAQ132M\|RTD_CLICK\|MAX31865\|PWM\|MOTOR0\|MOTOR1\|STORAGE\|COMMS]` | Active probe of all or one selected component. `DAQ132M`/`RTD_CLICK` are accepted as legacy aliases for `SEQUENT_RTD` (the retired temperature path). `MAX31865` selects the two v3 sample-resistance clicks — a command-argument addition only, no `COMPONENT_STATE`/frame-format change |
| `ARM` | none | Enable manual flight outputs |
| `DISARM` | none | Disable outputs, clear heater overrides, stop steppers |
| `SET_PHASE` | `<phase>` | Set `BOOT`, `ASCENT`, `PRE_FLOAT`, `FLOAT`, `DESCENT`, `LANDED`, or `STOPPED` |
| `FORCE_START` | none | Manual-first alias for `SET_PHASE ASCENT` |
| `FORCE_STOP` | none | Manual-first alias for `SET_PHASE DESCENT` and stepper stop |
| `HEATERS_OFF` | none | Emergency heater shutoff |
| `RESET_CTRL` | none | Reset PID integrators |
| `SHUTDOWN_SAFE` | none | Flush logs and stop process |
| `SET_TICK_HZ` | `<hz>` | Runtime tick/downlink rate, `0.1..5.0` Hz |
| `RADIO_SILENCE` | none | Stop telemetry transmission while keeping the queue |
| `RADIO_RESUME` | none | Resume telemetry transmission |
| `SET_HEATER_DUTY` | `<index> <duty>` | Set one heater duty, index `0..5`. Normal mode requires valid mapped temperature feedback; bench/debug arm allows open-loop duty on channels without feedback or scheduler clamping. |
| `SET_ALL_DUTY` | `<duty>` | Set all heater duties. Normal mode requires valid temperature feedback for every heater; bench/debug arm allows open-loop duty on all channels without feedback or scheduler clamping. |
| `SET_TEMP_TARGET` | `<index> <temp_c>` | Set one closed-loop target within configured limits |
| `SET_ALL_TEMP_TARGETS` | `<temp_c>` | Set all six closed-loop targets |
| `CLEAR_TEMP_TARGET` | `<index>` | Clear one target |
| `CLEAR_TEMP_TARGETS` | none | Clear every target |
| `SET_PID` | `<index\|ALL> <kp> <ki> <kd>` | Set non-negative PID gains |
| `GET_THERMAL` | none | Return target, measured temperature, and duty for every heater |
| `CLEAR_OVERRIDES` | none | Clear duty, target, and PID overrides |
| `SET_POSITION_ZERO` | `<id>` | Set current physical position as software zero without motion |
| `STEPPER_MOVE` | `<id> <steps>` | Relative motor move |
| `STEPPER_MOVETO` | `<id> <abs_usteps> [hold_s]` | Absolute move; motor must be zeroed |
| `STEPPER_ROTATE` | `<id> <revs>` | Rotate by full revolutions |
| `STEPPER_BEND` | `<id> <abs_usteps> [hold_s]` | Compatibility alias for absolute move; motor must be zeroed |
| `STEPPER_HOME` | `<id>` | Return to software zero; motor must be zeroed |
| `STEPPER_STOP` | `<id>` | Stop motion and release `MotionLock` |
| `STEPPER_SET_SPEED` | `<id> <hz>` | Set motor speed |
| `STEPPER_SET_MICROSTEP` | `<id> <n>` | Set microstep divisor |
| `STEPPER_ENABLE` / `STEPPER_DISABLE` | `<id>` | Enable or disable driver output |
| `PULL_ARM` | `<id>` | Queue one pull cycle |
| `PULL_EXECUTE` | `<id>` | Queue one pull cycle and report as executed |
| `BENDSEQ_LOAD` | `<id> <name> <target>:<hold>[:<hz>] ...` | Load a runtime absolute-microstep sequence |
| `BENDSEQ_RUN` | `<id> <name>` | Run a loaded sequence |
| `BENDSEQ_PAUSE` / `BENDSEQ_RESUME` | `<id>` | Pause or resume the active sequence |
| `BENDSEQ_STOP` / `BENDSEQ_STATUS` | `<id>` | Stop or inspect sequence state |
| `BENDSEQ_CLEAR` | `<id> [name]` | Clear one or all stored definitions for a motor |

`ON`, `OFF`, and `RESET` remain aliases for `FORCE_START`, `FORCE_STOP`, and
`RESET_CTRL`.

Setting a duty clears that channel's temperature target. Setting a temperature
target clears that channel's duty override. `HEATERS_OFF` clears all duties and
targets. Any invalid PT100 channel forces its matching heater off, including in
open-loop duty mode.

Absolute motion, homing, pull cycles, and bend sequences require
`SET_POSITION_ZERO <id>` after each onboard restart. Relative
`STEPPER_MOVE`/`STEPPER_ROTATE` commands are allowed before zeroing.

Example:

```text
ARM
STEPPER_ENABLE 0
SET_POSITION_ZERO 0
BENDSEQ_LOAD 0 flex 800:2:50 1600:3:75 0:1:50
BENDSEQ_RUN 0 flex
BENDSEQ_STATUS 0
```

### Bench-Only Commands

These require `runtime.bench_mode=true` and `ARM_DEBUG <token>`.

| Command | Args | Description |
|---|---|---|
| `DISARM_DEBUG` | none | Disable debug mode |
| `SET_BENCH_MODE` | `<1|0>` | Toggle bench mode |
| `HEATER_TEST` | `<index> <duty> <seconds>` | Bounded commissioning pulse. Requires bench mode, debug arm, RUN mode, no active motor motion lock, and configured duty/time limits. |
