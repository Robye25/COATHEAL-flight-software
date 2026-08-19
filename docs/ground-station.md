# Ground Station Module Reference

The ground station is the operator interface for Rev C manual-first flight
operations. It provides a PyQt6 GUI, a CLI telemetry receiver, and a CLI command
client.

```text
ground-station/
  gui_app.py                 PyQt6 desktop GUI
  main.py                    CLI entry point
  app/protocol.py            DATA, EVT,PULL, ACK, and command parsing
  app/telemetry_server.py    CLI telemetry receiver
  app/command_client.py      CLI command uplink
```

## Launch

Plug the laptop into the Pi Ethernet link, then start:

```powershell
cd ground-station
.\.venv\Scripts\Activate.ps1
python gui_app.py
```

With no `--host`, the GUI runs plug-and-play:

1. Listen for telemetry on TCP `4000`.
2. Send UDP discovery beacons on `4100`.
3. Probe the Pi at `169.254.10.10:5000`.
4. Use a successful probe/discovery result as the command target.
5. Let the Pi retarget telemetry to this laptop's link-local IP.

Explicit host mode is still available:

```powershell
python gui_app.py --host 169.254.10.10 --tel-port 4000 --cmd-port 5000
```

## GUI Responsibilities

| Component | Responsibility |
|---|---|
| `MainWindow` | Main Qt thread, widgets, plots, command buttons |
| `TelemetryReceiver` | Background TCP server, frame parsing, ACKs, CSV writes |
| `CommandSender` | One-shot TCP command client |
| Discovery worker | UDP beacons and onboard-host cache |

The GUI updates all plots and panels from parsed `TelemetryPacket` objects. It
ACKs duplicate replayed frames so the Pi can clear its durable queue, but it
does not duplicate CSV rows.

## Rev C Display Model

| Panel / plot | Rev C data |
|---|---|
| Connection | Pi command target, telemetry bind state, discovery result |
| Heater control | Six duties, six PID targets, global/per-channel target controls, PID tuning, and local JSON profiles |
| Motor dock | Explicit M0/M1 selector, jog/absolute controls, software zero, and runtime sequence editor |
| Pull events | `EVT,PULL` rows with motor id, steps, hold time, sample group |
| Temperature plot | PT100 sample temperatures S0..S7 from the Sequent RTD HAT; invalid channels show as unavailable |
| Pressure plot | DPS310 pressure |
| Environment / UV plot | GUVA-S12SD value through ADS1115 |
| Resistance plot | MAX31865 click coating-specimen resistance by default (two monitored slots only); Sequent RTD HAT per-channel PT100 element resistance under `sequent_rtd`; `-`/simulated per `sensor.resistance_source` |
| Health tab | Dot for every OK/FAIL flag, tri-state mode flag, and COMPONENT_STATE entry; feeds the aggregate health dot on the top status strip |
| Motors tab | Read-only M0/M1 dashboard (position, target, Hz, microstep, enable/moving/holding) -- independent of the Motor dock's controls |
| Values panel | Latest parsed telemetry fields (status/component flags moved to the Health tab) |
| Preflight tab | Checklist dots: RTC valid, ambient sensors in-range, heater duties reporting, motors enabled, telemetry link healthy, specimen uniformity, over-temperature latch clear |
| Status bar | Phase, sequence, pressure, sample mean, link state, packet rate |

## Safety controls

`Esc` stops BOTH motors (`STEPPER_STOP 0` and `STEPPER_STOP 1`) -- panic-class,
no confirmation dialog. The EmergencyBar's `STOP MOTORS` button (beside
`HEATERS OFF`) does the same thing from the mouse; both are unconfirmed panic
buttons by design, same policy. Every other EmergencyBar action
(`ENTER SAFE`, `SHUTDOWN SAFE`, `RADIO SILENCE`) requires a confirmation
dialog first.

While a confirm dialog is open, keyboard shortcuts (including `Esc`) are
blocked by Qt's modal dialog until it's dismissed -- `Esc` closes the dialog
itself before it can reach anything else. The View menu (checkable actions
toggling the left/right/bottom docks, wired to each dock's own
`toggleViewAction()`) lets an operator hide any dock that isn't needed for the
current phase of operations.

## Command Workflow

Typical connected operation:

```powershell
python main.py command --cmd PING
python main.py command --cmd STATUS
python main.py command --cmd CHECK
python main.py command --cmd ARM
python main.py command --cmd "SET_PHASE ASCENT"
python main.py command --cmd "SET_PID ALL 0.20 0.02 0.03"
python main.py command --cmd "SET_TEMP_TARGET 0 25.0"
python main.py command --cmd GET_THERMAL
python main.py command --cmd "STEPPER_ENABLE 0"
python main.py command --cmd "SET_POSITION_ZERO 0"
python main.py command --cmd "BENDSEQ_LOAD 0 flex 800:2:50 1600:3:75 0:1:50"
python main.py command --cmd "BENDSEQ_RUN 0 flex"
python main.py command --cmd "BENDSEQ_STATUS 0"
python main.py command --cmd HEATERS_OFF --yes
python main.py command --cmd DISARM
```

Dangerous commands require GUI confirmation or the CLI `--yes` flag:

```text
FORCE_STOP
HEATERS_OFF
RESET_CTRL
SHUTDOWN_SAFE
OFF
RESET
```

Thermal profiles are stored locally in `profiles/thermal_profiles.json`. The Pi
does not persist targets or tuned gains; applying a profile re-sends them.
Bench-only commands require `runtime.bench_mode=true` and `ARM_DEBUG`. After debug arming, manual heater duty commands can drive open-loop channels without mapped temperature feedback or scheduler clamping.

## Protocol Parser

`app/protocol.py` accepts:

| Input | Parser |
|---|---|
| DATA frame | `parse_telemetry_csv` |
| Pull event | `parse_pull_event` |
| ACK | `build_ack` |
| Command line | `build_command` |

The parser keeps compatibility with the `RESISTANCE=` field; which physical
quantity and slots it carries depends on the onboard `sensor.resistance_source`
setting — see [configuration.md#sensors](configuration.md#sensors).

Invalid samples are not added to plots. Before a sensor has ever succeeded the
readout is `N/A`; after a failure it shows the last good value and stale age.
The component panel reports each sensor, motor, and PWM subsystem separately.

## CSV Logs

The GUI writes received DATA frames to `logs/ground_telemetry.csv`. Pull events
are written to a sibling pull-event CSV. The ACK cursor is stored in
`logs/ground_ack_cursor.json` so restarted ground-station sessions can dedupe
replayed onboard queue frames.

## CLI Telemetry Server

Headless telemetry receiver:

```powershell
python main.py telemetry-server --host 0.0.0.0 --port 4000
```

It binds the telemetry socket, parses incoming DATA and event frames, writes CSV
logs, and sends ACKs.

## CLI Command Client

One-shot command uplink:

```powershell
python main.py command --cmd STATUS
python main.py command --host 169.254.10.10 --cmd PING
```

Host resolution order:

1. Explicit `--host`.
2. `logs/discovered_onboard.json`.
3. UDP discovery if enabled.
4. Static fallback `169.254.10.10`.
