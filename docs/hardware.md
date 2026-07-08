# Hardware Reference (Rev C Final BOM)

This document is the active hardware reference for the final component list.
Use it with [rev-c-installation-and-hardware-setup.md](rev-c-installation-and-hardware-setup.md)
and `config/onboard.example.ini`.

The software is manual-first. Hardware outputs are commanded by the operator
while the ground link is healthy; pressure/thermal fallback is only used after
link loss.

## Final Component List

| Subsystem | Final component | Interface | Config keys |
|---|---|---|---|
| Stepper drivers | FYSETC TMC5160 | SPI + STEP/DIR/EN GPIO | `motor0.*`, `motor1.*`, `pull.*` |
| Linear actuators | NEMA 17 external ball-screw linear stepper, 2.5 A, 48 mm | STEP/DIR/EN through TMC5160 | `stepper.*`, `motor*.samples` |
| Sample temperature | XF-931-FAR PT100 probes | PT100 into DAQ132M | `hardware.sample_count=8` |
| PT100 acquisition | DAQ132M 8-channel thermocouple/PT100 RS485 Modbus card | USB-RS485, Modbus RTU | `sensor.daq132m_*` |
| Optional PT100 bench path | RTD Click MIKROE-2815 / MAX31865 | SPI + DRDY GPIO | `sensor.rtd_click_*` |
| UV | GUVA-S12SD analog UV sensor | Analog into ADS1115 | `sensor.uv_*` |
| ADC | Adafruit ADS1115 16-bit 4-channel PGA | I2C, STEMMA QT/Qwiic | `sensor.ads1115_i2c_addr` |
| Pressure / ambient T | Adafruit DPS310 precision pressure/altitude sensor | I2C, STEMMA QT/Qwiic | `sensor.dps310_i2c_addr` |
| Heater switching | Electrokit EKM014 UCC27524 4-channel MOSFET driver board | GPIO PWM inputs | `heater.output_lines` |
| Heaters | Polyimide film heaters | MOSFET-switched heater rail | `hardware.heater_count=6` |
| Logic rail | Pololu D24V50F5 5 V / 5 A regulator | 5 V DC | `power.logic_regulator_v=5.0` |
| Stepper rail | Pololu D42V110F12 12 V / 9 A regulator | 12 V DC | `power.stepper_regulator_v=12.0` |
| RS485 adapter | USB-to-RS485 converter | `/dev/ttyUSB0` default | `sensor.daq132m_device` |
| Wiring breakout | Pi-EzConnect Terminal Block Breakout HAT | Pass-through GPIO | BCM numbering |

There is no separate resistance instrument in the final BOM. The telemetry
`RESISTANCE=` field remains for protocol compatibility and emits `-` values
when `sensor.resistance_source=disabled`.

## Default Pin Map

All GPIO numbers are BCM line numbers on `/dev/gpiochip0`.

| Function | BCM line | Config key | Notes |
|---|---:|---|---|
| Heartbeat LED | 17 | `hal.status_led_line` | Optional operator heartbeat |
| Mode LED | 27 | `hal.mode_led_line` | Optional mode indication |
| Motor 0 STEP | 5 | `motor0.step_line` | TMC5160 STEP |
| Motor 0 DIR | 6 | `motor0.dir_line` | TMC5160 DIR |
| Motor 0 EN | 13 | `motor0.enable_line` | Active-low by default |
| Motor 0 CS | 8 | `motor0.cs_line` | SPI0 CE0, `/dev/spidev0.0` |
| Motor 1 STEP | 19 | `motor1.step_line` | TMC5160 STEP |
| Motor 1 DIR | 26 | `motor1.dir_line` | TMC5160 DIR |
| Motor 1 EN | 16 | `motor1.enable_line` | Active-low by default |
| Motor 1 CS | 7 | `motor1.cs_line` | SPI0 CE1, `/dev/spidev0.1` |
| Heater 0 | 12 | `heater.output_lines[0]` | EKM014 input |
| Heater 1 | 20 | `heater.output_lines[1]` | EKM014 input |
| Heater 2 | 21 | `heater.output_lines[2]` | EKM014 input |
| Heater 3 | 23 | `heater.output_lines[3]` | EKM014 input |
| Heater 4 | 24 | `heater.output_lines[4]` | EKM014 input |
| Heater 5 | 25 | `heater.output_lines[5]` | EKM014 input |
| I2C SDA/SCL | 2 / 3 | Pi I2C bus | DPS310 + ADS1115 |
| SPI0 MOSI/MISO/SCLK | 10 / 9 / 11 | Pi SPI0 bus | TMC5160 drivers; RTD Click only if enabled |
| RTD Click DRDY | 22 | `sensor.rtd_click_drdy_line` | Optional bench/backup path |
| DAQ132M RS485 | USB | `sensor.daq132m_device` | Default `/dev/ttyUSB0` |

The pin map is configurable. If wiring changes, update `config/onboard.local.ini`
and keep this table in sync.

## Sensors

### XF-931-FAR PT100 + DAQ132M

The eight XF-931-FAR PT100 probes are the primary sample-temperature source.
They connect to the DAQ132M 8-channel card, which is read over Modbus RTU via
the USB-to-RS485 converter.

Relevant config:

```ini
hardware.sample_count=8
sensor.sample_temperature_source=daq132m_modbus
sensor.daq132m_device=/dev/ttyUSB0
sensor.daq132m_baud=9600
sensor.daq132m_parity=N
sensor.daq132m_data_bits=8
sensor.daq132m_stop_bits=1
sensor.daq132m_slave_id=1
sensor.daq132m_register_base=0
sensor.daq132m_register_count=8
sensor.daq132m_c_per_count=0.1
```

The Modbus register base and scaling must be verified against the exact DAQ132M
manual before flight. The software now stores and validates these settings, but
the real Modbus read backend still needs bench validation against the card.

### RTD Click MIKROE-2815

The RTD Click is a MAX31865 single-channel SPI RTD board. It is not the primary
eight-sample path because it handles one RTD channel. It is supported as a
configurable bench or backup path.

```ini
sensor.rtd_click_enabled=false
sensor.rtd_click_spi_device=/dev/spidev0.0
sensor.rtd_click_cs_line=18
sensor.rtd_click_drdy_line=22
sensor.rtd_click_wires=3
```

Enable it only when the SPI chip-select wiring does not conflict with the two
TMC5160 drivers.

### DPS310

The DPS310 provides ambient pressure and sensor temperature over I2C. The
default I2C address is configured as `0x77`; confirm with `i2cdetect -y 1`.

```ini
sensor.pressure_source=dps310
sensor.dps310_i2c_addr=0x77
```

### GUVA-S12SD + ADS1115

The GUVA-S12SD analog output is wired into the ADS1115. The ADS1115 address
range is selectable by board wiring; the default is `0x48`.

```ini
sensor.uv_source=guva_s12sd_ads1115
sensor.ads1115_i2c_addr=0x48
sensor.uv_ads1115_channel=0
sensor.uv_full_scale_v=4.096
```

## Motion System

Both motors use FYSETC TMC5160 drivers. The software programs the TMC5160 over
SPI, then uses STEP/DIR/EN GPIO for motion. Only one motor can run a pull cycle
at a time because `MotionLock` serializes pull commands; heaters are forced to
zero while the lock is held.

```ini
stepper.steps_per_rev=200
stepper.microstep=4
stepper.default_step_hz=100.0
stepper.max_step_hz=100.0
stepper.max_position_steps=200000

pull.max_step_hz=100.0
pull.accel_steps_per_s2=200.0
pull.microstep=4
pull.travel_full_steps=200
pull.hold_s=5.0
```

Motor 0 controls samples 0-3. Motor 1 controls samples 4-7.

## Heater System

Six polyimide film heaters are switched through Electrokit EKM014 UCC27524
4-channel MOSFET driver boards. Two boards are expected when using six heater
channels.

```ini
hardware.heater_count=6
heater.output_lines=12,20,21,23,24,25
heater.pwm_frequency_hz=10.0
heater.active_high=true
power.max_active_heaters=4
power.max_thermal_w=20.0
power.heater_nominal_w=5.0
```

`HEATERS_OFF` and `MotionLock` must be bench-tested with dummy loads before
connecting flight heaters.

## HAL Status

| Area | Status |
|---|---|
| Command protocol, manual-first state, telemetry queue | Implemented |
| TMC5160 SPI setup | Implemented as a conservative register-write pass; current scaling must be bench-verified |
| STEP/DIR/EN GPIO pulse backend | Still integration-stubbed; final GPIO waveform timing needs Pi bench validation |
| Heater channel mapping | Parsed and passed into `LibgpiodPwmController`; real PWM waveform backend still needs Pi validation |
| DAQ132M Modbus | Configured and validated; real Modbus reads still pending |
| DPS310 / ADS1115 I2C | Configured and validated; real I2C reads still pending |
| RTD Click MAX31865 | Configured as optional; real SPI reads still pending |

## Bring-Up Commands

```bash
sudo raspi-config nonint do_i2c 0
sudo raspi-config nonint do_spi 0
sudo reboot

i2cdetect -y 1
ls -l /dev/spidev*
ls -l /dev/ttyUSB*
gpioinfo gpiochip0
```

Expected I2C devices:

```text
0x48  ADS1115
0x77  DPS310, unless address jumper changes it
```

Expected SPI devices:

```text
/dev/spidev0.0  motor0 TMC5160
/dev/spidev0.1  motor1 TMC5160
```

Expected RS485:

```text
/dev/ttyUSB0  DAQ132M through USB-RS485 converter
```
