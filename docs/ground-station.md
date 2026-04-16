# Ground Station Module Reference

The ground station is a Python package (`ground-station/`) providing two user interfaces and a shared protocol library.

```
ground-station/
├── gui_app.py          PyQt6 desktop GUI (recommended for operations)
├── main.py             CLI entry point (telemetry-server + command subcommands)
└── app/
    ├── protocol.py     Wire protocol parser and builders
    ├── telemetry_server.py   CLI telemetry receiver + live matplotlib plot
    └── command_client.py     CLI one-shot command uplink
```

---

## gui_app.py

**`ground-station/gui_app.py`**

Full-featured PyQt6 desktop application for flight operations. All network I/O runs in a background `QThread`; all Qt widget operations run on the main thread. Data crosses the boundary via queued `pyqtSignal` connections.

### Launch

```bash
python gui_app.py [--host <ip>] [--tel-port 4000] [--cmd-port 5000]
```

Default host is `169.254.10.10` (link-local Ethernet to Pi).

### Threading Model

| Thread | Responsibility |
|---|---|
| **Main thread** | Qt event loop — all widget creation, updates, plot redraws |
| **`TelemetryReceiver` (QThread)** | TCP server on `tel_port` — `accept()`, `recv()`, parse, ACK, CSV write |

The `TelemetryReceiver` emits three signals:

| Signal | Type | Delivered to |
|---|---|---|
| `packet_received` | `object` (`TelemetryPacket`) | `MainWindow._on_packet` — updates all plots and panels |
| `connection_changed` | `(bool, str)` | `MainWindow._on_connection` — updates connection label, status bar |
| `log_message` | `str` | `MainWindow._on_log` — appends to log panel |

### TelemetryReceiver

TCP server that loops: `bind → listen → accept → recv → parse → ACK → emit`. Reconnect behaviour:

- Each accepted connection is handled in `_handle_connection`. On data timeout (`_DATA_TIMEOUT_S = 3.0`) or on `socket.timeout`, the method returns, triggering reconnect.
- Windows `ConnectionResetError` (WinError 10054) from `recv()` is caught as `OSError` and treated as a clean disconnect — the thread loops back to `accept()` rather than dying.
- `srv.listen(5)` allows the OS to queue multiple pending connections, so the Pi can reconnect before the ground station calls `accept()` again.

### CommandSender

Synchronous one-shot TCP connection to the command port. Called from the main thread directly (acceptable: commands are infrequent and the 3-second timeout is tolerable).

```python
sender = CommandSender(host, cmd_port)
response = sender.send("PING")   # → "ACK,PING,pong"
```

### LivePlotWidget

PyQtGraph-based scrolling plot. Each trace is a pre-created `PlotDataItem` (curve). Data is stored in a `collections.deque(maxlen=1200)` ring buffer. `push(seq, values_dict)` appends a point; `setData(x, y)` redraws the curve without recreating the graphics item.

### GUI Panels

#### Connection Panel (left dock)

- IP address field, telemetry port, command port
- Start / Stop telemetry button
- Connection label: grey "Waiting..." or green "● <ip>"

#### Heater Duties Panel (left dock)

**Rev-B:** 9 `HeaterCell` widgets. Labels `H0–H7` (the 8 sample heaters) are
colored from the `HEATER_COLORS` amber-orange palette; `BOX` (the electronics
box heater, index 8) is colored cyan and sits on its own row spanning both
columns. Each cell has a 0–100 % progress bar, a readout of `duty | temp`,
and a `Set` / `Off` action row.

#### Motor Dock (right dock, "Motors" tab)

**Rev-B:** dedicated read-only dashboard for the two sample-bending motors.
Each motor (M0, M1) has its own `QGroupBox` with pos/tgt/Hz/µstep/hold/pulses
readouts and three status dots (`en`, `mv`, `hold`). Populated from
`packet.steppers[0]` and `packet.steppers[1]`. When the onboard reports
only one motor (legacy single-`STEPPER=` frame), M1 is rendered as "—".

#### Pull Events Panel (bottom dock, "Pull events" tab)

**Rev-B:** scrolling `QTableWidget` of `EVT,PULL,...` frames. Columns:
`time`, `motor`, `pull_id`, `steps`, `hold s`, `samples`, `session`.
Motor column is colored per motor (green for M0, orange for M1). Fed by
the `TelemetryReceiver.pull_event` signal; the dispatcher also appends to
`<log>_pulls.csv` alongside the main DATA log.

#### Commands Panel (left dock)

Buttons for every command. Dangerous commands (`FORCE_STOP`, `HEATERS_OFF`, `RESET_CTRL`, `SHUTDOWN_SAFE`) require a `QMessageBox` confirmation dialog before sending. Debug commands (`SET_HEATER_DUTY`, `SET_ALL_DUTY`, `SET_PID`, etc.) require `ARM_DEBUG` first.

#### Temperature Plot (center tab)

Box temperature and all sample temperatures plotted over sequence number. One trace per channel, auto-colored.

#### Pressure Plot (center tab)

Ambient pressure (mbar) over sequence number. Useful for observing ascent/float/descent transitions.

#### Heater Duties Plot (center tab)

All 10 heater duty cycles (0.0–1.0) as line traces over sequence number.

#### Environment Plot (center tab)

Ambient humidity (%) and UV × 100 over sequence number.

#### Values Panel (right, resizable)

Latest value for every telemetry field, updated on each packet. **Rev-B**
adds rows for 8 samples (S0–S7), a `MOTORS` section with per-motor state
(M0 and M1 each surface pos/tgt, Hz · µstep, mode, src), and broken-out
indicator rows for the new STATUS bits `RS485_OK/FAIL` and
`HEATER_INHIBITED/HEATER_ACTIVE`. Status flags are color-coded green / red
/ amber.

#### Log Panel (bottom dock)

Timestamped scrolling event log from both the telemetry receiver and any command responses. Controls: auto-scroll toggle, Clear, Save to file.

#### Status Bar

Five status widgets at the bottom edge:

| Widget | Content |
|---|---|
| Phase | Current mission phase string, color-coded per phase |
| SEQ | Latest sequence number |
| Pressure | Ambient pressure in mbar |
| Box Temp | Electronics box temperature in °C |
| LINK | `LINK: OK` (green) / `LINK: FAIL` (red) / `LINK: —` (disconnected) |
| Rate | Packet rate and age ("X.X s ago") |

All widgets are cleared / greyed out when the onboard disconnects.

### CSV Log

The GUI writes received telemetry to `logs/ground_telemetry.csv` in the same format as the CLI server. **File → Open CSV log…** loads any existing CSV into all plot panels for post-flight analysis.

### ACK Cursor

Acknowledgement state is persisted to `logs/ground_ack_cursor.json`. The GUI deduplicates replayed frames using `{session_id → last_seq}` — duplicate packets are ACK'd (to let the Pi clear its queue) but are not written to the CSV log again.

---

## app/protocol.py

**`ground-station/app/protocol.py`**

Pure-Python wire protocol primitives. No I/O — only parsing and building.

### `TelemetryPacket` (dataclass)

Holds all fields from a decoded DATA frame:

| Field | Type |
|---|---|
| `session_id` | `str` |
| `seq` | `int` |
| `timestamp` | `str` (ISO-8601) |
| `rtc_valid` | `int` (0 or 1) |
| `ambient_temp_c` | `float` |
| `ambient_pressure_mbar` | `float` |
| `ambient_humidity_pct` | `float` |
| `uv` | `float` |
| `box_temp_c` | `float` |
| `sample_temps_c` | `List[float]` |
| `heater_duty` | `List[float]` |
| `phase` | `str` |
| `status` | `str` |
| `mode` | `str` (Rev-B: populated from `MODE=` token) |
| `steppers` | `List[Dict]` (Rev-B: 0, 1, or 2+ motor snapshots) |
| `stepper` | `StepperSnapshot` or `None` (legacy; mirrors `steppers[0]`) |

### `parse_telemetry_csv(line) → TelemetryPacket`

Parses a raw DATA frame string. Raises `TelemetryParseError` on:

- Fewer than 13 comma-separated tokens
- Missing `DATA` prefix
- Missing `HEATER_DUTY=` field
- Missing `PHASE=` or `STATUS=` field

The number of sample temperatures is inferred from the position of `HEATER_DUTY=` — it is not hardcoded to 8, 9, or 10.

**Rev-B stepper handling:** both the legacy single-segment `STEPPER=…` and
the new indexed `STEPPER0=…`, `STEPPER1=…` forms are accepted.
`packet.steppers` is a list of dicts with keys
`motor_id, position, target, hz, microstep, enabled, moving, holding,
hold_s, pulses, source`:

| Input | `packet.steppers` length | `packet.stepper` |
|---|---|---|
| No stepper segment (legacy short frame) | 0 | `None` |
| Legacy single `STEPPER=…` | 1 (motor_id=0) | mirrors `steppers[0]` |
| Rev-B `STEPPER0=…`, `STEPPER1=…` | 2+, sorted by motor_id | mirrors `steppers[0]` |

### `parse_pull_event(line) → PullEvent`

Parses an `EVT,PULL,<session>,<pull_id>,<motor_id>,<start_ts>,<steps_moved>,<hold_s>,<samples>` line. `samples` is either pipe-separated specimen indices or `-` for empty. Raises `TelemetryParseError` on malformed input.

### `build_ack(session_id, seq) → str`

Returns `"ACK,<session_id>,<seq>\n"`.

### `build_command(command) → str`

Strips and appends `\n`. Raises `ValueError` on empty input.

### `TelemetryParseError`

Subclass of `ValueError`. Raised by `parse_telemetry_csv` on malformed input.

---

## app/telemetry_server.py

**`ground-station/app/telemetry_server.py`**

CLI telemetry receiver (headless). Used via `python main.py telemetry-server`.

### TelemetryServer

Manages the TCP server, discovery beacon, optional matplotlib live plot, CSV log, ACK cursor, and discovered-host cache.

#### `run()`

1. Creates log, cursor, and discovered directories
2. Optionally starts `LivePlotter`
3. Optionally starts `_discovery_loop` daemon thread
4. Starts `_network_loop` daemon thread
5. If plotting: drives matplotlib on the main thread at ~20 fps via `plt.pause(0.05)`
6. If not plotting: joins the network thread until `KeyboardInterrupt`

#### `_network_loop()`

Binds TCP server socket, accepts connections in a loop. Each accepted connection is passed to `_handle_connection` — the loop blocks until that connection closes, then immediately waits for the next `accept()`.

#### `_handle_connection(conn)`

Per-connection handler. Opens (or appends to) the CSV log, then loops:

1. `conn.recv(4096)` with 1-second socket timeout
2. On `socket.timeout`: checks elapsed time vs `timeout_s`; returns if stale
3. On empty `recv`: clean close, returns
4. Parse line → `TelemetryPacket`
5. Check duplicate by `(session_id, seq)` against cursor
6. Send ACK regardless of duplicate status
7. If not duplicate: write CSV row, flush, push to plotter if enabled

#### `_discovery_loop()`

Runs as a daemon thread. Sends `GS_HELLO` broadcast on UDP `discovery_port` once per second and listens for `ONBOARD_HELLO` replies. On a valid reply (matching nonce), updates `_last_onboard_ip` and `_last_onboard_session`, writes `discovered_onboard.json`.

### LivePlotter

Optional matplotlib live plot (box temperature + pressure vs. sequence). Uses a `collections.deque` as a thread-safe buffer between the network thread and the main thread. The main thread calls `tick()` every ~50 ms to drain the buffer and redraw.

---

## app/command_client.py

**`ground-station/app/command_client.py`**

CLI one-shot command uplink. Used via `python main.py command --cmd <CMD>`.

### `send_command(host, port, command, timeout) → str`

Opens a TCP connection, sends `command + "\n"`, reads the response, and closes the connection. Returns the stripped response string.

### `discover_onboard_host(discovery_port, command_port, timeout) → str | None`

Sends a single `GS_HELLO` UDP broadcast and waits up to `timeout` seconds for an `ONBOARD_HELLO` reply with a matching nonce. Returns the sender's IP address or `None`.

### `load_discovered_host(path) → str | None`

Reads `discovered_onboard.json` and returns the cached `onboard_ip` string, or `None` if the file is absent or malformed.

### Host Resolution Order

1. `--host` argument (explicit)
2. `logs/discovered_onboard.json` cache
3. UDP auto-discovery (if `--discovery-enabled`)
4. `--static-host` fallback (`192.168.50.2`)

### Safety Confirmation

Commands in `DANGEROUS_COMMANDS` (`FORCE_STOP`, `HEATERS_OFF`, `RESET_CTRL`, `SHUTDOWN_SAFE`, `OFF`, `RESET`) require interactive confirmation (`Type YES to continue`) unless `--yes` is passed.

---

## main.py

**`ground-station/main.py`**

CLI entry point. Registers two subcommands via `argparse`:

| Subcommand | Module | Description |
|---|---|---|
| `telemetry-server` | `app.telemetry_server` | TCP telemetry receiver + CSV log + optional plot |
| `command` | `app.command_client` | Send one command and print the response |

Each subcommand's `add_subparser()` sets `_coatheal_handler` as the `argparse` default, which `main()` calls after parsing.
