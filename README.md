# COATHEAL Flight Software

COATHEAL is a BEXUS high-altitude balloon experiment investigating thermal
self-healing materials under stratospheric cold soak with manual mechanical
pull-induced microcracking. This repository contains the onboard Raspberry Pi
C++17 flight software and the Python ground station.

Rev C is manual-first. While the ground link is healthy, operators command
phase, heater duties, and motor pulls. The onboard software keeps telemetry,
logging, safety interlocks, watchdog recovery, and link-loss fallback.

```text
Raspberry Pi 4                         Ground station laptop
coatheal_onboard                       gui_app.py / CLI
  TCP command server :5000  <--------  one-shot commands
  TCP telemetry client :4000 --------> telemetry receiver + ACKs
  UDP discovery :4100       <------->  discovery beacons
  durable telemetry queue              CSV logs + live plots
```

## Repository Layout

```text
onboard/         C++17 flight software
tests/           C++ unit tests
ground-station/  Python GUI, CLI, and protocol library
config/          Runtime INI templates
deploy/          systemd unit files
scripts/         Setup, preflight, and security scripts
docs/            Architecture, protocol, configuration, and hardware docs
```

## Documentation

| Document | Description |
|---|---|
| [docs/rev-c-installation-and-hardware-setup.md](docs/rev-c-installation-and-hardware-setup.md) | Installation, plug-and-play Ethernet, final component setup, pins, and commands |
| [docs/hardware.md](docs/hardware.md) | Final Rev C hardware reference and HAL status |
| [docs/configuration.md](docs/configuration.md) | Full INI configuration reference |
| [docs/protocol.md](docs/protocol.md) | DATA, `EVT,PULL`, ACK, discovery, and command protocol |
| [docs/onboard.md](docs/onboard.md) | Onboard C++ module reference |
| [docs/ground-station.md](docs/ground-station.md) | Ground station GUI/CLI reference |
| [docs/architecture.md](docs/architecture.md) | System architecture and data flow |
| [docs/deployment.md](docs/deployment.md) | Pi service installation notes |
| [docs/development.md](docs/development.md) | Build, test, and bench workflow |

## Quick Start: Ground Station

```powershell
cd D:\COATHEAL-flight-software\COATHEAL-flight-software
cd ground-station
py -3 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
pip install -r requirements.txt
python gui_app.py
```

With no `--host`, the GUI probes the Pi at `169.254.10.10:5000`, listens for
telemetry on `4000`, and runs UDP discovery on `4100`.

## Quick Start: Onboard

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/onboard/coatheal_onboard --config config/onboard.debug.ini
```

On the Pi, use `config/onboard.local.ini` copied from
`config/onboard.example.ini`.

## Build and Test

```bash
cmake -S . -B build
cmake --build build --config Release --parallel
ctest --test-dir build --output-on-failure
python -m unittest discover -s ground-station/tests -p "test_*.py"
```

## Mission Phases

| Phase | Normal Rev C trigger | Thermal behavior |
|---|---|---|
| `BOOT` | Power-on | Heaters off |
| `ASCENT` | `SET_PHASE ASCENT` / `FORCE_START` | Manual duties while connected; +5 C floor only during fallback |
| `PRE_FLOAT` | `SET_PHASE PRE_FLOAT` | Manual duties; no automatic fatigue pulls |
| `FLOAT` | `SET_PHASE FLOAT` | Explicit `PULL_*` commands |
| `DESCENT` | `SET_PHASE DESCENT` / `FORCE_STOP` | Manual duties while connected; +5 C floor only during fallback |
| `LANDED` | `SET_PHASE LANDED` | Heaters off unless manually overridden |
| `STOPPED` | `SHUTDOWN_SAFE` | Heaters off |

## Final Rev C Component List

| Subsystem | Component | Interface |
|---|---|---|
| Stepper driver | FYSETC TMC5160 | SPI + STEP/DIR/EN |
| Linear actuator | NEMA 17 external ball-screw linear stepper, 2.5 A, 48 mm | Through TMC5160 |
| Sample PT100s | XF-931-FAR PT100 Class B probes | Into DAQ132M |
| Temperature DAQ | DAQ132M 8-channel PT100 card | USB-RS485 Modbus RTU |
| Optional bench PT100 | RTD Click MIKROE-2815 / MAX31865 | SPI + DRDY |
| Pressure / ambient T | Adafruit DPS310 | I2C / STEMMA QT |
| UV ADC | Adafruit ADS1115 | I2C / STEMMA QT |
| UV sensor | GUVA-S12SD | Analog into ADS1115 |
| Heater switching | Electrokit EKM014 UCC27524 4-channel MOSFET driver board | GPIO PWM inputs |
| Heaters | Polyimide film heaters | MOSFET outputs |
| Power rails | Pololu D24V50F5 5 V and D42V110F12 12 V regulators | DC power |
| Pi breakout | Pi-EzConnect Terminal Block Breakout HAT | Wiring breakout |

## Command Summary

| Command | Description |
|---|---|
| `PING` / `STATUS` | Link and system status |
| `ARM` / `DISARM` | Enable or disable manual flight outputs |
| `SET_PHASE <phase>` | Manually set mission phase |
| `SET_HEATER_DUTY <i> <0-1>` | Set one heater duty |
| `SET_ALL_DUTY <0-1>` | Set all heater duties |
| `HEATERS_OFF` | Emergency heater shutoff |
| `PULL_ARM <id>` / `PULL_EXECUTE <id>` | Run one motor pull cycle |
| `STEPPER_*` | Direct motor movement commands |
| `SHUTDOWN_SAFE` | Flush logs and stop onboard process |

See [docs/protocol.md](docs/protocol.md) for the complete command list.

## Hardware Status

The software is configured for the final component list. The TMC5160 SPI setup
path is implemented. The remaining physical I/O work before powered hardware
operation is bench validation and completion of real GPIO PWM/pulse timing,
DPS310/ADS1115 I2C reads, and DAQ132M Modbus reads.

## Security

Historical credential exposure was remediated. See
[docs/security-remediation.md](docs/security-remediation.md),
[scripts/rotate_ssh_key.sh](scripts/rotate_ssh_key.sh), and
[scripts/purge_sensitive_history.sh](scripts/purge_sensitive_history.sh).
