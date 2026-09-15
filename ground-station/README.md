# Ground Station

The ground station provides telemetry reception, real-time visualization, and command uplink for the COATHEAL onboard flight software.

Two interfaces are available:

- **GUI** (`gui_app.py`) — the PyQt6 mission console: fixed layout, gated controls, mission-time plots, alarms, command console. Recommended for operations.
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
python gui_app.py [--host <onboard-ip>] [--tel-port 4000] [--cmd-port 5000] [--log logs]
```

**Default:** `python gui_app.py` starts the telemetry receiver, broadcasts
discovery, probes `169.254.10.10:5000` and takes the first onboard it hears
as the command target — plug in Ethernet, launch, wait for `LINK OK`. The
console is a fixed layout that scales to any screen from 1366×768 up
(`Ctrl+=` / `Ctrl+-`). Full reference: [docs/ground-station.md](../docs/ground-station.md).

| Region | Content |
|---|---|
| **Top strip** | MODE, PHASE, HEALTH, LINK age, RX rate, command target, session/seq, T+ mission time, UTC, RADIO state; panic group `HEATERS OFF` / `STOP MOTORS` (no confirmation) and `ENTER SAFE` (confirmed) |
| **Alarm strip** | Active alarms as chips (over-temperature, invalid heated channel, link-loss fallback, sequence paused, energy budget, motor failed, heaters inhibited, sensor faults, onboard backlog, stale link) with `ACK` |
| **System tab** | Link status, `ARM` / `DISARM` / `ENTER SAFE` / `EXIT SAFE`, `SET PHASE`, `RADIO SILENCE` / `RADIO RESUME`, downlink rate, diagnostics (`PING` `STATUS` `COMPONENTS` `GET_THERMAL` `CHECK <component>` `RESET_CTRL`), `SHUTDOWN SAFE` |
| **Thermal tab** | Energy budget, active heaters, the heaters and specimens of each motor group as the onboard reports them (`GET_LAYOUT`: heater rows with measured sample, target, duty, state; unheated specimens), all-channel targets, presets (`profiles/thermal_presets.json`), PID autotune. Targets 0–75 °C; above 40 °C asks first |
| **Motion tab** | M0 / M1 cards (enabled, zeroed, moving, holding, healthy, position, resistance before/after the bend), `ENABLE` `DISABLE` `SET ZERO` `HOME` `STOP`, jog, speed 0.01–0.5 mm/s, current, acceleration in mm/s², **BEND** (`STEPPER_MOVETO_MM` with hold), **STANDARD PULL** (`PULL_EXECUTE`), recent pulls |
| **Advanced tab** | Bend sequences, PID tuning, open-loop duty, microstep, preset management, fallback plan, network |
| **Plots** | Temperatures and Heaters (one plot per motor group), Ambient (T + pressure + UV), Resistance, Motors — mission-time axis, 5 m / 30 m / 2 h / all window, follow/pause, PNG/CSV export, full-session retention |
| **Right column** | Health (every wire flag as a dot), Checkout (live go/no-go + `RUN CHECK ALL`), Values (grouped by motor) |
| **Bottom** | Console (every command and reply, entry with completion and history), Events, Pulls |

Controls the onboard would refuse are disabled with the reason in their
tooltip (mode, enable, zero, fallback, temperature validity, radio
silence). Every reply is shown in a persistent line under its control group
and in the console; there are no toasts. Number boxes change on the mouse
wheel only while `Ctrl` or `Shift` is held, take `.` as the decimal point
whatever the system locale, and show their unit beside the box.

**Confirmation dialogs:** `ARM`, `SET PHASE`, `ENTER SAFE`, `SHUTDOWN SAFE`,
`RADIO SILENCE`, `RESET_CTRL`, `BENDSEQ RUN`, `FALLBACK ARM`. Everything else
sends immediately, including the panic pair.

**Shortcuts:** `Esc` stops both motors, `Ctrl+Shift+H` heaters off,
`Ctrl+L` console, `Ctrl+1…4` left tabs, `Alt+1…5` plot pages, `P` pause
plots, `F5` STATUS, `F1` the list. While a confirmation dialog is open every
shortcut is blocked — `Esc` closes the dialog first.

**Radio silence:** after `RADIO SILENCE` is acknowledged the ground station
stops beaconing and probing and sends nothing but `RADIO RESUME` (plus
`STATUS` / `PING` from the console); the onboard stops telemetry, beacons and
hello replies and refuses every other command. The onboard queue keeps every
frame and replays it after `RADIO RESUME`.

**Link budget (24 kbps):** all E-Link traffic stays under 24 kbit/s in every
second ([docs/link-budget.md](../docs/link-budget.md)); the ground station
keeps to its 1 150 B share. A command exchange uses about a second of it, so
commands go out one after another — safety commands (`HEATERS_OFF`,
`STEPPER_STOP`, `DISARM`, `SHUTDOWN_SAFE`, `RADIO_SILENCE`, `RADIO_RESUME`)
first, background polls and discovery last — and the latency shown includes
the wait. A request line longer than 230 B is refused locally; a command
that finds no room within 10 s (or its own longer timeout) fails with
`waited … s for link budget`. While telemetry arrives the beacon slows to
every 15 s and the command probe stops. Telemetry arrives compressed (`Z1,`
lines) when the onboard offers the dictionary in
`protocol/telemetry-dictionary-z1.txt`; without that file the ground station
answers `HELLO,plain` and the onboard sends plain lines.

### Logs

Every onboard session gets its own directory under `logs/sessions/`
(`telemetry.csv` schema v6, `pulls.csv`, `commands.csv`, `events.log`,
`session.json`); `logs/latest_session.txt` points at the one in use. The
headless `telemetry-server` writes exactly the same files.

### Reconnect behaviour

The receiver closes a connection after 8 s without data and waits for the
onboard to reconnect (it retries every ~2 s). Replayed frames are ACKed and
deduplicated by `(session_id, seq)` against `logs/ground_ack_cursor.json`.

---

## CLI — Telemetry Server

```bash
python main.py telemetry-server [OPTIONS]
```

| Option | Default | Description |
|---|---|---|
| `--bind` | `0.0.0.0` | Interface to listen on |
| `--port` | `4000` | TCP telemetry port |
| `--log` | `logs` | Log root; each onboard session gets its own directory under `<root>/sessions/` |
| `--plot` | off | Enable live matplotlib plot (basic; use GUI for full visualization) |
| `--alert-temp-c` | `75.0` | Hottest-sample temperature alert threshold (°C) |
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

The command is paced by the same link budget as the GUI; a request line over
230 B is not sent (exit code 1).

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
python main.py command --cmd "BENDSEQ_LOAD 1 flex 800:2:50 1600:3:40 0:1:50"
python main.py command --cmd "BENDSEQ_RUN 1 flex"
```

---

## Reliability

- Every received telemetry packet is acknowledged with `ACK,<session_id>,<seq>`.
- Packets are deduplicated by `(session_id, seq)`; the cursor persists in `logs/ground_ack_cursor.json` (written at most once per second) so a restarted ground station never double-logs a replayed frame.
- Commands and events that happen before the first frame are buffered and written into the session directory when it opens; a run that never hears the onboard still leaves a `_no-session` directory with the operator's record.
- The last discovered onboard IP/session is cached in `logs/discovered_onboard.json` (CLI).

## Module Reference

See [docs/ground-station.md](../docs/ground-station.md) for a full description of every module and class.
