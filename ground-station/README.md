# Ground Station

The ground station provides telemetry reception, real-time visualization, and command uplink for the COATHEAL onboard flight software.

Two interfaces are available:

- **GUI** (`gui_app.py`) — PyQt6 desktop application with live plots, heater bars, command buttons, and a log viewer. Recommended for operations.
- **CLI** (`main.py`) — Headless telemetry server and command uplink for scripting and testing.

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
| **Connection** | Left dock | Onboard IP, telemetry port, command port; Start/Stop button; connection status |
| **Heater Control** | Left dock | 6 duties, manual targets, PID tuning, and local JSON profiles |
| **Motor Control** | Left dock | M0/M1 selection, jog/absolute commands, software zero, and bend sequence editor |
| **Commands** | Left dock | Diagnostics including `CHECK`, plus arbitrary command entry |
| **Temperature** | Center tab | Live PyQtGraph plot — up to 8 sample temps |
| **Pressure** | Center tab | Live pressure trace |
| **Heater Duties** | Center tab | All 6 heater duty traces over time |
| **Resistance** | Center tab | Compatibility traces; final BOM normally emits `-` because no resistance instrument is carried |
| **Values** | Right panel | Latest value for every telemetry field; status flags color-coded |
| **Log** | Bottom dock | Timestamped scrolling log; auto-scroll toggle; save to file |
| **Status bar** | Bottom edge | Phase (color-coded), SEQ, pressure, hottest sample, LINK status, staleness |

### Command Buttons

**Normal controls**:
- `PING`, `STATUS`, `CHECK`, `ARM`, `DISARM`
- Per-heater duty and temperature target controls
- Per-channel/all-channel PID tuning
- Explicit motor selection, software zero, and runtime bend sequences

**Safety-critical** (confirmation dialog):
- `FORCE STOP`, `HEATERS OFF`, `RESET CTRL`, `SHUTDOWN SAFE`

**Debug** (`ARM_DEBUG` token required first):
- `ARM DEBUG <token>`, `DISARM DEBUG`
- `BENCH ON` / `BENCH OFF`

Thermal profiles are saved to `profiles/thermal_profiles.json` and are applied
by re-sending PID gains and targets to the Pi.

### Reconnect Behaviour

The GUI automatically reconnects when the onboard restarts or the link drops. The telemetry receiver detects a stale connection after 3 seconds of no data, closes it, and immediately waits for a new connection. The onboard retries every ~2 seconds.

### Replaying a CSV Log

**File → Open CSV log…** loads any previously recorded `ground_telemetry.csv` into all plot panels for post-flight analysis.

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
| `--timeout` | `3.0` | Socket timeout (seconds) |
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
