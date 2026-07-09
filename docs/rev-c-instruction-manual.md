# COATHEAL Rev C Instruction Manual

This manual describes how to install, configure, wire, validate, operate, and
troubleshoot the COATHEAL Rev C onboard software and ground station.

The software is manual-first. It does not start motor movements or heater
profiles automatically while the ground station is connected. If an established
ground link is lost, the onboard software:

- stops manual motor movement that is not part of a running bend sequence;
- permits an already-running bend sequence to continue;
- continues existing temperature targets;
- applies the configured cold-protection floor to heaters without targets;
- continues telemetry, logging, command handling, and hardware recovery.

## 1. Safety Rules

1. Disconnect motor and heater power before changing wiring or GPIO
   configuration.
2. Keep the Raspberry Pi, sensor, DAQ, motor-driver logic, and MOSFET-driver
   signal grounds connected.
3. Do not power motors or heaters from the Raspberry Pi.
4. Do not connect or disconnect a stepper motor while TMC2240 motor voltage is
   present.
5. Use external pull-down resistors on active-high heater-enable inputs.
6. Use external pull-up resistors on active-low TMC2240 enable inputs.
7. Test heater outputs with a meter, LED, or current-limited dummy load before
   connecting polyimide heaters.
8. Test motors unloaded, at `0.8 A RMS`, and at low speed before attaching the
   ball-screw mechanisms.
9. There are no limit switches or encoders. Software zero is not a physical
   home sensor.
10. A heater is inhibited whenever its mapped PT100 reading is invalid or
    stale, including when manual duty control is used.

## 2. System Components

| Function | Component | Software interface |
|---|---|---|
| Onboard controller | Raspberry Pi 4B | Linux, GPIO, I2C, SPI, USB, Ethernet |
| Motor driver 0/1 | TMC2240 carrier | SPI mode 3 plus STEP/DIR/EN |
| Linear actuator 0/1 | NEMA 17 external ball-screw stepper, 2.5 A | TMC2240 |
| Sample temperature | Eight XF-931-FAR PT100 probes | DAQ132M inputs |
| Temperature DAQ | DAQ132M eight-channel card | USB-RS485 Modbus RTU |
| Optional bench RTD | RTD Click MIKROE-2815 | Reserved SPI/DRDY pins |
| UV conversion | Adafruit ADS1115 | I2C |
| UV measurement | GUVA-S12SD breakout | ADS1115 analog input |
| Pressure/ambient T | Adafruit DPS310 | I2C |
| Heater switching | Two EKM014 four-channel boards | Six GPIO PWM inputs |
| Heater load | Polyimide film heaters | External fused heater rail |
| Logic supply | Pololu D24V50F5 | Regulated 5 V |
| Motor supply | Pololu D42V110F12 | Regulated 12 V |
| DAQ adapter | USB-to-RS485 converter | Stable Linux serial path |
| Pi breakout | Pi-EzConnect terminal HAT | Header terminal access |

## 3. Software Locations

The expected Raspberry Pi installation is:

```text
/bexus/code/coatheal
```

Important paths:

| Path | Purpose |
|---|---|
| `config/onboard.example.ini` | Version-controlled flight template |
| `config/onboard.local.ini` | Recommended Pi-specific configuration |
| `config/onboard.debug.ini` | Explicit simulated bench configuration |
| `build/onboard/coatheal_onboard` | Compiled onboard executable |
| `/etc/coatheal/env` | Configuration path selected by systemd |
| `docs/rev-c-instruction-manual.md` | This manual |

Do not make Pi-specific wiring changes directly in
`config/onboard.example.ini`. Copy it to `onboard.local.ini`.

## 4. Raspberry Pi Preparation

Install build and diagnostic packages:

```bash
sudo apt update
sudo apt install -y \
  git cmake g++ pkg-config libgpiod-dev gpiod \
  i2c-tools python3 python3-serial netcat-openbsd
```

Enable I2C and SPI:

```bash
sudo raspi-config nonint do_i2c 0
sudo raspi-config nonint do_spi 0
sudo usermod -aG gpio,spi,i2c,dialout coatheal
sudo reboot
```

After reboot:

```bash
groups
ls -l /dev/gpiochip* /dev/i2c-* /dev/spidev*
gpioinfo gpiochip0
i2cdetect -y 1
```

The `coatheal` account should be a member of `gpio`, `spi`, `i2c`, and
`dialout`.

## 5. Update, Build, and Test

Stop the service before replacing the executable:

```bash
sudo systemctl stop coatheal-onboard.service
cd /bexus/code/coatheal
git switch main
git pull --ff-only origin main
git rev-parse --short HEAD
```

Build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --clean-first --parallel 2
```

Run onboard tests:

```bash
ctest --test-dir build --output-on-failure
```

Validate a configuration without starting hardware control:

```bash
./build/onboard/coatheal_onboard \
  --config config/onboard.example.ini \
  --check-config
```

The expected result is:

```text
Config OK: config/onboard.example.ini
```

An `unknown config key` error after a Git pull normally means the configuration
is newer than the compiled executable. Rebuild the executable before starting
the service.

## 6. Raspberry Pi Header Numbering

Configuration uses **BCM GPIO numbers**, not physical header pin numbers.

Power and ground header pins are not configurable:

| Rail | Physical pins |
|---|---|
| 3.3 V | 1, 17 |
| 5 V | 2, 4 |
| Ground | 6, 9, 14, 20, 25, 30, 34, 39 |

Fixed communication buses:

| Signal | Physical pin | BCM |
|---|---:|---:|
| I2C SDA | 3 | 2 |
| I2C SCL | 5 | 3 |
| SPI MOSI | 19 | 10 |
| SPI MISO | 21 | 9 |
| SPI SCLK | 23 | 11 |

The fixed I2C and SPI pins are selected by the Pi hardware interfaces and are
not reassigned in the INI file.

## 7. Default Configurable Pin Map

| Function | Physical pin | BCM | Configuration key |
|---|---:|---:|---|
| Heater H0 / HEAT_EN1 | 11 | 17 | `heater.output_lines[0]` |
| Heater H1 / HEAT_EN2 | 12 | 18 | `heater.output_lines[1]` |
| Heater H2 / HEAT_EN3 | 13 | 27 | `heater.output_lines[2]` |
| Heater H3 / HEAT_EN4 | 29 | 5 | `heater.output_lines[3]` |
| Heater H4 / HEAT_EN5 | 31 | 6 | `heater.output_lines[4]` |
| Heater H5 / HEAT_EN6 | 33 | 13 | `heater.output_lines[5]` |
| Motor 0 CS | 15 | 22 | `motor0.cs_line` |
| Motor 0 EN | 32 | 12 | `motor0.enable_line` |
| Motor 0 STEP | 35 | 19 | `motor0.step_line` |
| Motor 0 DIR | 37 | 26 | `motor0.dir_line` |
| Motor 1 CS | 16 | 23 | `motor1.cs_line` |
| Motor 1 STEP | 36 | 16 | `motor1.step_line` |
| Motor 1 DIR | 38 | 20 | `motor1.dir_line` |
| Motor 1 EN | 40 | 21 | `motor1.enable_line` |
| Optional RTD Click DRDY | 22 | 25 | `sensor.rtd_click_drdy_line` |
| Optional RTD Click CS | 26 | 7 | `sensor.rtd_click_cs_line` |

The DAQ132M is connected through USB-RS485 and has no configurable Pi GPIO.

## 8. Creating a Local Configuration

```bash
cd /bexus/code/coatheal
cp -n config/onboard.example.ini config/onboard.local.ini
nano config/onboard.local.ini
```

Save in `nano` with `Ctrl+O`, press Enter, then exit with `Ctrl+X`.

The local file must retain:

```ini
manual.manual_first=true
runtime.use_simulated_pwm=false
runtime.use_simulated_sensors=false
```

Simulation is permitted only when explicitly running
`config/onboard.debug.ini`.

## 9. Changing Heater GPIO Pins

The order of `heater.output_lines` defines heater identity:

```ini
heater.output_lines=17,18,27,5,6,13
```

This means:

```text
list position 0 -> H0
list position 1 -> H1
...
list position 5 -> H5
```

Example: to move H0 from BCM 17 to BCM 4:

```ini
heater.output_lines=4,18,27,5,6,13
```

Move the physical H0 wire to physical pin 7, which is BCM 4. Do not place BCM
4 elsewhere in the heater or motor configuration.

The number of entries must equal:

```ini
hardware.heater_count=6
```

Heater polarity and PWM frequency:

```ini
heater.active_high=true
heater.pwm_frequency_hz=10.0
```

Do not change `heater.active_high` unless the electrical input stage has been
verified. An incorrect polarity can energize a heater when zero duty is
requested.

## 10. Changing Heater-to-PT100 Mapping

GPIO assignment and temperature-feedback assignment are separate:

```ini
heater.temperature_channels=0,1,2,3,4,5
```

The value at each list position is the zero-based DAQ sample used by that
heater:

```text
H0 -> S0
H1 -> S1
H2 -> S2
H3 -> S3
H4 -> S4
H5 -> S5
```

Example: if H0 physically heats the sample measured by DAQ channel `S3` and H3
heats `S0`, swap the mappings:

```ini
heater.temperature_channels=3,1,2,0,4,5
```

Every mapped channel must be less than `hardware.sample_count`. Use one unique
PT100 per heater. A heater whose mapped sample is invalid or stale remains
physically off.

## 11. Changing Motor GPIO Pins

Motor 0:

```ini
motor0.cs_line=22
motor0.gpio_chip=/dev/gpiochip0
motor0.step_line=19
motor0.dir_line=26
motor0.enable_line=12
```

Motor 1:

```ini
motor1.cs_line=23
motor1.gpio_chip=/dev/gpiochip0
motor1.step_line=16
motor1.dir_line=20
motor1.enable_line=21
```

Rules:

1. Each CS, STEP, DIR, and EN `(gpio_chip, line)` pair must be unique.
2. A motor GPIO cannot also be used by a heater or enabled LED.
3. Do not use BCM 2/3 or BCM 9/10/11 because they are active bus pins.
4. Avoid BCM 0/1 because HAT identification EEPROMs may use them.
5. Avoid BCM 14/15 unless the serial console and UART use have been checked.
6. Keep `motor*.enable_active_low=true` when the carrier exposes active-low
   enable input.
7. Set `motor*.invert_direction=true` only when software direction must be
   reversed. Do not reverse a motor by changing wires while powered.

Motor-to-sample grouping:

```ini
motor0.samples=0,1,2,3
motor1.samples=4,5,6,7
```

This grouping is telemetry/event metadata. It does not change STEP/DIR wiring.

## 12. GPIO Conflict Validation

Validate every edit:

```bash
./build/onboard/coatheal_onboard \
  --config config/onboard.local.ini \
  --check-config
```

Example conflict:

```text
BCM GPIO 17 assigned to both heater.output_lines[0] and motor0.step_line
```

Do not start the service until the validator prints `Config OK`.

Inspect kernel GPIO ownership:

```bash
gpioinfo gpiochip0
```

If a configured line is already claimed, stop the process or kernel overlay
using it. Do not install the old `spi0-2cs` overlay; COATHEAL uses software CS
on BCM 22 and BCM 23.

## 13. Selecting the Configuration Used by systemd

Install or update the service:

```bash
sudo ./scripts/install_onboard_service.sh \
  /bexus/code/coatheal \
  /bexus/code/coatheal/config/onboard.local.ini
```

Verify:

```bash
cat /etc/coatheal/env
```

Expected:

```text
COATHEAL_CONFIG=/bexus/code/coatheal/config/onboard.local.ini
```

Then:

```bash
sudo systemctl daemon-reload
sudo systemctl reset-failed coatheal-onboard.service
sudo systemctl restart coatheal-onboard.service
sudo systemctl status coatheal-onboard.service --no-pager --full
```

View logs:

```bash
journalctl -u coatheal-onboard.service -n 100 --no-pager
journalctl -fu coatheal-onboard.service
```

## 14. DAQ132M and PT100 Configuration

Physical DAQ channel labels are commonly one-based, while software samples are
zero-based:

| Physical DAQ channel | Software sample |
|---:|---|
| 1 | `S0` |
| 2 | `S1` |
| 3 | `S2` |
| 4 | `S3` |
| 5 | `S4` |
| 6 | `S5` |
| 7 | `S6` |
| 8 | `S7` |

For one PT100 connected to physical channel 2:

```ini
sensor.daq132m_enabled_channels=1
```

For all channels:

```ini
sensor.daq132m_enabled_channels=0,1,2,3,4,5,6,7
```

Use a stable adapter path:

```bash
ls -l /dev/serial/by-id/
```

Configure:

```ini
sensor.daq132m_enabled=true
sensor.daq132m_device=/dev/serial/by-id/<adapter-name>
sensor.daq132m_auto_discover=true
sensor.daq132m_baud=9600
sensor.daq132m_parity=N
sensor.daq132m_data_bits=8
sensor.daq132m_stop_bits=1
sensor.daq132m_slave_id=1
sensor.daq132m_function_code=3
sensor.daq132m_register_base=0
sensor.daq132m_register_count=8
sensor.daq132m_c_per_count=0.1
sensor.daq132m_c_offset=0.0
```

Stop the service before scanning:

```bash
sudo systemctl stop coatheal-onboard.service
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

The scan utility performs only Modbus function 03/04 reads. It does not write
DAQ registers.

`RS485_OK` proves that a correctly addressed frame with a valid CRC was
received. It does not prove that every PT100 channel is connected.

## 15. I2C Sensor Configuration

Expected devices:

```text
ADS1115: 0x48, 0x49, 0x4A, or 0x4B
DPS310:  0x76 or 0x77
```

Detect:

```bash
i2cdetect -y 1
```

Configure:

```ini
sensor.dps310_enabled=true
sensor.dps310_i2c_addr=0x77
sensor.dps310_auto_discover=true

sensor.ads1115_enabled=true
sensor.ads1115_i2c_addr=0x48
sensor.ads1115_auto_discover=true
sensor.uv_ads1115_channel=0
sensor.uv_full_scale_v=4.096
```

Connect GUVA-S12SD analog output to the selected ADS1115 analog channel. Do not
connect the analog output directly to a Pi digital GPIO.

Both Qwiic/STEMMA boards must operate with Pi-safe 3.3 V I2C pull-ups.

## 16. TMC2240 Configuration

Both drivers share:

```text
SPI MOSI: BCM 10
SPI MISO: BCM 9
SPI SCLK: BCM 11
```

Each driver has separate CS, STEP, DIR, and EN lines.

Commissioning values:

```ini
motor0.run_current_a_rms=0.8
motor1.run_current_a_rms=0.8
motor0.current_range_a_peak=0
motor1.current_range_a_peak=0
motor0.pulse_high_us=3
motor1.pulse_high_us=3
motor0.retry_ms=2000
motor1.retry_ms=2000
stepper.enable_on_boot=false
```

The TMC2240 uses integrated current sensing. Verify the carrier's IREF and
full-scale-current hardware before changing current. The 2.5 A motor nameplate
is not a command to exceed the TMC2240's thermal/current limits.

Non-motion driver communication and configuration check:

```bash
printf 'CHECK MOTOR0\n' | nc 127.0.0.1 5000
printf 'CHECK MOTOR1\n' | nc 127.0.0.1 5000
```

Low-speed out-and-back test:

```bash
python3 scripts/hardware_setup.py motor-test \
  --motor 0 --steps 200 --speed 25 --confirm-motion

python3 scripts/hardware_setup.py motor-test \
  --motor 1 --steps 200 --speed 25 --confirm-motion
```

The software verifies TMC2240 version `0x40`, SPI register readback, and emitted pulses. The
operator must visually verify physical movement because there are no encoders.

## 17. Heater Commissioning

Before attaching a heater:

1. Confirm the mapped PT100 is reporting a plausible temperature.
2. Confirm `COMPONENTS` reports the DAQ and required PWM channel healthy.
3. Connect a current-limited dummy load.
4. Start at a low duty.

Commands:

```bash
printf 'ARM\n' | nc 127.0.0.1 5000
printf 'SET_HEATER_DUTY 0 0.05\n' | nc 127.0.0.1 5000
printf 'GET_THERMAL\n' | nc 127.0.0.1 5000
printf 'HEATERS_OFF\n' | nc 127.0.0.1 5000
```

Closed-loop control:

```bash
printf 'SET_PID 0 0.20 0.02 0.03\n' | nc 127.0.0.1 5000
printf 'SET_TEMP_TARGET 0 25.0\n' | nc 127.0.0.1 5000
printf 'GET_THERMAL\n' | nc 127.0.0.1 5000
```

Always tune PID gains with conservative temperature targets and supervised
loads.

## 18. Runtime Health Commands

```bash
printf 'PING\n' | nc 127.0.0.1 5000
printf 'STATUS\n' | nc 127.0.0.1 5000
printf 'COMPONENTS\n' | nc 127.0.0.1 5000
printf 'CHECK ALL\n' | nc 127.0.0.1 5000
```

Targeted checks:

```bash
printf 'CHECK DPS310\n' | nc 127.0.0.1 5000
printf 'CHECK ADS1115\n' | nc 127.0.0.1 5000
printf 'CHECK DAQ132M\n' | nc 127.0.0.1 5000
printf 'CHECK PWM\n' | nc 127.0.0.1 5000
printf 'CHECK MOTOR0\n' | nc 127.0.0.1 5000
printf 'CHECK MOTOR1\n' | nc 127.0.0.1 5000
printf 'CHECK STORAGE\n' | nc 127.0.0.1 5000
printf 'CHECK COMMS\n' | nc 127.0.0.1 5000
```

Component states:

| State | Meaning |
|---|---|
| `DISABLED` | Disabled in configuration |
| `DISCOVERING` | No first result yet |
| `OK` | Current valid communication/data |
| `DEGRADED` | Partial data or recent failure with usable subsystem state |
| `STALE` | Last success is older than `sensor.stale_after_ms` |
| `FAILED` | No usable communication or backend |

Telemetry continues when components fail. Never-valid sensor values are `nan`.
After a failure, the last good value is retained with validity false and an
increasing age.

## 19. Manual Motor Operation

Enable and jog:

```bash
printf 'ARM\n' | nc 127.0.0.1 5000
printf 'STEPPER_ENABLE 0\n' | nc 127.0.0.1 5000
printf 'STEPPER_SET_SPEED 0 25\n' | nc 127.0.0.1 5000
printf 'STEPPER_MOVE 0 100\n' | nc 127.0.0.1 5000
```

After positioning at a known physical reference:

```bash
printf 'SET_POSITION_ZERO 0\n' | nc 127.0.0.1 5000
```

Absolute moves are then allowed:

```bash
printf 'STEPPER_MOVETO 0 800\n' | nc 127.0.0.1 5000
printf 'STEPPER_HOME 0\n' | nc 127.0.0.1 5000
```

Stop and disable:

```bash
printf 'STEPPER_STOP 0\n' | nc 127.0.0.1 5000
printf 'STEPPER_DISABLE 0\n' | nc 127.0.0.1 5000
```

Repeat with motor ID `1` for the second actuator.

## 20. Bend Sequences

Each step is:

```text
<absolute_target_microsteps>:<hold_seconds>[:<speed_hz>]
```

Example:

```bash
printf 'STEPPER_ENABLE 0\n' | nc 127.0.0.1 5000
printf 'SET_POSITION_ZERO 0\n' | nc 127.0.0.1 5000
printf 'BENDSEQ_LOAD 0 flex 800:2:25 1600:3:25 0:1:25\n' \
  | nc 127.0.0.1 5000
printf 'BENDSEQ_RUN 0 flex\n' | nc 127.0.0.1 5000
printf 'BENDSEQ_STATUS 0\n' | nc 127.0.0.1 5000
```

Control:

```bash
printf 'BENDSEQ_PAUSE 0\n' | nc 127.0.0.1 5000
printf 'BENDSEQ_RESUME 0\n' | nc 127.0.0.1 5000
printf 'BENDSEQ_STOP 0\n' | nc 127.0.0.1 5000
printf 'BENDSEQ_CLEAR 0 flex\n' | nc 127.0.0.1 5000
```

Only one motor moves at a time. All heater outputs are inhibited while the
motion lock is active.

## 21. Ground Station Installation and Startup

From Windows PowerShell:

```powershell
cd D:\COATHEAL-flight-software\COATHEAL-flight-software\ground-station
py -3 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
pip install -r requirements.txt
python gui_app.py
```

The default connection workflow:

1. listens for telemetry on TCP port `4000`;
2. sends discovery on UDP port `4100`;
3. probes the Pi at `169.254.10.10:5000`;
4. uses the responding Pi as the command target.

Explicit mode:

```powershell
python gui_app.py --host 169.254.10.10 --tel-port 4000 --cmd-port 5000
```

## 22. Fault Recovery

### Service reports unknown configuration key

Cause: the INI file is newer than the executable.

```bash
sudo systemctl stop coatheal-onboard.service
cd /bexus/code/coatheal
git pull --ff-only origin main
cmake --build build --clean-first --parallel 2
./build/onboard/coatheal_onboard \
  --config config/onboard.local.ini --check-config
sudo systemctl restart coatheal-onboard.service
```

### Service preflight permission denied

```bash
chmod +x scripts/*.sh
namei -l /bexus/code/coatheal/scripts/preflight_healthcheck.sh
sudo systemctl daemon-reload
sudo systemctl restart coatheal-onboard.service
```

### RS485 failure

Check:

```bash
ls -l /dev/serial/by-id/ /dev/ttyUSB* 2>/dev/null
groups
printf 'CHECK DAQ132M\n' | nc 127.0.0.1 5000
```

Then verify DAQ power, A/B polarity, ground/reference, baud, parity, slave ID,
function code, register base, register count, scale, and PT100 channel mode.

### I2C sensor failure

```bash
i2cdetect -y 1
printf 'CHECK DPS310\n' | nc 127.0.0.1 5000
printf 'CHECK ADS1115\n' | nc 127.0.0.1 5000
```

Verify 3.3 V power, common ground, SDA/SCL continuity, and address jumpers.

### Motor reports failed

Check SPI and GPIO:

```bash
ls -l /dev/spidev0.0
gpioinfo gpiochip0
printf 'CHECK MOTOR0\n' | nc 127.0.0.1 5000
```

Verify VIO, VM, ground, CS, MISO, MOSI, SCLK, EN, IREF, and board
mode jumpers. EN remains inactive if TMC2240 readback fails.

### Pulses increase but motor does not move

Verify motor voltage, enable polarity, coil pairs, driver current, mechanical
freedom, and STEP/DIR wiring. Software cannot prove physical movement without
an encoder.

### Heater duty remains zero

Check:

- mapped PT100 validity and age;
- overtemperature latch;
- motor motion lock;
- system mode is `RUN`;
- PWM channel state;
- energy budget latch.

Use:

```bash
printf 'GET_THERMAL\n' | nc 127.0.0.1 5000
printf 'COMPONENTS\n' | nc 127.0.0.1 5000
```

### Ethernet is connected but telemetry is absent

On the Pi:

```bash
ip address show
ss -lntup | grep -E ':(4000|5000|4100)'
printf 'PING\n' | nc 127.0.0.1 5000
```

Confirm the Pi has `169.254.10.10/16`, the ground station is listening on TCP
4000, and Windows Firewall permits TCP 4000/5000 and UDP 4100.

## 23. Emergency Stop

From the ground station or Pi command port:

```bash
printf 'HEATERS_OFF\n' | nc 127.0.0.1 5000
printf 'BENDSEQ_STOP 0\n' | nc 127.0.0.1 5000
printf 'BENDSEQ_STOP 1\n' | nc 127.0.0.1 5000
printf 'STEPPER_STOP 0\n' | nc 127.0.0.1 5000
printf 'STEPPER_STOP 1\n' | nc 127.0.0.1 5000
printf 'DISARM\n' | nc 127.0.0.1 5000
```

For immediate electrical isolation, remove heater and motor power using the
external fused power disconnect. Do not rely only on software for emergency
power isolation.

## 24. Configuration Change Checklist

Before every powered run:

- [ ] Correct Git commit is checked out.
- [ ] Onboard executable rebuilt after source/config updates.
- [ ] `--check-config` reports `Config OK`.
- [ ] `/etc/coatheal/env` selects `onboard.local.ini`.
- [ ] BCM numbers match physical wiring.
- [ ] No duplicate GPIO assignments.
- [ ] I2C devices appear at expected addresses.
- [ ] Stable USB-RS485 path is configured.
- [ ] PT100 physical channels match software sample indices.
- [ ] Heater-to-PT100 mappings are verified.
- [ ] TMC2240 carrier IREF/full-scale configuration and commissioning current are verified.
- [ ] Motor and heater power are initially disconnected.
- [ ] `COMPONENTS` and targeted `CHECK` results are reviewed.
- [ ] Both motors pass supervised low-speed tests.
- [ ] Every heater output passes a dummy-load test.
- [ ] Ground-station telemetry remains continuous with sensors disconnected.
- [ ] Link-loss behavior is tested before flight.

Related references:

- [Component Configuration and Bring-Up](component-configuration-and-bring-up.md)
- [TMC2240 Pin Configuration and Commissioning](tmc2240-pin-configuration-and-commissioning.md)
- [Configuration Reference](configuration.md)
- [Wire Protocol](protocol.md)
- [Manual Operations](manual-operations.md)
- [Hardware Reference](hardware.md)
