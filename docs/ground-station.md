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
cd D:\COATHEAL-flight-software\COATHEAL-flight-software\ground-station
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
| Temperature plot | PT100 sample temperatures S0..S7. Current bench uses RTD Click on S0; invalid channels show as unavailable |
| Pressure plot | DPS310 pressure |
| Environment / UV plot | GUVA-S12SD value through ADS1115 |
| Resistance plot | Compatibility field; normally empty/`-` for final BOM |
| Values panel | Latest parsed telemetry fields and status tokens |
| Status bar | Phase, sequence, pressure, sample mean, link state, packet rate |

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
Bench-only commands require `runtime.bench_mode=true` and `ARM_DEBUG`.

## Protocol Parser

`app/protocol.py` accepts:

| Input | Parser |
|---|---|
| DATA frame | `parse_telemetry_csv` |
| Pull event | `parse_pull_event` |
| ACK | `build_ack` |
| Command line | `build_command` |

The parser keeps compatibility with the `RESISTANCE=` field. In final-BOM Rev C
operation, no resistance instrument is carried, so that field normally contains
`-` placeholders.

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
