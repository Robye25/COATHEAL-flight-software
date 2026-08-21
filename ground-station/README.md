# Ground Station

The ground station provides telemetry reception, real-time visualization, and command uplink for the COATHEAL onboard flight software.

Two interfaces are available:

- **GUI** (`gui_app.py`) — PyQt6 desktop application with live plots, heater bars, command buttons, and a log viewer. Recommended for operations.
- **CLI** (`main.py`) — Headless telemetry server and command uplink for scripting and testing.

## Windows quick start (no shell commands)

Double-click **`COATHEAL-GroundStation.bat`**. The first run creates a local
Python environment, installs the dependencies, and offers a one-click
Windows-firewall setup so onboard auto-discovery works (one administrator
prompt). Every later run launches instantly. Run the same file with
`--check` to verify the environment without opening the GUI.

## Linux quick start

Same flow via **`./COATHEAL-GroundStation.sh`** — bootstraps `.venv`,
installs dependencies (offering `python3-venv`/`libxcb-cursor0` via apt if
missing), opens `ufw` ports on request, then launches. `--check` supported.

## Requirements

```bash
pip install -r requirements.txt
# PyQt6, pyqtgraph, numpy, matplotlib
```

---

## GUI (Recommended)

```bash
python gui_app.py [--host <onboard-ip>] [--tel-port 4000] [--cmd-port 5000]
```

**Default:** `python gui_app.py` starts telemetry immediately, broadcasts discovery, and probes `169.254.10.10:5000`. A successful probe also teaches the Pi where to send telemetry, so the normal flow is plug in Ethernet, launch the GUI, wait for `connected`.

### GUI Panels

| Panel | Location | Description |
|---|---|---|
| **Connection** | Left dock | Onboard IP, telemetry/command ports, Start Telemetry button, discovery and receiver status |
| **Mode** | Left dock | `ARM`/`DISARM`, `FORCE START`/`FORCE STOP`, phase selection, SAFE mode, radio silence/resume |
| **Heater Control** | Left dock | 6 duties, manual targets, PID tuning, and local JSON profiles |
| **Motor Control** | Left dock | M0/M1 selection, jog and absolute moves, software zero, collapsible bend-sequence editor |
| **Commands** | Left dock | Diagnostics (`PING`, `STATUS`, `CHECK`, `COMPONENTS`, `RESET_CTRL`), tick rate, arbitrary command entry |
| **Temperature / Pressure / Heaters / Resistance / Stepper** | Center tabs | Live PyQtGraph traces |
| **Health** | Right dock, first tab | Green/red dots for all 17 wire status flags plus the six `COMPONENT_STATE` entries; opens by default |
| **Values** | Right dock | Latest value for every telemetry field (flag state lives on Health, not here) |
| **Motors** | Right dock | Per-motor position, target, speed, and microstep detail |
| **Preflight** | Right dock | Go/no-go checklist dots |
| **Cmd History** | Right dock | Every command sent with its response and latency; double-click a row to re-issue it |
| **Emergency bar** | Bottom dock, always visible | `HEATERS OFF` and `STOP MOTORS` fire immediately; `ENTER SAFE`, `SHUTDOWN SAFE`, `RADIO SILENCE` confirm first |
| **Event log / Pull events** | Bottom dock tabs | Timestamped scrolling log with auto-scroll and save; pull-event table |
| **Status strip** | Above the plots | MODE, PHASE, aggregate HEALTH dot, LINK staleness, session, sequence, discovery |

The **Resistance** traces are the coating-specimen measurement from the two
MAX31865 clicks (4-wire Kelvin per specimen). Blank `-` values are NOT normal:
they mean the instrument is not reporting, the onboard raises `RESISTANCE_FAIL`,
and the Health tab's RESISTANCE dot turns red. An absent or unpowered click
reports `CLICK_NOT_DETECTED` in `CHECK`. See
[docs/sequent-rtd-bring-up.md](../docs/sequent-rtd-bring-up.md) for the
reference-resistor and range bring-up gates.

### Command Buttons

**Normal controls**:
- `PING`, `STATUS`, `CHECK`, `ARM`, `DISARM`
- Per-heater duty and temperature target controls
- Per-channel/all-channel PID tuning
- Explicit motor selection, software zero, and runtime bend sequences

**Panic actions** (fire immediately, no confirmation dialog -- by design):
- `HEATERS OFF` and `STOP MOTORS` in the emergency bar
- `Esc` stops BOTH motors (`STEPPER_STOP 0` and `STEPPER_STOP 1`)

**Confirm-gated** (a dialog appears first):
- `ARM`, `SET PHASE`, `FORCE STOP`, `ENTER SAFE`, `RADIO SILENCE`, `RESET_CTRL`,
  `SHUTDOWN SAFE`

While a confirmation dialog is open, Qt blocks every keyboard shortcut,
including `Esc` -- `Esc` dismisses the dialog first, so press it again to stop
the motors. An open bend-sequence table-cell editor swallows the first `Esc`
the same way. `F1` lists all shortcuts.

Thermal profiles are saved to `profiles/thermal_profiles.json` and are applied
by re-sending PID gains and targets to the Pi.

### Reconnect Behaviour

The GUI automatically reconnects when the onboard restarts or the link drops. The telemetry receiver detects a stale connection after 8.0 seconds of no data, closes it, and immediately waits for a new connection. The onboard retries every ~2 seconds.

---

## CLI — Telemetry Server

```bash
python main.py telemetry-server [OPTIONS]
```

| Option | Default | Description |
|---|---|---|
| `--bind` | `0.0.0.0` | Interface to listen on |
| `--port` | `4000` | TCP telemetry port |
| `--log` | `logs/ground_telemetry.csv` | CSV output path |
| `--plot` | off | Enable live matplotlib plot (basic; use GUI for full visualization) |
| `--alert-temp-c` | `80.0` | Hottest-sample temperature alert threshold (°C) |
| `--timeout-s` | `10.0` | Seconds before stale connection is closed |
| `--no-discovery-enabled` | — | Disable UDP discovery beacon |
| `--discovery-port` | `4100` | UDP discovery port |
| `--command-port` | `5000` | Command port (reported in discovery) |
| `--cursor` | `logs/ground_ack_cursor.json` | ACK cursor persistence file |
| `--discovered` | `logs/discovered_onboard.json` | Discovered onboard IP cache |

---

## CLI — Command Uplink

```bash
python main.py command --cmd "<COMMAND>" [OPTIONS]
```

| Option | Default | Description |
|---|---|---|
| `--host` | auto | Onboard IP. Omit for auto-discovery |
| `--port` | `5000` | Command port |
| `--cmd` | required | Command string, e.g. `"STATUS"` or `"SET_HEATER_DUTY 0 0.5"` |
| `--timeout` | per command | Socket timeout (seconds). Omit it and the value comes from `protocol.COMMAND_TIMEOUTS`: `CHECK` gets 15.0 (it runs a full hardware conversation), everything else 3.0. Passing `--timeout` always wins. |
| `--yes` | off | Skip safety confirmation for dangerous commands |
| `--no-discovery-enabled` | — | Disable UDP discovery |
| `--static-host` | `169.254.10.10` | Static fallback IP |

**Examples:**

```bash
# Liveness check
python main.py command --cmd PING

# Arm manual outputs and set phase
python main.py command --cmd ARM
python main.py command --cmd "SET_PHASE ASCENT"

# Emergency heater off (requires confirmation unless --yes)
python main.py command --cmd HEATERS_OFF --yes

# Set a single heater duty
python main.py command --cmd "SET_HEATER_DUTY 3 0.75"

# Closed-loop temperature control
python main.py command --cmd "SET_PID ALL 0.20 0.02 0.03"
python main.py command --cmd "SET_TEMP_TARGET 3 25.0"
python main.py command --cmd GET_THERMAL

# Define and run a bend sequence on motor 1
python main.py command --cmd "STEPPER_ENABLE 1"
python main.py command --cmd "SET_POSITION_ZERO 1"
python main.py command --cmd "BENDSEQ_LOAD 1 flex 800:2:50 1600:3:75 0:1:50"
python main.py command --cmd "BENDSEQ_RUN 1 flex"
```

---

## Reliability

- Every received telemetry packet is acknowledged with `ACK,<session_id>,<seq>`.
- The ground station deduplicates packets by `(session_id, seq)` — replayed frames from the onboard queue are not double-logged.
- The ACK cursor persists in `logs/ground_ack_cursor.json` so the ground station can resume a session correctly after restart.
- The last discovered onboard IP/session is cached in `logs/discovered_onboard.json`.

## Module Reference

See [docs/ground-station.md](../docs/ground-station.md) for a full description of every module and class.
