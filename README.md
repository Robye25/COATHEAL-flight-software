# COATHEAL Flight Software

COATHEAL is a BEXUS high-altitude balloon experiment investigating thermal self-healing materials. This repository contains the complete flight software stack: a C++17 onboard application running on a Raspberry Pi 4, and a Python ground station for telemetry reception, real-time visualization, and command uplink.

```
┌──────────────────────────────────┐         ┌──────────────────────────────────┐
│        Raspberry Pi 4            │         │        Ground Station (PC)        │
│  ┌────────────────────────────┐  │  E-Link  │  ┌────────────────────────────┐  │
│  │   coatheal_onboard (C++)   │  │  TCP/IP  │  │   gui_app.py  (PyQt6)      │  │
│  │  - Mission phase control   │◄─┼──────────┼─►│  - Live plots (PyQtGraph)  │  │
│  │  - 10-channel PID thermal  │  │  :4000   │  │  - Command buttons         │  │
│  │  - Durable telemetry queue │  │  :5000   │  │  - Heater duty bars        │  │
│  │  - Dual CSV logging        │  │          │  │  - Status & log panels     │  │
│  └────────────────────────────┘  │          │  └────────────────────────────┘  │
└──────────────────────────────────┘          └──────────────────────────────────┘
```

## Repository Layout

```
├── onboard/            C++17 flight software (sources, headers, HAL)
├── tests/              C++ unit tests (PID, scheduler, parser, telemetry, queue)
├── ground-station/     Python ground station (GUI, CLI, protocol library)
├── config/             Runtime configuration examples
├── deploy/             systemd unit file
├── scripts/            Setup, preflight, and security scripts
└── docs/               Detailed documentation
```

## Documentation

| Document | Description |
|---|---|
| [docs/architecture.md](docs/architecture.md) | System architecture, data-flow, thread model |
| [docs/onboard.md](docs/onboard.md) | Onboard C++ module reference |
| [docs/ground-station.md](docs/ground-station.md) | Ground station module reference (GUI + CLI) |
| [docs/protocol.md](docs/protocol.md) | Wire protocol specification |
| [docs/configuration.md](docs/configuration.md) | Full INI configuration reference |
| [docs/deployment.md](docs/deployment.md) | Pi setup and service installation guide |
| [docs/development.md](docs/development.md) | Build, test, and bench-mode workflow |
| [docs/hardware.md](docs/hardware.md) | Hardware reference and HAL status |

## Quick Start — Ground Station GUI

```bash
cd ground-station
pip install -r requirements.txt
python gui_app.py --host 169.254.10.10
```

The GUI connects to the onboard over Ethernet, displays live telemetry plots (temperature, pressure, heater duties, environment), and provides one-click command buttons. See [docs/ground-station.md](docs/ground-station.md) for full details.

## Quick Start — Onboard (Raspberry Pi)

```bash
# Build on the Pi
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# Run in bench/simulation mode (no real hardware needed)
./build/onboard/coatheal_onboard --config /tmp/flight.ini
```

See [docs/deployment.md](docs/deployment.md) for first-time Pi setup and [docs/development.md](docs/development.md) for bench-mode configuration.

## Build and Test

```bash
# Configure
cmake -S . -B build

# Build
cmake --build build --config Release --parallel

# Test
ctest --test-dir build --output-on-failure

# Python tests
python -m unittest discover -s ground-station/tests -p "test_*.py"
```

## Mission Phases

| Phase | Trigger | Target |
|---|---|---|
| `ASCENT_HOLD` | Power-on | −30 °C |
| `ACTIVATION_RAMP` | Pressure < 140 mbar | Ramp to +70 °C at 0.85 °C/s |
| `FLOAT_HOLD` | Ramp complete | +70 °C for 90 min |
| `DESCENT_FLOOR` | Pressure > 300 mbar | −20 °C floor |
| `STOPPED` | Float duration elapsed | Off |

Phase transitions are pressure-based and can be overridden by ground command (`FORCE_START`, `FORCE_STOP`, `RESET_CTRL`).

## Command Summary

| Command | Safe? | Description |
|---|---|---|
| `PING` | ✓ | Liveness check |
| `STATUS` | ✓ | Phase, bench mode, queue depth |
| `FORCE_START` | ✓ | Force activation ramp |
| `FORCE_STOP` | ⚠ | Force stop phase |
| `HEATERS_OFF` | ⚠ | Emergency heater shutoff |
| `RESET_CTRL` | ⚠ | Reset thermal control loop |
| `SHUTDOWN_SAFE` | ⚠ | Graceful process shutdown |
| `ARM_DEBUG <token>` | — | Arm extended debug commands |
| `SET_HEATER_DUTY <i> <0–1>` | debug | Override single heater |
| `SET_ALL_DUTY <0–1>` | debug | Override all heaters |
| `SET_PID <kp> <ki> <kd>` | debug | Override PID gains |
| `CLEAR_OVERRIDES` | debug | Clear all overrides |
| `SET_BENCH_MODE <1\|0>` | debug | Toggle bench mode |

⚠ = requires confirmation in GUI / `--yes` flag in CLI. `debug` = requires `ARM_DEBUG` first.

## Pi Boot Autostart

```bash
./scripts/install_onboard_service.sh /bexus/code/coatheal /bexus/code/coatheal/config/onboard.example.ini
sudo systemctl enable coatheal-onboard
sudo systemctl start coatheal-onboard
```

## Security

Historical credential exposure was remediated. See [docs/security-remediation.md](docs/security-remediation.md), [`scripts/rotate_ssh_key.sh`](scripts/rotate_ssh_key.sh), and [`scripts/purge_sensitive_history.sh`](scripts/purge_sensitive_history.sh).
