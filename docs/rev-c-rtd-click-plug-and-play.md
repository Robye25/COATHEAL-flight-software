# Rev C RTD Click Plug-And-Play Bring-Up

This is the current Rev C bench procedure. It replaces the older DAQ132M-first
temperature path until the damaged Modbus hardware is replaced.

Current temperature source:

- One PT100 probe through RTD Click MIKROE-2815 / MAX31865.
- RTD Click on SPI0 with manual chip select on BCM 7 and DRDY on BCM 25.
- Software sample channel `S0` by default.
- DAQ132M disabled by default, but the DAQ code and config remain available for
  later replacement hardware.

Normal heater control remains strict: a heater can run only when its mapped
temperature channel is valid and fresh. For bench commissioning only, the
bounded `HEATER_TEST` command can pulse one heater without valid feedback after
bench debug arming.

Reference documents:

- RTD Click: <https://www.mikroe.com/rtd-click>
- MAX31865 datasheet: <https://www.analog.com/media/en/technical-documentation/data-sheets/max31865.pdf>
- Final pin map: [hardware.md](hardware.md)
- TMC2240 commissioning: [tmc2240-pin-configuration-and-commissioning.md](tmc2240-pin-configuration-and-commissioning.md)

## Final Bench Pin Map

Use BCM numbering in all config files and commands. Physical header pins are
included only to avoid wiring confusion.

| Function | Physical pin | BCM GPIO | Config key |
|---|---:|---:|---|
| Heater 0 / HEAT_EN1 | 11 | 17 | `heater.output_lines[0]` |
| Heater 1 / HEAT_EN2 | 12 | 18 | `heater.output_lines[1]` |
| Heater 2 / HEAT_EN3 | 13 | 27 | `heater.output_lines[2]` |
| Heater 3 / HEAT_EN4 | 29 | 5 | `heater.output_lines[3]` |
| Heater 4 / HEAT_EN5 | 31 | 6 | `heater.output_lines[4]` |
| Heater 5 / HEAT_EN6 | 33 | 13 | `heater.output_lines[5]` |
| Motor 0 CS | 15 | 22 | `motor0.cs_line` |
| Motor 0 EN | 32 | 12 | `motor0.enable_line` |
| Motor 0 STEP | 35 | 19 | `motor0.step_line` |
| Motor 0 DIR | 37 | 26 | `motor0.dir_line` |
| Motor 1 CS | 16 | 23 | `motor1.cs_line` |
| Motor 1 STEP | 36 | 16 | `motor1.step_line` |
| Motor 1 DIR | 38 | 20 | `motor1.dir_line` |
| Motor 1 EN | 40 | 21 | `motor1.enable_line` |
| RTD Click CS | 26 | 7 | `sensor.rtd_click_cs_line` |
| RTD Click DRDY | 22 | 25 | `sensor.rtd_click_drdy_line` |
| SPI0 MOSI / MISO / SCLK | 19 / 21 / 23 | 10 / 9 / 11 | fixed SPI0 |
| I2C SDA / SCL | 3 / 5 | 2 / 3 | fixed I2C-1 |

All grounds must be common: Pi, RTD Click, TMC2240 carriers, MOSFET boards,
regulators, and sensors. Heater and motor load current must not return through
sensor ground wiring.

## Current Required Config

The setup script migrates stale `config/onboard.ini` into
`config/onboard.local.ini`. It removes legacy TMC5160 and old `stepper.*` pin
keys, forces TMC2240 drivers, applies the final pin map, and switches sample
temperature acquisition to RTD Click.

Expected temperature config:

```ini
sensor.sample_temperature_source=rtd_click_max31865
sensor.daq132m_enabled=false
sensor.rtd_click_enabled=true
sensor.rtd_click_spi_device=/dev/spidev0.0
sensor.rtd_click_cs_line=7
sensor.rtd_click_drdy_line=25
sensor.rtd_click_wires=3
sensor.rtd_click_sample_channel=0
sensor.rtd_click_reference_ohm=400.0
sensor.rtd_click_filter_hz=50
sensor.rtd_click_spi_speed_hz=500000
```

Expected motor pins:

```ini
motor0.driver=tmc2240
motor0.gpio_chip=/dev/gpiochip0
motor0.spi_device=/dev/spidev0.0
motor0.cs_line=22
motor0.enable_line=12
motor0.step_line=19
motor0.dir_line=26

motor1.driver=tmc2240
motor1.gpio_chip=/dev/gpiochip0
motor1.spi_device=/dev/spidev0.0
motor1.cs_line=23
motor1.enable_line=21
motor1.step_line=16
motor1.dir_line=20
```

Expected heater pins and mappings:

```ini
hardware.heater_count=6
heater.output_lines=17,18,27,5,6,13
heater.temperature_channels=0,1,2,3,4,5
heater.debug_max_duty=0.25
heater.debug_max_seconds=10.0
```

With only one RTD Click on `S0`, normal heater control is valid only for the
heater mapped to `S0` by default (`H0`). Other heaters stay inhibited until their
temperature channels are valid or until you use explicit bench `HEATER_TEST`.

## One-Command Pi Setup

Run this from the repository root on the Pi:

```bash
cd /bexus/code/coatheal
python3 scripts/hardware_setup.py plug-and-play \
  --config config/onboard.local.ini \
  --migrate-from config/onboard.ini \
  --yes
```

What it does:

1. Reads old `config/onboard.ini` if present.
2. Writes `config/onboard.local.ini` atomically.
3. Backs up the old config with a timestamp.
4. Removes obsolete config keys such as `stepper.microstep`,
   `stepper.step_line`, and `motor*.sense_resistor`.
5. Applies the final TMC2240 pins and RTD Click settings.
6. Builds the onboard binary if `build/` exists and CMake is configured.
7. Installs the systemd service with `config/onboard.local.ini`.
8. Restarts `coatheal-onboard.service`.
9. Prints the last service journal lines if startup fails.

If the build directory is missing:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
python3 scripts/hardware_setup.py plug-and-play \
  --config config/onboard.local.ini \
  --migrate-from config/onboard.ini \
  --yes
```

If you only want migration and pin validation without installing the service:

```bash
python3 scripts/hardware_setup.py migrate-config \
  --config config/onboard.local.ini \
  --migrate-from config/onboard.ini \
  --yes

python3 scripts/hardware_setup.py pin-check \
  --config config/onboard.local.ini
```

## Service Commands

```bash
sudo systemctl daemon-reload
sudo systemctl restart coatheal-onboard.service
systemctl status coatheal-onboard.service --no-pager
journalctl -u coatheal-onboard.service -n 120 --no-pager
```

The installer now prefers `config/onboard.local.ini` when it exists. Confirm:

```bash
cat /etc/coatheal/env
```

Expected:

```text
COATHEAL_CONFIG=/bexus/code/coatheal/config/onboard.local.ini
```

## Hardware Discovery And Health

Basic discovery:

```bash
python3 scripts/hardware_setup.py discover
```

Pin/config check:

```bash
python3 scripts/hardware_setup.py pin-check \
  --config config/onboard.local.ini
```

Live service health:

```bash
python3 scripts/hardware_setup.py doctor \
  --config config/onboard.local.ini
```

Direct command checks:

```bash
python3 - <<'PY'
import socket
for cmd in ("PING", "STATUS", "COMPONENTS", "CHECK RTD_CLICK", "CHECK PWM", "CHECK MOTOR0", "CHECK MOTOR1"):
    with socket.create_connection(("127.0.0.1", 5000), timeout=3) as s:
        s.sendall((cmd + "\n").encode())
        print(cmd, "=>", s.recv(4096).decode(errors="replace").strip())
PY
```

Expected partial-bench behavior:

- `CHECK RTD_CLICK` is `OK` when the RTD Click, PT100, SPI, CS, and DRDY path
  work.
- `COMPONENTS` includes `rtd_click=<state>` and `rtd_click_sample=0`.
- `SENSOR_VALID` shows `S0:1` only after a valid RTD conversion.
- Other sample channels are `nan` and invalid until more sensors are added.
- `DAQ132M` should be `DISABLED` when `sensor.daq132m_enabled=false`.

## RTD Click Wiring And Checks

RTD Click default bench assumptions:

- RTD Click VCC to Pi 3.3 V, not 5 V.
- RTD Click GND to Pi ground.
- SPI MOSI/MISO/SCLK to Pi SPI0.
- RTD Click CS to BCM 7.
- RTD Click DRDY to BCM 25.
- PT100 wiring mode matches `sensor.rtd_click_wires`.
- Default RTD Click jumper setup is normally 3-wire; change config if your
  probe is wired as 2-wire or 4-wire.

Enable Pi SPI:

```bash
sudo raspi-config nonint do_spi 0
sudo reboot
```

Check Linux devices:

```bash
ls -l /dev/spidev0.0 /dev/gpiochip0
gpioinfo gpiochip0 | egrep 'line +7:|line +25:|line +22:|line +23:'
```

Run active RTD check:

```bash
python3 scripts/hardware_setup.py rtd-check
```

Common RTD failures:

| Failure | Likely cause | Action |
|---|---|---|
| `SPI_OPEN_FAILED` | `/dev/spidev0.0` missing | Enable SPI and reboot |
| `SPI_CONFIG_FAILED` | Kernel rejected SPI mode/speed | Lower `sensor.rtd_click_spi_speed_hz` |
| `CS_GPIO_FAILED` | BCM 7 busy or unavailable | Stop conflicting service, inspect `gpioinfo`, confirm final pin map |
| `DATA_READ_FAILED` | No MAX31865 response | Check MOSI/MISO/SCLK/CS wiring and RTD Click power |
| `FAULT_0x..` | MAX31865 fault bit set | Check PT100 wiring, jumper mode, open/short sensor |
| `TEMPERATURE_RANGE` | Converted resistance outside PT100 range | Check `sensor.rtd_click_reference_ohm` and wiring |

## Normal Heater Control

Normal commands require `ARM`:

```bash
python3 - <<'PY'
import socket
for cmd in ("ARM", "SET_TEMP_TARGET 0 35", "SET_PID 0 0.2 0.02 0.03", "GET_THERMAL"):
    with socket.create_connection(("127.0.0.1", 5000), timeout=3) as s:
        s.sendall((cmd + "\n").encode())
        print(s.recv(4096).decode(errors="replace").strip())
PY
```

Open-loop manual duty is still feedback-gated:

```text
SET_HEATER_DUTY 0 0.10
SET_ALL_DUTY 0.05
HEATERS_OFF
```

If the mapped temperature channel is invalid or stale, the thermal controller
forces that heater physically off even when a duty command is accepted.

## Bench Heater Pulse

Use this only with dummy loads or a supervised low-power heater setup. It is
for commissioning GPIO/MOSFET wiring, not normal thermal control.

Requirements:

- `runtime.bench_mode=true`.
- Debug token from `runtime.debug_arm_code`.
- System in `RUN` mode.
- No active motor motion lock.
- Duty and duration within `heater.debug_max_duty` and
  `heater.debug_max_seconds`.

Wrapper command:

```bash
python3 scripts/hardware_setup.py heater-test \
  --heater 0 \
  --duty 0.10 \
  --seconds 2 \
  --debug-token COATHEAL_DEBUG \
  --confirm-load
```

Manual command sequence:

```text
ARM_DEBUG COATHEAL_DEBUG
ARM
HEATER_TEST 0 0.10 2
HEATERS_OFF
DISARM_DEBUG
```

`HEATERS_OFF`, `DISARM`, `ENTER_SAFE`, `DISARM_DEBUG`, and service shutdown
cancel active heater tests.

## Motor Bring-Up

Do not connect the mechanical load for the first electrical test. Use low
current, low speed, and confirm motor direction before any real bend.

Check driver communication:

```bash
python3 scripts/hardware_setup.py doctor \
  --config config/onboard.local.ini
```

Run a supervised low-speed movement:

```bash
python3 scripts/hardware_setup.py motor-test \
  --motor 0 \
  --steps 200 \
  --speed 25 \
  --confirm-motion

python3 scripts/hardware_setup.py motor-test \
  --motor 1 \
  --steps 200 \
  --speed 25 \
  --confirm-motion
```

Manual motor commands:

```text
ARM
CHECK MOTOR0
STEPPER_ENABLE 0
STEPPER_SET_SPEED 0 25
STEPPER_MOVE 0 200
STEPPER_MOVE 0 -200
STEPPER_DISABLE 0
```

Absolute motion and bend sequences require manual software zero after every
restart:

```text
ARM
STEPPER_ENABLE 0
SET_POSITION_ZERO 0
BENDSEQ_LOAD 0 flex 200:1:25 0:1:25
BENDSEQ_RUN 0 flex
BENDSEQ_STATUS 0
```

The final pin map has no limit switches. Software can verify TMC2240
communication and emitted step pulses, but physical movement must be confirmed
visually during bench acceptance.

## Telemetry Freeze Check

The onboard now accepts ground-station `ACK,<session>,0` replies for queued
`EVT,PULL` frames by deleting that exact event frame. DATA frame ACK behavior
remains cumulative by sequence number.

After a motor command, verify telemetry is still updating:

```bash
journalctl -u coatheal-onboard.service -n 120 --no-pager | grep telemetry
```

This error should no longer appear after an event frame:

```text
received mismatched telemetry ACK
```

## Re-Enabling DAQ Later

When replacement Modbus hardware is available, switch only after validating the
register map with read-only diagnostics:

```bash
python3 scripts/hardware_setup.py daq-scan \
  --device auto \
  --baud 9600 \
  --parity N \
  --slave-start 1 \
  --slave-end 10 \
  --function 3 4 \
  --base 0 1 \
  --count 8
```

Then edit `config/onboard.local.ini`:

```ini
sensor.sample_temperature_source=daq132m_modbus
sensor.daq132m_enabled=true
sensor.rtd_click_enabled=false
```

Restart:

```bash
sudo systemctl restart coatheal-onboard.service
```

## Acceptance Checklist

- [ ] `python3 scripts/hardware_setup.py pin-check --config config/onboard.local.ini` prints `pin-check: OK`.
- [ ] `systemctl status coatheal-onboard.service --no-pager` is active.
- [ ] `CHECK RTD_CLICK` returns `overall=OK` with the PT100 connected.
- [ ] Telemetry shows `S0` valid and other sample channels invalid or `nan`.
- [ ] `DAQ132M` is disabled, not blocking telemetry.
- [ ] `CHECK PWM` reports usable heater PWM channels.
- [ ] `heater-test` pulses only the selected dummy-load channel and shuts off.
- [ ] `CHECK MOTOR0` and `CHECK MOTOR1` report TMC2240 SPI readback OK.
- [ ] `motor-test` moves each motor visibly at low speed, out and back.
- [ ] Telemetry continues after motor motion.

