# Development Guide

---

## Repository Layout

```
├── onboard/            C++17 onboard application
│   ├── src/            Implementation files (.cpp)
│   ├── include/coatheal/  Public headers (.hpp)
│   └── CMakeLists.txt
├── tests/
│   ├── unit/test_suite.cpp   C++ unit tests (doctest)
│   └── CMakeLists.txt
├── ground-station/
│   ├── gui_app.py      PyQt6 GUI (recommended)
│   ├── main.py         CLI entry point
│   ├── app/            Protocol, telemetry server, command client
│   └── tests/          Python unit tests
├── config/             Example INI configurations
├── deploy/             systemd unit file
├── scripts/            Utility shell scripts
└── docs/               Documentation
```

---

## C++ Build

### Requirements

| Tool | Minimum version |
|---|---|
| CMake | 3.16 |
| C++ compiler | C++17 support (GCC 10+, Clang 12+) |
| libgpiod | 1.x or 2.x (for real GPIO output backends) |

On Raspberry Pi OS:
```bash
sudo apt install cmake g++ libgpiod-dev
```

### Configure and build

```bash
# Debug build (default)
cmake -S . -B build
cmake --build build --parallel

# Release build (flight)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The onboard binary is at `build/onboard/coatheal_onboard`.

### Strict warnings

The `COATHEAL_STRICT` CMake option (enabled by default) adds `-Wall -Wextra -Wpedantic -Werror`. To disable during development:

```bash
cmake -S . -B build -DCOATHEAL_STRICT=OFF
```

---

## C++ Tests

Unit tests use [doctest](https://github.com/doctest/doctest) (header-only, bundled or fetched by CMake).

```bash
# Run all C++ tests
ctest --test-dir build --output-on-failure

# Run with verbose output
ctest --test-dir build -V
```

Test file: `tests/unit/test_suite.cpp`

Tests cover:
- `PidController` — output clamping, anti-windup, reset
- `HeaterScheduler` — power budget enforcement, heater count limits
- `CommandParser` — valid commands, aliases, argument validation, unknown commands
- `TelemetrySerializer` — DATA frame format and field ordering
- `TelemetryQueue` — enqueue, pending, acknowledge, disk persistence

---

## Python Environment

```bash
cd ground-station
pip install -r requirements.txt
```

**Dependencies:**

| Package | Version | Purpose |
|---|---|---|
| `PyQt6` | ≥ 6.5 | GUI framework |
| `pyqtgraph` | ≥ 0.13 | Real-time scientific plots |
| `numpy` | ≥ 1.24 | Array support for pyqtgraph |
| `matplotlib` | ≥ 3.7, < 4 | CLI live plot (`--plot` flag only) |

---

## Python Tests

```bash
cd ground-station
python -m unittest discover -s tests -p "test_*.py" -v
```

Test file: `ground-station/tests/test_protocol.py`

Tests cover `parse_telemetry_csv` — valid frames, variable sample counts, malformed inputs, duplicate detection logic.

---

## Bench Mode

Bench mode allows full software testing without any hardware attached.

### Enable bench mode

Set in the config:
```ini
runtime.bench_mode=true
runtime.use_simulated_pwm=true
runtime.use_simulated_sensors=true
```

Or use `config/onboard.debug.ini`.

### What bench mode does

- `SensorManager` uses simulation only when `runtime.use_simulated_sensors=true`
- Simulated pressure decreases over time to drive automatic phase transitions
- Simulated temperatures respond to heater duty cycles
- `SimulatedPwmController` stores duty cycles in memory (no GPIO required)
- Heater duty, targets, and `SET_PID` are normal manual controls.
- `SET_BENCH_MODE` remains bench/debug gated by `ARM_DEBUG <token>`.

### End-to-end bench test

Terminal 1 — run onboard:
```bash
./build/onboard/coatheal_onboard --config config/onboard.debug.ini
```

Terminal 2 — run ground station:
```bash
cd ground-station
python gui_app.py --host 127.0.0.1
```

Or using the CLI:
```bash
python main.py telemetry-server --bind 0.0.0.0 --port 4000 --no-discovery-enabled
```

Terminal 3 — send commands:
```bash
python main.py command --host 127.0.0.1 --cmd PING
python main.py command --host 127.0.0.1 --cmd STATUS
python main.py command --host 127.0.0.1 --cmd FORCE_START
```

---

## Simulating Signal Loss

To test telemetry reconnection behavior without a physical Ethernet cable:

1. In the GUI, click **Stop telemetry** — this closes the TCP server socket
2. Wait a few seconds for the Pi to time out and close its connection
3. Click **Start telemetry** — the GUI re-opens the server socket
4. The Pi retries every `reconnect_ms` (default 2000 ms) and reconnects automatically

Alternatively, on the Pi:
```bash
sudo systemctl restart coatheal-onboard
```

The ground station will receive a new connection from a new session ID. All queued frames from the previous session are replayed and deduplicated.

---

## Adding a New Command

1. Add a new `CommandType` enum value in `onboard/include/coatheal/command.hpp`
2. Register it in `CommandParser::kCommandMap` in `onboard/src/command_parser.cpp` with its argument count
3. Handle it in `SystemController::HandleCommandLine` in `onboard/src/system_controller.cpp`
4. Add a button and handler in the GUI `CommandPanel` class in `ground-station/gui_app.py`
5. Update `docs/protocol.md` and `docs/onboard.md`

---

## Code Style

- **C++**: C++17, `snake_case` names, RAII ownership, no raw `new`/`delete`. Headers use `#pragma once`. All public API in `coatheal` namespace.
- **Python**: PEP 8, type hints via `from __future__ import annotations`. Dataclasses for protocol types.
