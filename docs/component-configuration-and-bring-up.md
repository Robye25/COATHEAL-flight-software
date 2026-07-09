# COATHEAL Rev C Component Configuration and Bring-Up

This is the authoritative wiring, configuration, and commissioning procedure
for the Rev C flight stack. Do not energize heaters or motor power until the
logic-only checks in this document pass.

## 1. Operating Model

- Production uses `config/onboard.local.ini`.
- Missing or failed hardware does not stop onboard telemetry or commands.
- Sensor polling runs independently from the 1 Hz telemetry/control loop.
- A failed sensor retains its last good value with `valid=0` and an age.
- A heater is always off when its mapped PT100 is invalid or stale.
- Motors remain disabled until TMC5160 SPI readback succeeds.
- With no limit switches, relative jog is allowed before zeroing. Absolute
  moves and bend sequences require `SET_POSITION_ZERO`.
- Simulation is only enabled by `config/onboard.debug.ini`.

## 2. Power and Grounding

1. Use the Pololu D24V50F5 for the regulated 5 V logic rail and the
   D42V110F12 for the regulated 12 V motor rail.
2. Verify both regulator output voltages with a multimeter before connecting
   the Pi or any sensor.
3. Power the Pi from one 5 V source only. Do not simultaneously back-power its
   5 V header and USB-C input.
4. Join Pi, DAQ, USB-RS485, ADS1115, DPS310, MOSFET boards, TMC5160 VIO, and
   regulator signal grounds.
5. Route motor and heater return currents separately from sensor ground wiring.
6. Fit an external pull-down on every active-high `HEAT_EN` line and an
   external pull-up on every active-low TMC5160 `EN` line.
7. Fit heatsinks to both TMC5160 boards before motor power is applied.
8. Power each polyimide heater from a fused, current-limited rail matching its
   rated voltage. The component list does not define a universal heater
   voltage; do not assume 12 V without checking the heater label/datasheet.

## 3. Raspberry Pi Pin Map

Configuration uses BCM GPIO numbers, not physical header numbers.

| Function | Physical pin | BCM | Configuration |
|---|---:|---:|---|
| I2C SDA | 3 | 2 | fixed I2C-1 |
| I2C SCL | 5 | 3 | fixed I2C-1 |
| Heater H0 | 11 | 17 | `heater.output_lines[0]` |
| Heater H1 | 12 | 18 | `heater.output_lines[1]` |
| Heater H2 | 13 | 27 | `heater.output_lines[2]` |
| Heater H3 | 29 | 5 | `heater.output_lines[3]` |
| Heater H4 | 31 | 6 | `heater.output_lines[4]` |
| Heater H5 | 33 | 13 | `heater.output_lines[5]` |
| SPI MOSI | 19 | 10 | SPI0 |
| SPI MISO | 21 | 9 | SPI0 |
| SPI SCLK | 23 | 11 | SPI0 |
| Motor 0 CS | 15 | 22 | `motor0.cs_line` |
| Motor 0 EN | 32 | 12 | `motor0.enable_line` |
| Motor 0 STEP | 35 | 19 | `motor0.step_line` |
| Motor 0 DIR | 37 | 26 | `motor0.dir_line` |
| Motor 1 CS | 16 | 23 | `motor1.cs_line` |
| Motor 1 STEP | 36 | 16 | `motor1.step_line` |
| Motor 1 DIR | 38 | 20 | `motor1.dir_line` |
| Motor 1 EN | 40 | 21 | `motor1.enable_line` |
| Optional RTD Click DRDY | 22 | 25 | disabled |
| Optional RTD Click CS | 26 | 7 | disabled |

The DAQ132M uses USB and consumes no Pi header GPIO. Status LEDs are disabled
because BCM 17 and 27 are heater outputs.

## 4. Pi Interface Setup

```bash
sudo raspi-config nonint do_i2c 0
sudo raspi-config nonint do_spi 0
sudo usermod -aG gpio,spi,i2c,dialout coatheal
sudo reboot
```

After reboot:

```bash
cd /bexus/code/coatheal
python3 scripts/hardware_setup.py discover
i2cdetect -y 1
ls -l /dev/spidev* /dev/serial/by-id/* /dev/ttyUSB* 2>/dev/null
gpioinfo gpiochip0
```

Expected I2C addresses are DPS310 `0x77` or `0x76` and ADS1115 `0x48` through
`0x4B`. Both boards must use 3.3 V so their I2C pull-ups remain Pi-safe.

Do not install `dtoverlay=spi0-2cs`. Both TMC5160 boards share
`/dev/spidev0.0`; the software drives CS on BCM 22 and BCM 23.

## 5. Guided Configuration

Create a local configuration:

```bash
python3 scripts/hardware_setup.py wizard \
  --config config/onboard.local.ini
```

For the current bench setup, where only physical DAQ channel 2 is connected,
enter:

```text
1
```

Software indices are zero-based, so physical DAQ channel 2 is `S1`.

Validate without touching hardware:

```bash
./build/onboard/coatheal_onboard \
  --config config/onboard.local.ini --check-config
```

The service must use the local configuration:

```bash
sudo ./scripts/install_onboard_service.sh \
  /bexus/code/coatheal \
  /bexus/code/coatheal/config/onboard.local.ini
```

## 6. PT100 and DAQ132M

Connect each XF-931-FAR PT100 to the matching DAQ input using the DAQ
manufacturer's two-wire or three-wire terminal arrangement. Do not infer the
terminal order from wire color alone. Configure each used DAQ channel as
PT100, not thermocouple or PT1000.

Connect the powered DAQ RS485 `A/+` and `B/-` terminals to the USB-RS485
adapter. Join signal ground if required by the adapter/DAQ manual. If every
request times out, power down and verify A/B polarity; naming conventions
differ between manufacturers.

Relevant configuration:

```ini
sensor.daq132m_enabled=true
sensor.daq132m_device=/dev/serial/by-id/<adapter>
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
sensor.daq132m_enabled_channels=0,1,2,3,4,5,6,7
```

The DAQ register map is not confirmed. Stop the service before scanning:

```bash
sudo systemctl stop coatheal-onboard
python3 scripts/hardware_setup.py daq-scan \
  --device auto --baud 9600 --parity N \
  --slave-start 1 --slave-end 10 \
  --function 3 4 --base 0 1 --count 8
sudo systemctl start coatheal-onboard
```

The scanner only sends Modbus read functions 03/04. It never writes DAQ
configuration. Copy the successful slave, function, base, count, scale, and
offset into `onboard.local.ini`.

`RS485_OK` means a frame with matching address/function/length and CRC was
received. `SAMPLE_TEMP_OK` means at least one enabled channel is valid.
An enabled channel is invalid only when the returned value is non-finite or
outside the configured PT100 safety range. Confirm the DAQ's documented
open-probe sentinel before relying on automatic disconnected-probe detection.

## 7. DPS310, ADS1115, and GUVA-S12SD

Connect the DPS310 and ADS1115 through I2C-1. The software tries the configured
address first and then the safe supported address set.

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

Wire GUVA-S12SD analog output to ADS1115 A0, plus 3.3 V and ground. Do not
connect the GUVA analog output directly to a digital Pi GPIO.

## 8. Heater Outputs

Two four-channel EKM014 boards provide six used channels. Connect H0..H5 in
the pin-table order. The final mapping is:

```ini
heater.output_lines=17,18,27,5,6,13
heater.temperature_channels=0,1,2,3,4,5
heater.active_high=true
heater.pwm_frequency_hz=10.0
```

Test each output with an LED or meter before attaching heaters. A missing GPIO
disables only that heater channel. Any invalid mapped PT100 forces duty to
zero, including manual-duty commands.

## 9. FYSETC TMC5160 and Motors

For each standard StepStick:

1. Select SPI plus STEP/DIR mode according to the exact FYSETC board revision.
2. Connect VIO to 3.3 V and VM to the fused 12 V motor rail.
3. Connect CLK to ground if the board requires this for its internal clock.
4. Connect shared MOSI, MISO, and SCLK; use separate CS lines.
5. Verify motor coil pairs with an ohmmeter. Connect one coil to A1/A2 and the
   other to B1/B2. Never connect/disconnect a motor while VM is powered.
6. Confirm the board's sense resistor before enabling. The supplied standard
   profile uses `0.075 Ω`.

Commissioning configuration:

```ini
motor0.run_current_a_rms=0.8
motor1.run_current_a_rms=0.8
motor0.sense_resistor_ohm=0.075
motor1.sense_resistor_ohm=0.075
motor0.pulse_high_us=3
motor1.pulse_high_us=3
motor0.retry_ms=2000
motor1.retry_ms=2000
stepper.enable_on_boot=false
pull.microstep=4
```

The onboard software performs pipelined IOIN readback, verifies the TMC5160
version and configured registers, and reads GSTAT/DRV_STATUS. EN remains
inactive if verification fails.

With the mechanism unloaded and clear:

```bash
python3 scripts/hardware_setup.py motor-test \
  --motor 0 --steps 200 --speed 25 --confirm-motion
python3 scripts/hardware_setup.py motor-test \
  --motor 1 --steps 200 --speed 25 --confirm-motion
```

Each test moves out and back. Software can prove SPI communication and emitted
pulses; without limit switches or encoders, the operator must visually verify
physical movement and direction.

Normal manual commissioning:

```bash
printf 'COMPONENTS\n' | nc 127.0.0.1 5000
printf 'CHECK MOTOR0\n' | nc 127.0.0.1 5000
printf 'ARM\n' | nc 127.0.0.1 5000
printf 'STEPPER_ENABLE 0\n' | nc 127.0.0.1 5000
printf 'STEPPER_SET_SPEED 0 25\n' | nc 127.0.0.1 5000
printf 'STEPPER_MOVE 0 200\n' | nc 127.0.0.1 5000
printf 'SET_POSITION_ZERO 0\n' | nc 127.0.0.1 5000
```

## 10. Runtime Verification

```bash
sudo systemctl restart coatheal-onboard
sudo systemctl status coatheal-onboard --no-pager --full
printf 'COMPONENTS\n' | nc 127.0.0.1 5000
printf 'CHECK ALL\n' | nc 127.0.0.1 5000
printf 'STATUS\n' | nc 127.0.0.1 5000
```

Telemetry must continue even if `CHECK ALL` reports failures. Expected
component states are `DISABLED`, `DISCOVERING`, `OK`, `DEGRADED`, `STALE`, or
`FAILED`.

## 11. Fault Guide

| Symptom | Check |
|---|---|
| Service exit status 126 | Script mode and parent-directory permissions |
| `DAQ132M FAILED` | DAQ power, serial path, `dialout`, A/B, baud, slave, function, registers |
| `DAQ132M DEGRADED` | No enabled channel is valid, or only some enabled channels return in-range values |
| `DPS310 FAILED` | I2C enabled, address `0x76/0x77`, 3.3 V, SDA/SCL |
| `ADS1115 FAILED` | Address `0x48–0x4B`, 3.3 V, SDA/SCL |
| `MOTORn FAILED` | SPI mode, MISO, CS, CLK, VIO, sense resistor, IOIN version |
| Pulses increase but no motion | EN wiring, VM power, coil pairs, current, mechanical jam |
| Heater duty remains zero | Mapped PT100 invalid/stale, overtemperature latch, motion lock |
| No Ethernet telemetry | Ground listener/firewall, link-local addresses, discovery UDP 4100 |

Do not increase motor current or connect heater loads until the corresponding
logic-only and low-power tests pass.
