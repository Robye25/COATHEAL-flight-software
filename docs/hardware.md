# Hardware Reference (Rev C Final BOM)

This document is the active hardware reference for the final component list.
Use it with [rev-c-installation-and-hardware-setup.md](rev-c-installation-and-hardware-setup.md)
and `config/onboard.example.ini`.

The authoritative connection and commissioning procedure is
[component-configuration-and-bring-up.md](component-configuration-and-bring-up.md).

The software is manual-first. Hardware outputs are commanded by the operator
while the ground link is healthy; pressure/thermal fallback is only used after
link loss.

## Final Component List

| Subsystem | Final component | Interface | Config keys |
|---|---|---|---|
| Stepper drivers | TMC2240 carriers | SPI mode 3 + STEP/DIR/EN GPIO | `motor0.*`, `motor1.*`, `pull.*` |
| Linear actuators | NEMA 17 external ball-screw linear stepper, 2.5 A, 48 mm | STEP/DIR/EN through TMC2240 | `stepper.*`, `motor*.samples` |
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

## Final Pin Map

All GPIO numbers are BCM line numbers on `/dev/gpiochip0`. Physical header
numbers are included to prevent BCM/physical-number confusion.

| Function | Physical pin | BCM line | Config key |
|---|---:|---:|---|
| Heater 0 / HEAT_EN1 | 11 | 17 | `heater.output_lines[0]` |
| Heater 1 / HEAT_EN2 | 12 | 18 | `heater.output_lines[1]` |
| Heater 2 / HEAT_EN3 | 13 | 27 | `heater.output_lines[2]` |
| Heater 3 / HEAT_EN4 | 29 | 5 | `heater.output_lines[3]` |
| Heater 4 / HEAT_EN5 | 31 | 6 | `heater.output_lines[4]` |
| Heater 5 / HEAT_EN6 | 33 | 13 | `heater.output_lines[5]` |
| Motor 0 / STEP1 CS | 15 | 22 | `motor0.cs_line` |
| Motor 0 / STEP1 EN | 32 | 12 | `motor0.enable_line` |
| Motor 0 / STEP1 STEP | 35 | 19 | `motor0.step_line` |
| Motor 0 / STEP1 DIR | 37 | 26 | `motor0.dir_line` |
| Motor 1 / STEP2 CS | 16 | 23 | `motor1.cs_line` |
| Motor 1 / STEP2 STEP | 36 | 16 | `motor1.step_line` |
| Motor 1 / STEP2 DIR | 38 | 20 | `motor1.dir_line` |
| Motor 1 / STEP2 EN | 40 | 21 | `motor1.enable_line` |
| RTD Click CS | 26 | 7 | `sensor.rtd_click_cs_line` |
| RTD Click DRDY | 22 | 25 | `sensor.rtd_click_drdy_line` |
| I2C SDA / SCL | 3 / 5 | 2 / 3 | Fixed Pi I2C-1 |
| SPI0 MOSI / MISO / SCLK | 19 / 21 / 23 | 10 / 9 / 11 | Shared SPI0 bus |
| DAQ132M RS485 | USB | n/a | `sensor.daq132m_device` |

The diagram has no status LEDs, so `hal.status_led_enabled` and
`hal.mode_led_enabled` are both `false`.

## Wiring Sanity Requirements

Do not energize heaters or motors until these points are verified:

1. Use two EKM014 boards for six heaters. Each EKM014 has four channels.
2. Power each EKM014 driver supply within its specified 4.5-12 V range. Its
   control inputs accept Pi 3.3 V logic; do not power heater loads from the Pi.
3. Add an external pull-down to every `HEAT_EN` input so all heaters remain off
   while the Pi boots, reboots, or has its GPIO lines unclaimed.
4. Add an external pull-up to each active-low TMC2240 `EN` input so both motors
   remain disabled during boot. Power TMC2240 `VIO` from 3.3 V and the motor
   stage from the separate 12 V rail.
5. Tie Pi, EKM014, TMC2240, sensor, and regulator signal grounds together.
   Route heater and motor return current separately from sensor ground wiring.
6. Power the Pi from one controlled 5 V source. If the 5 V header is used,
   verify polarity and regulation before connecting it and do not also inject
   power from another source.
7. The final diagram provides no limit switches. Software position is unknown
   after reboot, so travel must remain mechanically constrained and low-speed
   commissioning must establish safe step limits before any full pull.
8. SPI0 has three possible targets. The TMC2240 backend uses `/dev/spidev0.0`
   with kernel CS disabled and software-controlled CS on BCM 22/23. Do not
   install the old `spi0-2cs` overlay. RTD Click on BCM 7 remains an optional
   bench path and must stay disabled until its MAX31865 read path is validated.

The ADS1115 and DPS310 may share I2C-1 at default addresses `0x48` and `0x77`.
Power both STEMMA QT boards from 3.3 V so their I2C pull-ups cannot raise SDA or
SCL above the Pi's 3.3 V GPIO domain. RTD Click MIKROE-2815 is also a 3.3 V
board.

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
sensor.daq132m_function_code=3
sensor.daq132m_register_base=0
sensor.daq132m_register_count=8
sensor.daq132m_c_per_count=0.1
sensor.daq132m_c_offset=0.0
```

`RS485_OK` indicates that a valid Modbus frame and CRC were received.
Disconnected DAQ inputs do not fail the bus; `SAMPLE_TEMP_OK` requires at
least one valid channel, and thermal control inhibits each invalid channel
independently.

The Modbus RTU read, CRC validation, range checks, function code, register base,
scale, and offset are implemented. The exact register map and engineering-unit
conversion must still be verified against the supplied DAQ132M manual.

### RTD Click MIKROE-2815

The RTD Click is a MAX31865 single-channel SPI RTD board. It is not the primary
eight-sample path because it handles one RTD channel. Its pins and configuration
are reserved, but the active Rev C sensor manager does not read it; keep
`sensor.rtd_click_enabled=false`.

```ini
sensor.rtd_click_enabled=false
sensor.rtd_click_spi_device=/dev/spidev0.0
sensor.rtd_click_cs_line=7
sensor.rtd_click_drdy_line=25
sensor.rtd_click_wires=3
```

Do not enable it for flight until a dedicated MAX31865 backend and bench test
are added.

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

Both motors use TMC2240 carriers. The software programs them over SPI mode 3
using configurable chip-selects, then uses configurable STEP/DIR/EN GPIO for motion.
`MotionLock` serializes all motion, including jogs and sequences. Heater GPIOs
are forced low before motion begins and remain inhibited while the lock is held.

```ini
stepper.steps_per_rev=200
stepper.default_step_hz=100.0
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
heater.output_lines=17,18,27,5,6,13
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
| TMC2240 SPI setup | GPIO chip-select and register writes implemented; integrated current scaling must be bench-verified |
| STEP/DIR/EN GPIO pulse backend | Implemented with libgpiod; waveform timing needs Pi bench validation |
| Heater PWM | Implemented as a zero-safe libgpiod software PWM thread; validate with dummy loads |
| DAQ132M Modbus | RTU read/CRC/range handling implemented; register map and scaling need card validation |
| DPS310 / ADS1115 I2C | Linux `i2c-dev` reads implemented; validate addresses and calibration on the assembled bus |
| RTD Click MAX31865 | Pins/config reserved; active read backend not implemented |

## Bring-Up Commands

```bash
sudo raspi-config nonint do_i2c 0
sudo raspi-config nonint do_spi 0
sudo reboot

i2cdetect -y 1
ls -l /dev/spidev*
ls -l /dev/ttyUSB*
gpioinfo gpiochip0
./build/onboard/coatheal_onboard --config config/onboard.example.ini
```

Expected I2C devices:

```text
0x48  ADS1115
0x77  DPS310, unless address jumper changes it
```

Expected SPI device:

```text
/dev/spidev0.0  shared bus for both TMC2240 drivers
```

After the onboard command server starts, run `CHECK`. It actively probes
DPS310, ADS1115, DAQ132M, storage, PWM/stepper backends, and TMC2240 SPI:

```powershell
python main.py command --cmd CHECK
```

Expected RS485:

```text
/dev/ttyUSB0  DAQ132M through USB-RS485 converter
```
