# Rev C Installation and Hardware Setup

This is the operator-facing setup guide for the Rev C software branch.

Rev C is manual-first. While the ground station link is healthy, the onboard
software does not run autonomous phase-entry motion, fatigue pulls, or automatic
thermal sequencing. Operators command phase, heaters, and pull cycles from the
ground station. If an established link is lost for the configured timeout, the
onboard can fall back to pressure-based phase tracking and the sample +5 C
floor controller.

The final component list is still pending. Until that list is frozen, treat the
pin table below as the current software contract plus known gaps, not as a
flight wiring release.

## Source of Truth

- Git branch: `rev-c`
- Raspberry Pi project path: `/bexus/code/coatheal`
- Flight config template: `config/onboard.example.ini`
- Recommended local Pi config: `config/onboard.local.ini`
- Ground station entry point: `ground-station/gui_app.py`
- Onboard binary: `build/onboard/coatheal_onboard`
- Onboard systemd service: `coatheal-onboard.service`
- Ethernet command port: `5000/tcp`
- Ground station telemetry port: `4000/tcp`
- Discovery port: `4100/udp`
- Static onboard Ethernet IP: `169.254.10.10/16`

Use BCM GPIO line numbers everywhere. Do not use physical header pin numbers in
the config unless a document explicitly says so.

## Current Hardware Support Status

| Area | Current status | Action before flight wiring |
|---|---|---|
| Manual command protocol | Implemented | Use GUI/CLI commands listed below |
| Link-loss fallback | Implemented | Verify `manual.link_loss_fallback_s` during bench tests |
| Plug-and-play Ethernet | Implemented | Pi must keep `169.254.10.10/16`; Windows firewall must allow inbound telemetry |
| Telemetry logging and queue | Implemented | Verify SD/USB paths on the Pi |
| Status LEDs | Software state implemented; real GPIO write path is still a libgpiod boundary stub | Confirm whether real LED toggling is required, then finish GPIO backend |
| Heater PWM | Scheduler and command logic implemented; real PWM waveform backend is not implemented | Freeze heater MOSFET pins and implement/test real PWM output |
| Stepper control | Command/state logic and TMC2240 SPI register writes exist; STEP/DIR/EN pulse backend is still a stub | Freeze driver modules, pins, pulse timing, and implement/test real pulses |
| MS5803 / ADS1015 / DS3231 I2C | Adapter stubs only | Implement device-specific I2C reads |
| INA3221 resistance | Adapter stub returns zeros | Implement real I2C channel reads for addresses `0x40` and `0x41` |
| PT100 over RS485/Modbus | Not implemented | Add Modbus RTU adapter after collector model/register map is confirmed |

This means the current software is suitable for command, telemetry, GUI, service,
networking, queueing, and bench-state validation. It is not yet a complete
physical I/O release for final flight wiring.

## Ground Station Installation

Use Windows PowerShell from the repository root.

```powershell
cd D:\COATHEAL-flight-software\COATHEAL-flight-software
git switch rev-c
git pull --ff-only origin rev-c
cd ground-station
py -3 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
pip install -r requirements.txt
```

Allow inbound telemetry and discovery on the link-local Ethernet interface. Run
PowerShell as Administrator:

```powershell
cd D:\COATHEAL-flight-software\COATHEAL-flight-software
Set-ExecutionPolicy -Scope Process Bypass
.\ground-station\scripts\configure_firewall.ps1
```

Start the GUI:

```powershell
cd D:\COATHEAL-flight-software\COATHEAL-flight-software\ground-station
.\.venv\Scripts\Activate.ps1
python gui_app.py
```

Default behavior:

1. The telemetry server listens on `0.0.0.0:4000`.
2. The GUI broadcasts discovery on UDP `4100`.
3. The GUI probes `169.254.10.10:5000` with command pings.
4. A successful command connection teaches the Pi which laptop IP should receive
   telemetry.

CLI command examples:

```powershell
cd D:\COATHEAL-flight-software\COATHEAL-flight-software\ground-station
.\.venv\Scripts\Activate.ps1
python main.py command --cmd PING
python main.py command --cmd STATUS
python main.py command --cmd ARM
python main.py command --cmd "SET_PHASE ASCENT"
python main.py command --cmd "SET_HEATER_DUTY 0 0.25"
python main.py command --cmd HEATERS_OFF --yes
```

## Raspberry Pi Installation

Use Raspberry Pi OS 64-bit. Bookworm is preferred. The service file currently
runs as user `coatheal`, so either create that user or update
`deploy/coatheal-onboard.service` before installing the service.

Install system packages:

```bash
sudo apt update
sudo apt install -y git cmake g++ libgpiod-dev python3 python3-venv netcat-openbsd i2c-tools
```

Create the service user if it does not already exist:

```bash
id coatheal || sudo adduser --disabled-password --gecos "" coatheal
```

Enable hardware interfaces:

```bash
sudo raspi-config nonint do_i2c 0
sudo raspi-config nonint do_spi 0
sudo usermod -aG gpio,i2c,spi,dialout coatheal
sudo reboot
```

Clone or update the repository:

```bash
sudo mkdir -p /bexus/code
sudo chown coatheal:coatheal /bexus/code
sudo -u coatheal git clone --branch rev-c https://github.com/Robye25/COATHEAL-flight-software.git /bexus/code/coatheal
cd /bexus/code/coatheal
git switch rev-c
git pull --ff-only origin rev-c
```

If the repository already exists on the Pi:

```bash
cd /bexus/code/coatheal
git fetch origin
git switch rev-c
git pull --ff-only origin rev-c
```

Create a local config file. Do not commit local credentials, local paths, or
machine-specific values.

```bash
cd /bexus/code/coatheal
cp config/onboard.example.ini config/onboard.local.ini
nano config/onboard.local.ini
```

Build and test:

```bash
cd /bexus/code/coatheal
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Install the flight service:

```bash
cd /bexus/code/coatheal
sudo ./scripts/install_onboard_service.sh /bexus/code/coatheal /bexus/code/coatheal/config/onboard.local.ini
sudo systemctl status coatheal-onboard.service
sudo systemctl status coatheal-link-watch.path
```

Useful service commands:

```bash
sudo systemctl restart coatheal-onboard.service
sudo systemctl stop coatheal-onboard.service
sudo systemctl start coatheal-onboard.service
sudo journalctl -u coatheal-onboard.service -f
sudo journalctl -u coatheal-link-watch.service -f
```

## Plug-and-Play Ethernet

The intended workflow is: plug in Ethernet, start the GUI, wait for command and
telemetry connection.

The Pi should keep the fixed link-local address `169.254.10.10/16`. The laptop
may use any `169.254.x.x/16` address. The ground station probes
`169.254.10.10:5000`; when the Pi receives a command, it uses the command peer
IP as the telemetry return target.

NetworkManager setup on the Pi:

```bash
nmcli -t -f NAME,DEVICE con show
sudo nmcli con mod "Wired connection 1" ipv4.method manual ipv4.addresses 169.254.10.10/16 ipv4.gateway "" ipv4.dns "" ipv6.method ignore connection.autoconnect yes
sudo nmcli con up "Wired connection 1"
ip -4 addr show eth0
```

If the Pi image uses `dhcpcd` instead of NetworkManager:

```bash
sudo cp /etc/dhcpcd.conf /etc/dhcpcd.conf.backup
sudo tee -a /etc/dhcpcd.conf >/dev/null <<'EOF'

interface eth0
static ip_address=169.254.10.10/16
EOF
sudo systemctl restart dhcpcd
ip -4 addr show eth0
```

Windows verification:

```powershell
Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -like "169.254.*" }
Test-NetConnection 169.254.10.10 -Port 5000
```

Pi verification:

```bash
ip -4 addr show eth0
ss -ltnp | grep ':5000'
```

Ground station verification:

```powershell
cd D:\COATHEAL-flight-software\COATHEAL-flight-software\ground-station
.\.venv\Scripts\Activate.ps1
python main.py command --cmd STATUS
```

Expected `STATUS` fields for a connected Rev C system include:

```text
manual_first=1
link_seen=1
fallback_active=0
telemetry_target=<laptop-link-local-ip>
```

## Manual-First Configuration

These keys are the important Rev C behavior controls:

```ini
manual.manual_first=true
manual.link_loss_fallback_enabled=true
manual.link_loss_fallback_s=10.0

comms.telemetry_host=
comms.static_ground_ip=
comms.static_pi_ip=169.254.10.10
comms.telemetry_port=4000
comms.command_port=5000
comms.discovery_enabled=true
comms.discovery_port=4100

runtime.bench_mode=false
runtime.use_simulated_pwm=false
runtime.gpio_chip=/dev/gpiochip0
```

Operational meaning:

| Key | Meaning |
|---|---|
| `manual.manual_first=true` | Operators command phase, heater duties, and pulls while connected |
| `manual.link_loss_fallback_enabled=true` | Onboard may use fallback automation only after link loss |
| `manual.link_loss_fallback_s=10.0` | Seconds after link loss before fallback activates |
| `comms.telemetry_host=` | Empty enables plug-and-play targeting from discovery/command peer |
| `comms.static_ground_ip=` | Optional fixed laptop IP; leave empty for plug-and-play |
| `comms.static_pi_ip=169.254.10.10` | Static onboard address probed by the GUI |

## Operator Command Sequence

Basic connected operation:

```powershell
python main.py command --cmd STATUS
python main.py command --cmd ARM
python main.py command --cmd "SET_PHASE ASCENT"
python main.py command --cmd "SET_ALL_DUTY 0.00"
python main.py command --cmd "SET_HEATER_DUTY 0 0.20"
python main.py command --cmd "SET_HEATER_DUTY 1 0.20"
python main.py command --cmd "PULL_ARM 0"
python main.py command --cmd "PULL_EXECUTE 0"
python main.py command --cmd CLEAR_OVERRIDES
python main.py command --cmd HEATERS_OFF --yes
python main.py command --cmd DISARM
```

Emergency commands:

```powershell
python main.py command --cmd HEATERS_OFF --yes
python main.py command --cmd FORCE_STOP --yes
python main.py command --cmd SHUTDOWN_SAFE --yes
```

Do not use `ARM_DEBUG`, `SET_BENCH_MODE`, or `SET_PID` during flight operations
unless the test procedure explicitly calls for them.

## Component and Pin Configuration

### GPIO Numbering

The config uses BCM GPIO line numbers through `/dev/gpiochip0`.

Example:

```ini
hal.status_led_line=17
hal.mode_led_line=27
stepper.step_line=5
stepper.dir_line=6
stepper.enable_line=13
```

### Current Pin and Bus Table

| Function | Current config/software value | Current implementation state |
|---|---|---|
| Heartbeat LED | `hal.status_led_line=17` | State tracking only; real GPIO write backend pending |
| Mode LED | `hal.mode_led_line=27` | State tracking only; real GPIO write backend pending |
| Shared STEP | `stepper.step_line=5` | Parsed; real STEP pulse backend pending |
| Shared DIR | `stepper.dir_line=6` | Parsed; real DIR write backend pending |
| Shared EN | `stepper.enable_line=13` | Parsed; real EN write backend pending |
| Motor 0 SPI | Hardcoded `/dev/spidev1.0` | TMC2240 register writes implemented |
| Motor 1 SPI | Hardcoded `/dev/spidev1.1` | TMC2240 register writes implemented |
| Motor 0 sample group | Hardcoded samples `0,1,2,3` | Used by pull events and resistance simulation |
| Motor 1 sample group | Hardcoded samples `4,5,6,7` | Used by pull events and resistance simulation |
| Heater channels | `hardware.heater_count=6` | Scheduler and command state only; real PWM backend pending |
| I2C bus | Pi `/dev/i2c-1` | Device drivers pending |
| INA3221 addresses | `0x40`, `0x41` | Adapter stub pending real reads |
| ADS1015 address | usually `0x48` | Driver pending |
| DS3231 address | usually `0x68` | Driver pending |
| PT100 collectors | USB-RS485, expected `/dev/ttyUSB0` | Modbus adapter pending |

Important constraint: `motor0.*` and `motor1.*` keys in
`config/onboard.example.ini` are not currently wired into `config.cpp` or
`SystemController`. The active stepper GPIO pins are the shared `stepper.*`
keys. The active SPI device paths are hardcoded in `SystemController`.

### I2C Components

Enable I2C, then verify connected devices:

```bash
sudo raspi-config nonint do_i2c 0
sudo reboot
i2cdetect -y 1
```

Expected addresses after final wiring, subject to the final component list:

| Component | Expected address | Software path |
|---|---|---|
| INA3221 chip A | `0x40` | `onboard/include/coatheal/hal/ina3221_adapter.hpp` |
| INA3221 chip B | `0x41` | `onboard/include/coatheal/hal/ina3221_adapter.hpp` |
| ADS1015 | `0x48` by default | Pending ADS1015 read path in `SensorManager` |
| DS3231 RTC | `0x68` | `RtcAdapter` |
| MS5803 | module-dependent | Pending MS5803 read path in `SensorManager` |

Do not connect two devices with the same I2C address unless one address can be
changed by solder jumper or the bus is split through a mux.

### SPI / Stepper Components

Enable SPI and verify spidev nodes:

```bash
sudo raspi-config nonint do_spi 0
sudo reboot
ls -l /dev/spidev*
```

Current software expects:

```text
/dev/spidev1.0 -> motor 0 TMC2240 configuration
/dev/spidev1.1 -> motor 1 TMC2240 configuration
```

The TMC2240 SPI path programs current and chopper registers. The STEP/DIR/EN
pulse backend still needs the final driver timing and real GPIO writes. Before
benching motors, complete and test that backend with the final driver module,
logic voltage, enable polarity, and pulse-width requirements.

### Heater Components

Current thermal config:

```ini
hardware.heater_count=6
power.max_active_heaters=4
power.max_thermal_w=20.0
power.heater_nominal_w=5.0
```

The command layer supports:

```powershell
python main.py command --cmd "SET_HEATER_DUTY 0 0.25"
python main.py command --cmd "SET_ALL_DUTY 0.10"
python main.py command --cmd CLEAR_OVERRIDES
python main.py command --cmd HEATERS_OFF --yes
```

The physical MOSFET GPIO mapping is not finalized in software. Before connecting
heater power:

1. Freeze the six MOSFET input GPIO lines.
2. Add config keys for heater channel to BCM mapping.
3. Implement real PWM waveform output.
4. Test each channel with a dummy load and current limit.
5. Verify `HEATERS_OFF` drives every channel off.

### PT100 / RS485 Components

The expected physical layout is two 4-channel RTD collector modules on one
USB-RS485 adapter, normally appearing as `/dev/ttyUSB0`.

Before implementing the driver, capture:

| Required detail | Why it matters |
|---|---|
| Collector model and datasheet | Confirms protocol and register map |
| Slave address for each collector | Avoids bus conflicts |
| Baud rate, parity, stop bits | Required for Modbus RTU framing |
| Register type and scale | Converts raw register values to C |
| Channel order | Maps PT100 channel to sample index |

Useful bring-up commands:

```bash
ls -l /dev/ttyUSB*
dmesg | tail -50
sudo usermod -aG dialout coatheal
```

## Bench Verification Checklist

Run these before connecting final heaters or motors:

```bash
cd /bexus/code/coatheal
git rev-parse --abbrev-ref HEAD
git log -1 --oneline --decorate --date=iso
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
sudo ./scripts/preflight_healthcheck.sh config/onboard.local.ini
sudo systemctl restart coatheal-onboard.service
sudo systemctl status coatheal-onboard.service
```

From the ground station:

```powershell
python main.py command --cmd PING
python main.py command --cmd STATUS
python main.py command --cmd ARM
python main.py command --cmd "SET_PHASE ASCENT"
python main.py command --cmd "SET_ALL_DUTY 0.00"
python main.py command --cmd HEATERS_OFF --yes
python main.py command --cmd DISARM
```

Expected checks:

| Check | Pass condition |
|---|---|
| Service health | `systemctl status` shows active/running |
| Command link | `PING` returns an ACK |
| Manual mode | `STATUS` shows `manual_first=1` |
| Fallback inactive while connected | `STATUS` shows `fallback_active=0` |
| Telemetry target | `STATUS` shows the laptop link-local IP |
| Queue health | `queue_depth` does not grow while telemetry GUI is connected |
| Heater emergency | `HEATERS_OFF` ACKs and duty telemetry goes to zero |

## Information Needed for the Final Component List

When the component list is ready, provide this for each component:

| Field | Example |
|---|---|
| Component name and exact part number | `ADS1015 module, default address 0x48` |
| Electrical interface | I2C, SPI, GPIO PWM, USB-RS485, UART |
| Logic voltage | 3.3 V or level-shifted 5 V |
| Power rail | 3.3 V, 5 V, 24 V, 28.8 V |
| Signal pins | BCM GPIO line numbers or bus node |
| Address / chip select / slave ID | I2C address, SPI device, Modbus ID |
| Protocol settings | SPI mode/speed, Modbus baud/parity/registers |
| Data scale | raw register to engineering units |
| Failure behavior | safe default when read/write fails |
| Bench validation command | command that proves it works |

After that list is frozen, the required software work is:

1. Add explicit config keys for every physical pin and bus parameter.
2. Wire those keys into `config.cpp` and the relevant controller/HAL classes.
3. Replace remaining GPIO/I2C/RS485 stubs with real hardware access.
4. Add unit tests for parsing, bounds, and failure behavior.
5. Add Pi bench tests for each device family.
6. Update this document and `docs/hardware.md` with the verified pin map.

## Troubleshooting

No command connection:

```powershell
Test-NetConnection 169.254.10.10 -Port 5000
```

```bash
sudo systemctl status coatheal-onboard.service
sudo journalctl -u coatheal-onboard.service -n 100 --no-pager
ip -4 addr show eth0
ss -ltnp | grep ':5000'
```

No telemetry in GUI:

```powershell
Get-NetFirewallRule | Where-Object { $_.DisplayName -like "*COATHEAL*" }
Test-NetConnection 169.254.10.10 -Port 5000
python main.py command --cmd STATUS
```

Check the `telemetry_target` field in `STATUS`. If it is empty or not the
laptop link-local IP, send another `STATUS` command from the GUI/CLI so the Pi
learns the command peer address.

Service restarts on cable replug:

```bash
sudo journalctl -u coatheal-link-watch.service -n 50 --no-pager
sudo systemctl status coatheal-link-watch.path
```

The link watcher intentionally restarts the onboard process after NIC up/down
events so the telemetry path reopens cleanly.

Hardware bus missing:

```bash
ls -l /dev/i2c-1
ls -l /dev/spidev*
ls -l /dev/ttyUSB*
gpioinfo gpiochip0
```

If device nodes are missing, re-enable the interface with `raspi-config`, reboot,
and confirm the service user belongs to the required Linux groups.
