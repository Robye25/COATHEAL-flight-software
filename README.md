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
| [docs/rev-c-rtd-click-plug-and-play.md](docs/rev-c-rtd-click-plug-and-play.md) | Current bench setup: RTD Click/MAX31865 PT100, config migration, heater and motor checks |
| [docs/hardware.md](docs/hardware.md) | Final Rev C hardware reference and HAL status |
| [docs/configuration.md](docs/configuration.md) | Full INI configuration reference |
| [docs/component-configuration-and-bring-up.md](docs/component-configuration-and-bring-up.md) | Authoritative Rev C wiring, discovery, and commissioning guide |
| [docs/rev-c-instruction-manual.md](docs/rev-c-instruction-manual.md) | Complete installation, pin configuration, commissioning, operation, and troubleshooting manual |
| [docs/tmc2240-pin-configuration-and-commissioning.md](docs/tmc2240-pin-configuration-and-commissioning.md) | TMC2240 wiring, configurable pins, current setup, and supervised motor commissioning |
| [docs/protocol.md](docs/protocol.md) | DATA, `EVT,PULL`, ACK, discovery, and command protocol |
| [docs/manual-operations.md](docs/manual-operations.md) | Complete CLI workflow for thermal control, zeroing, bend sequences, fallback, and safe stop |
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

On the Pi, migrate stale config and install the service with the current bench
RTD Click settings:

```bash
python3 scripts/hardware_setup.py plug-and-play \
  --config config/onboard.local.ini \
  --migrate-from config/onboard.ini \
  --yes
```

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
| `BOOT` | Power-on | Explicit manual targets/duties only after `ARM` |
| `ASCENT` | `SET_PHASE ASCENT` / `FORCE_START` | Manual targets/duties; +5 C floor during fallback |
| `PRE_FLOAT` | `SET_PHASE PRE_FLOAT` | Manual control; no automatic fatigue pulls |
| `FLOAT` | `SET_PHASE FLOAT` | Manual control and operator-defined bend sequences |
| `DESCENT` | `SET_PHASE DESCENT` / `FORCE_STOP` | Manual targets/duties; +5 C floor during fallback |
| `LANDED` | `SET_PHASE LANDED` | Explicit manual targets/duties only |
| `STOPPED` | `SHUTDOWN_SAFE` | Heaters off |

## Final Rev C Component List

| Subsystem | Component | Interface |
|---|---|---|
| Stepper driver | TMC2240 carrier | SPI mode 3 + STEP/DIR/EN |
| Linear actuator | NEMA 17 external ball-screw linear stepper, 2.5 A, 48 mm | Through TMC2240 |
| Current bench PT100 | XF-931-FAR PT100 Class B probe | RTD Click MIKROE-2815 / MAX31865 |
| Future multi-channel PT100 | DAQ132M 8-channel PT100 card | USB-RS485 Modbus RTU, currently disabled |
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
| `PING` / `STATUS` / `CHECK` | Link, live state, and active hardware checks |
| `ARM` / `DISARM` | Enable or disable manual flight outputs |
| `SET_PHASE <phase>` | Manually set mission phase |
| `SET_TEMP_TARGET <i> <C>` | Set one closed-loop heater target |
| `SET_PID <i\|ALL> <kp> <ki> <kd>` | Tune one or all heater PID loops |
| `SET_HEATER_DUTY <i> <0-1>` | Set one heater duty |
| `SET_ALL_DUTY <0-1>` | Set all heater duties |
| `HEATER_TEST <i> <duty> <seconds>` | Bench-only bounded heater pulse after debug arm |
| `HEATERS_OFF` | Emergency heater shutoff |
| `SET_POSITION_ZERO <id>` | Declare the current physical motor position as zero |
| `BENDSEQ_LOAD` / `BENDSEQ_RUN` | Define and execute absolute bend sequences |
| `STEPPER_*` | Direct motor movement commands |
| `SHUTDOWN_SAFE` | Flush logs and stop onboard process |

See [docs/protocol.md](docs/protocol.md) for the complete command list.

## Hardware Status

The real hardware paths are implemented for libgpiod heater PWM, TMC2240
configuration with GPIO chip-select, STEP/DIR/EN pulses, DPS310, ADS1115,
RTD Click/MAX31865, and configurable DAQ132M Modbus RTU. The current bench
default uses RTD Click and disables DAQ132M. `runtime.use_simulated_sensors=true`
and `runtime.use_simulated_pwm=true` are explicit debug-only switches. Bench
validation, motor current calibration, and dummy-load heater tests are still
required before powered flight hardware operation.

## Security

Historical credential exposure was remediated. See
[docs/security-remediation.md](docs/security-remediation.md),
[scripts/rotate_ssh_key.sh](scripts/rotate_ssh_key.sh), and
[scripts/purge_sensitive_history.sh](scripts/purge_sensitive_history.sh).
