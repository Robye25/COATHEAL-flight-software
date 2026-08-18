# COATHEAL Rev C Component Configuration and Bring-Up

This is the authoritative wiring, configuration, and commissioning procedure
for the Rev C flight stack. Do not energize heaters or motor power until the
logic-only checks in this document pass.

For a complete start-to-finish operator procedure, including safe GPIO changes,
service installation, normal operation, and fault recovery, use
[COATHEAL Rev C Instruction Manual](rev-c-instruction-manual.md).

Sample temperature is acquired by one Sequent Microsystems 8-channel RTD HAT
over I2C, replacing the retired RTD Click (MAX31865/SPI) and DAQ-132M
(RS485/Modbus) paths. Use
[Sequent RTD Bench Bring-Up](sequent-rtd-bring-up.md) for register-map
verification, calibration, and other RTD-specific bench procedures. Config
migration and automated Pi checks remain in
[hardware_setup.py](../scripts/hardware_setup.py).

## 1. Operating Model

- Production uses `config/onboard.local.ini`.
- Missing or failed hardware does not stop onboard telemetry or commands.
- Sensor polling runs independently from the 1 Hz telemetry/control loop.
- A failed sensor retains its last good value with `valid=0` and an age.
- A heater is always off when its mapped PT100 is invalid or stale.
- Motors remain disabled until TMC2240 SPI readback succeeds.
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
4. Join Pi, Sequent RTD HAT, ADS1115, DPS310, MOSFET boards, TMC2240 VIO, and
   regulator signal grounds.
5. Route motor and heater return currents separately from sensor ground wiring.
6. Fit an external pull-down on every active-high `HEAT_EN` line and an
   external pull-up on every active-low TMC2240 `EN` line.
7. Fit heatsinks to both TMC2240 carriers before motor power is applied.
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
| Motor 1 STEP | 18 | 24 | `motor1.step_line` |
| Motor 1 DIR | 38 | 20 | `motor1.dir_line` |
| Motor 1 EN | 40 | 21 | `motor1.enable_line` |

The Sequent RTD HAT is I2C-only and consumes no Pi header GPIO. BCM 16 and 25
(formerly RTD Click CS and DRDY) are freed and deliberately unassigned; see
[Sequent RTD Bench Bring-Up](sequent-rtd-bring-up.md#7-freed-pins).
Status LEDs are disabled because BCM 17 and 27 are heater outputs.

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
ls -l /dev/spidev*
gpioinfo gpiochip0
```

Expected I2C addresses are DPS310 `0x77` or `0x76`, ADS1115 `0x48` through
`0x4B`, and the Sequent RTD HAT at `0x40 + stack` (`0x40` by default). The
DPS310 and ADS1115 boards must use 3.3 V so their I2C pull-ups remain
Pi-safe.

Do not install `dtoverlay=spi0-2cs`. Both TMC2240 carriers share
`/dev/spidev0.0`; the software drives CS on BCM 22 and BCM 23.

## 5. Guided Configuration

Create a local configuration:

```bash
python3 scripts/hardware_setup.py wizard \
  --config config/onboard.local.ini
```

The wizard writes `config/onboard.example.ini` with the final pin map and
commissioning motor current applied, then validates it with
`--check-config` before writing. Edit `sensor.sequent_rtd_*` keys afterward
if the DIP-switch stack or channel wiring differs from the defaults (stack
`0`, card channels `1..8` mapped 1:1 to software samples `S0..S7`).

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

## 6. PT100 Through the Sequent RTD HAT

Connect up to eight XF-931-FAR PT100 probes (3-wire) to the Sequent
Microsystems RTD HAT's card channels 1-8. Set the ID0/ID1/ID2 DIP switches to
the desired stack level (`0` unless stacking with another card) and seat the
HAT on the Pi's 40-pin header; it draws 5 V and communicates over I2C-1, no
SPI or GPIO wiring involved.

Relevant configuration:

```ini
sensor.sequent_rtd_stack=0
sensor.sequent_rtd_channels=1,2,3,4,5,6,7,8
sensor.sequent_rtd_poll_ms=1000
sensor.sequent_rtd_expect_sensor_type=pt100
sensor.sequent_rtd_resistance_min_ohm=60.0
sensor.sequent_rtd_resistance_max_ohm=390.0
sensor.sequent_rtd_crosscheck_tol_c=2.0
sensor.resistance_source=sequent_rtd
```

Check it:

```bash
python3 scripts/hardware_setup.py rtd-check
```

Expected result: `CHECK SEQUENT_RTD` reports `overall=OK` and
`sequent_rtd=OK`, and telemetry shows every wired channel valid. A channel
with no probe connected reads outside the plausibility window and is marked
invalid; that channel's mapped heater stays off.

**Before trusting any reading from this card**, complete the register-map
verification gate in [Sequent RTD Bench Bring-Up](sequent-rtd-bring-up.md#2-register-map-verification-blocking-gate).
The register offsets are derived from vendor source, not measured, and that
document is the authoritative bench procedure for confirming them, along with
burst-read behavior, diagnostic byte interpretation, sensor-type
verification, and bench-only calibration.

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

## 9. TMC2240 and Motors

For each TMC2240 carrier:

1. Select SPI plus STEP/DIR mode according to the exact carrier revision.
2. Connect VIO to 3.3 V and VM to the fused 12 V motor rail.
3. Follow the carrier documentation for CLK and IREF; do not infer jumper
   positions from a different driver family.
4. Connect shared MOSI, MISO, and SCLK; use separate CS lines.
5. Verify motor coil pairs with an ohmmeter. Connect one coil to A1/A2 and the
   other to B1/B2. Never connect/disconnect a motor while VM is powered.
6. Confirm the carrier's IREF/full-scale-current hardware before enabling.
   TMC2240 uses integrated current sensing, not phase sense resistors.

Commissioning configuration:

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
pull.microstep=4
```

The onboard software performs pipelined IOIN readback, requires TMC2240
version `0x40`, verifies configured registers, and reads GSTAT/DRV_STATUS. EN remains
inactive if verification fails.

See [TMC2240 pin configuration and commissioning](tmc2240-pin-configuration-and-commissioning.md)
before applying motor power.

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
| `SEQUENT_RTD FAILED` | I2C enabled, card address (`0x40 + stack`), 5 V power, DIP switch stack level, ribbon/HAT seating, `sequent_rtd_expect_sensor_type` matches the card, `card_type >= 1` |
| `SEQUENT_RTD DEGRADED` | Some channels outside the resistance plausibility window or failing the temperature/resistance cross-check; check wiring on the affected card channels |
| `DPS310 FAILED` | I2C enabled, address `0x76/0x77`, 3.3 V, SDA/SCL |
| `ADS1115 FAILED` | Address `0x48–0x4B`, 3.3 V, SDA/SCL |
| `MOTORn FAILED` | SPI mode, MISO, CS, CLK, VIO, sense resistor, IOIN version |
| Pulses increase but no motion | EN wiring, VM power, coil pairs, current, mechanical jam |
| Heater duty remains zero | Mapped PT100 invalid/stale, overtemperature latch, motion lock |
| No Ethernet telemetry | Ground listener/firewall, link-local addresses, discovery UDP 4100 |

Do not increase motor current or connect heater loads until the corresponding
logic-only and low-power tests pass.
