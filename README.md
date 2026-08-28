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
  durable telemetry queue              per-session logs + live plots
```

## Deployment quickstart (plug and play)

**Onboard (Raspberry Pi)** — SSH in. On a brand-new Pi the repository isn't
there yet, so clone it once (the repo is public — no credentials needed):

```bash
sudo mkdir -p /bexus/code && sudo chown -R $USER /bexus
git clone https://github.com/Robye25/COATHEAL-flight-software.git /bexus/code/coatheal
```

Then run the deploy script. It pulls the latest code, retires any previously
installed COATHEAL iteration, migrates the existing config to the current
schema (original backed up), rebuilds, validates the config with the flight
binary, and installs + starts the service:

```bash
bash /bexus/code/coatheal/deploy_onboard.sh   # first time
coatheal-deploy                               # every time after that
```

The script ends with a green `DEPLOYED and RUNNING` banner and the Pi's IP
addresses. Add `--dry-run` to see every action without changing anything.

**Ground station** — a fixed-layout mission console (System / Thermal /
Motion / Advanced tabs, mission-time plots, alarm strip, command console;
see [docs/ground-station.md](docs/ground-station.md)). Every session's
telemetry, commands and events land in `ground-station/logs/sessions/`.

**Ground station (Windows)** — double-click
`ground-station/COATHEAL-GroundStation.bat`. The first run sets up a local
Python environment and offers a one-click firewall configuration (needed for
onboard auto-discovery); every later run launches instantly. The GUI then
discovers and connects to the onboard automatically — no addresses to type.
Diagnostics without opening the GUI: run the same file with `--check`.

**Ground station (Linux)** — same experience via
`ground-station/COATHEAL-GroundStation.sh` (run it from a terminal or
double-click → "Run in Terminal"). It also handles `ufw` instead of the
Windows firewall. `--check` works identically.

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
| [docs/sequent-rtd-bring-up.md](docs/sequent-rtd-bring-up.md) | Sequent RTD HAT bench bring-up (register-map verification gate, burst-read confirmation, calibration) and MAX31865 sample-resistance click bring-up (reference resistor, coating-resistance range, sample-index mapping) |
| [docs/tmc5160-commissioning.md](docs/tmc5160-commissioning.md) | TMC5160 SPI-only motion commissioning: v3 pin map, four-device SPI0 topology, current model, bench gates |
| [docs/hardware.md](docs/hardware.md) | Final schematic v3 hardware reference and HAL status |
| [docs/configuration.md](docs/configuration.md) | Full INI configuration reference |
| [docs/component-configuration-and-bring-up.md](docs/component-configuration-and-bring-up.md) | Authoritative v3 wiring, discovery, and commissioning guide |
| [docs/rev-c-instruction-manual.md](docs/rev-c-instruction-manual.md) | Complete installation, pin configuration, commissioning, operation, and troubleshooting manual |
| [docs/protocol.md](docs/protocol.md) | DATA, `EVT,PULL`, ACK, discovery, and command protocol |
| [docs/manual-operations.md](docs/manual-operations.md) | Complete CLI workflow for thermal control, zeroing, bend sequences, fallback, and safe stop |
| [docs/onboard.md](docs/onboard.md) | Onboard C++ module reference |
| [docs/ground-station.md](docs/ground-station.md) | Ground station console and CLI reference |
| [docs/superpowers/specs/2026-08-28-ground-station-redesign-design.md](docs/superpowers/specs/2026-08-28-ground-station-redesign-design.md) | Ground-station redesign: owner decisions, layout, protocol additions, radio silence, link-loss failsafe |
| [docs/architecture.md](docs/architecture.md) | System architecture and data flow |
| [deploy/README.md](deploy/README.md) | systemd units, one-command deploy, storage layout, ground-station firewall |
| [docs/development.md](docs/development.md) | Build, test, and bench workflow |

## Quick Start: Ground Station

```powershell
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

On the Pi, migrate stale config and install the service:

```bash
python3 scripts/hardware_setup.py migrate-config \
  --config config/onboard.local.ini \
  --migrate-from config/onboard.local.ini \
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

## Final Rev C Component List (Schematic v3)

| Subsystem | Component | Interface |
|---|---|---|
| Stepper driver | TMC5160 carrier (QHV5160 v2) | SPI-only position dribble, no STEP/DIR |
| Linear actuator | NEMA 17 external ball-screw linear stepper, 2.5 A, 48 mm | Driven by TMC5160 over SPI |
| Sample PT100 | 8x XF-931-FAR PT100 Class B probes | Sequent Microsystems 8-channel RTD HAT, I2C `0x40 + stack` |
| Sample resistance | 2x MikroE RTD Click (MAX31865), 4-wire Kelvin per specimen | SPI0 native CS (CE0/CE1) |
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
| `PULL_EXECUTE <id>` | One config-defined standard pull (the console's STANDARD PULL) |
| `RADIO_SILENCE` / `RADIO_RESUME` | Stop / restart every onboard transmission (telemetry, beacons, hello replies); only `RADIO_RESUME`, `STATUS` and `PING` are answered while silent |
| `FALLBACK_PLAN <id> <target> <hold_s> [hz]` / `FALLBACK_ARM` / `FALLBACK_DISARM` / `FALLBACK_STATUS` | Operator-armed bend plan the onboard executes on its own only during link-loss fallback at PRE_FLOAT/FLOAT |
| `SHUTDOWN_SAFE` | Flush logs and stop onboard process |

See [docs/protocol.md](docs/protocol.md) for the complete command list.

## Hardware Status

The real hardware paths are implemented for libgpiod heater PWM, TMC5160
SPI-only position-dribble motion (software chip-select, no STEP/DIR — see
[docs/tmc5160-commissioning.md](docs/tmc5160-commissioning.md)), the
MAX31865 dual-click sample-resistance instrument, DPS310, ADS1115, and the
Sequent Microsystems 8-channel RTD HAT over I2C.
`runtime.use_simulated_sensors=true` and `runtime.use_simulated_pwm=true` are
explicit debug-only switches. The RTD HAT's register map is derived from
vendor source and gated on bench verification (see
[docs/sequent-rtd-bring-up.md](docs/sequent-rtd-bring-up.md)); motor current
calibration (including confirming the TMC5160 boards' actual sense-resistor
value), the MAX31865 clicks' reference-resistor value and coating-resistance
range, and dummy-load heater tests are also still required before powered
flight hardware operation.

## Security

Historical credential exposure was remediated. See
[docs/security-remediation.md](docs/security-remediation.md),
[scripts/rotate_ssh_key.sh](scripts/rotate_ssh_key.sh), and
[scripts/purge_sensitive_history.sh](scripts/purge_sensitive_history.sh).
