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

**Default:** `python gui_app.py` connects to `169.254.10.10` on the standard ports.

### GUI Panels

| Panel | Location | Description |
|---|---|---|
| **Connection** | Left dock | Onboard IP, telemetry port, command port; Start/Stop button; connection status |
| **Heater Duties** | Left dock | 6 animated progress bars (H0–H5 = sample-bank heaters) |
| **Commands** | Left dock | Buttons for every command; dangerous commands require confirmation dialog |
| **Temperature** | Center tab | Live PyQtGraph plot — up to 8 sample temps |
| **Pressure** | Center tab | Live pressure trace |
| **Heater Duties** | Center tab | All 6 heater duty traces over time |
| **Resistance** | Center tab | 8 sample-resistance traces from the INA3221 (channels with no coverage stay empty) |
| **Values** | Right panel | Latest value for every telemetry field; status flags color-coded |
| **Log** | Bottom dock | Timestamped scrolling log; auto-scroll toggle; save to file |
| **Status bar** | Bottom edge | Phase (color-coded), SEQ, pressure, hottest sample, LINK status, staleness |

### Command Buttons

**Safe commands** (no confirmation required):
- `PING`, `STATUS`, `FORCE START`

**Safety-critical** (confirmation dialog):
- `FORCE STOP`, `HEATERS OFF`, `RESET CTRL`, `SHUTDOWN SAFE`

**Debug** (ARM DEBUG token required first):
- `ARM DEBUG <token>`, `DISARM DEBUG`
- `BENCH ON` / `BENCH OFF`
- `SET HEATER DUTY <index> <duty>` — index 0–5, duty 0.0–1.0
- `SET ALL DUTY <duty>`
- `CLEAR OVERRIDES`

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
| `--static-host` | `192.168.50.2` | Static fallback IP |

**Examples:**

```bash
# Liveness check
python main.py command --cmd PING

# Force activation ramp
python main.py command --cmd FORCE_START

# Emergency heater off (requires confirmation unless --yes)
python main.py command --cmd HEATERS_OFF --yes

# Override a single heater (requires ARM_DEBUG first)
python main.py command --cmd "ARM_DEBUG mytoken"
python main.py command --cmd "SET_HEATER_DUTY 3 0.75"
```

---

## Reliability

- Every received telemetry packet is acknowledged with `ACK,<session_id>,<seq>`.
- The ground station deduplicates packets by `(session_id, seq)` — replayed frames from the onboard queue are not double-logged.
- The ACK cursor persists in `logs/ground_ack_cursor.json` so the ground station can resume a session correctly after restart.
- The last discovered onboard IP/session is cached in `logs/discovered_onboard.json`.

## Module Reference

See [docs/ground-station.md](../docs/ground-station.md) for a full description of every module and class.
