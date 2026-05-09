# COATHEAL Flight Software

COATHEAL is a BEXUS high-altitude balloon experiment investigating thermal self-healing materials under stratospheric cold-soak combined with mechanical pull-induced microcracking. This repository contains the complete flight software stack: a C++17 onboard application running on a Raspberry Pi 4 and a Python ground station for telemetry reception, real-time visualisation, and command uplink.

```
┌──────────────────────────────────┐         ┌──────────────────────────────────┐
│        Raspberry Pi 4            │         │        Ground Station (PC)       │
│  ┌────────────────────────────┐  │  E-Link │  ┌────────────────────────────┐  │
│  │   coatheal_onboard (C++)   │  │  TCP/IP │  │   gui_app.py  (PyQt6)      │  │
│  │  - Mission phase FSM       │◄─┼─────────┼─►│  - Live plots (PyQtGraph)  │  │
│  │  - 6-channel floor PID     │  │  :4000  │  │  - Command buttons         │  │
│  │  - Dual stepper (TMC2240)  │  │  :5000  │  │  - Heater duty bars (H0..5)│  │
│  │  - Durable telemetry queue │  │         │  │  - Resistance / pull pane  │  │
│  │  - Dual CSV logging        │  │         │  │  - Status & log panels     │  │
│  └────────────────────────────┘  │         │  └────────────────────────────┘  │
└──────────────────────────────────┘         └──────────────────────────────────┘
```

## Repository Layout

```
├── onboard/            C++17 flight software (sources, headers, HAL)
├── tests/              C++ unit tests (PID, scheduler, parser, telemetry, queue, MotionLock)
├── ground-station/     Python ground station (PyQt6 GUI, CLI, protocol library)
├── config/             Runtime configuration examples (onboard.example.ini, onboard.debug.ini)
├── deploy/             systemd unit file
├── scripts/            Setup, preflight, and security scripts
└── docs/               Detailed documentation
```

## Documentation

| Document | Description |
|---|---|
| [docs/architecture.md](docs/architecture.md) | System architecture, data-flow, thread model (Rev B.1) |
| [docs/onboard.md](docs/onboard.md) | Onboard C++ module reference (Rev B.1) |
| [docs/ground-station.md](docs/ground-station.md) | Ground station module reference (GUI + CLI) |
| [docs/protocol.md](docs/protocol.md) | Wire protocol specification (DATA, `EVT,PULL`, commands) |
| [docs/configuration.md](docs/configuration.md) | Full INI configuration reference |
| [docs/deployment.md](docs/deployment.md) | Pi setup and service installation guide |
| [docs/development.md](docs/development.md) | Build, test, and bench-mode workflow |
| [docs/hardware.md](docs/hardware.md) | Hardware reference and HAL status (Rev B.1 BOM) |
| [docs/CHANGELOG-RevB.md](docs/CHANGELOG-RevB.md) | Rev B + Rev B.1 change log |
| [docs/SED-Compliance-Report.md](docs/SED-Compliance-Report.md) | SED v2.0 compliance matrix |

## Quick Start — Ground Station GUI

```bash
cd ground-station
pip install -r requirements.txt
python gui_app.py --host 169.254.10.10
```

The GUI connects to the onboard over Ethernet, displays live telemetry plots (sample temperatures, ambient pressure, heater duties, sample resistance, stepper state), and provides one-click command buttons. See [docs/ground-station.md](docs/ground-station.md) for full details.

For a presentation stand with no Raspberry Pi connected, run:

```bash
cd ground-station
python demo_app.py
```

This launches the same GUI with synthetic telemetry and local command ACKs.

## Quick Start — Onboard (Raspberry Pi)

```bash
# Build on the Pi
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# Run in bench/simulation mode (no real hardware needed)
./build/onboard/coatheal_onboard --config config/onboard.debug.ini
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

## Mission Phases (Rev B)

| Phase | Trigger | Thermal policy |
|---|---|---|
| `BOOT` | Power-on | Heaters off |
| `ASCENT` | `SystemMode = RUN` | Floor ≥ +5 °C (per-sample PID, 0.5 °C hysteresis) |
| `FLOAT` | Pressure ≤ 100 mbar | Floor ≥ +5 °C; pulls happen here |
| `DESCENT` | Pressure ≥ 300 mbar | Floor ≥ +5 °C |
| `LANDED` | Pressure ≥ 800 mbar | Heaters off |
| `STOPPED` | `FORCE_STOP` / `SHUTDOWN_SAFE` | Heaters off |

Transitions are pressure-based and can be overridden by ground command (`FORCE_START`, `FORCE_STOP`, `RESET_CTRL`, `SHUTDOWN_SAFE`). Microcrack formation at Rev B.1 is driven by **mechanical pulls** from two ball-screw stepper motors, not by a thermal activation ramp.

## Heaters, Sensors, and Motors (Rev B.1)

| Subsystem | Rev B.1 | Count | Interface |
|---|---|---|---|
| Sample heaters | 5 W @ 24 V DC polyimide film | **6** (samples 0–5 only) | GPIO PWM via 6-ch MOSFET module |
| Unheated pulled samples | PT100 read only | 2 (samples 6 and 7) | — |
| Sample PT100s | Labfacility XF-931 | **8** | 2 × 4-ch Modbus RTD collector on USB-RS485 |
| Ambient P + T | GY-MS5803-01BA | 1 | I2C (no humidity) |
| UV | GUVA-S12SD + ADS1015 (12-bit) | 1 | Analog → I2C |
| Sample resistance | INA3221 (addr 0x40 / 0x41) | **2 chips × 3 ch = 6 samples** | I2C — science instrument |
| Steppers | OMC 17E19S2504BSM5-150RS ball-screw NEMA-17 (1 mm lead) | **2** | 2 × TMC2240 SPI + STEP/DIR/EN |
| Power | Pololu D24V50F5 (5 V / 5 A) from 28.8 V rail | — | XT60 connectors |

## Command Summary

| Command | Safe? | Description |
|---|---|---|
| `PING` | OK | Liveness check |
| `STATUS` | OK | Phase, bench mode, queue depth, tick rate, energy-budget state |
| `FORCE_START` | OK | Force transition into `ASCENT` (Rev B) |
| `FORCE_STOP` | confirm | Force mission into `DESCENT` / `STOPPED` |
| `HEATERS_OFF` | confirm | Emergency heater shutoff |
| `RESET_CTRL` | confirm | Reset all per-sample PID integrators |
| `SHUTDOWN_SAFE` | confirm | Graceful process shutdown |
| `SET_TICK_HZ <hz>` | OK | Live downlink/loop rate, range `[0.1, 5.0]` |
| `RADIO_SILENCE` / `RADIO_RESUME` | OK | Halt / resume TX socket (queue keeps filling) |
| `ARM_DEBUG <token>` | — | Arm extended debug commands (bench mode only) |
| `SET_HEATER_DUTY <i> <0–1>` | debug | Override single heater (i in 0..5) |
| `SET_ALL_DUTY <0–1>` | debug | Override all heaters |
| `SET_PID <kp> <ki> <kd>` | debug | Override PID gains |
| `CLEAR_OVERRIDES` | debug | Clear all overrides |
| `SET_BENCH_MODE <1\|0>` | debug | Toggle bench mode |
| `STEPPER_MOVE <id> <steps>` | motion | Relative step move (id 0 or 1) |
| `STEPPER_BEND <id> <abs> [hold_s]` | motion | Absolute move tagged as a bend |
| `PULL_ARM <id>` / `PULL_EXECUTE <id>` | motion | Acquire `MotionLock`, run a pull cycle |

`confirm` = requires confirmation in GUI / `--yes` flag in CLI. `debug` = requires `ARM_DEBUG` first. `motion` commands acquire the `MotionLock` interlock; while held, all heater duties are forced to zero.

## Pi Boot Autostart

```bash
./scripts/install_onboard_service.sh /bexus/code/coatheal /bexus/code/coatheal/config/onboard.example.ini
sudo systemctl enable coatheal-onboard
sudo systemctl start coatheal-onboard
```

## Security

Historical credential exposure was remediated. See [docs/security-remediation.md](docs/security-remediation.md), [`scripts/rotate_ssh_key.sh`](scripts/rotate_ssh_key.sh), and [`scripts/purge_sensitive_history.sh`](scripts/purge_sensitive_history.sh).
