# Hardware Reference (Rev C Final BOM)

This document is the active hardware reference for the final component list.
Use it with [sequent-rtd-bring-up.md](sequent-rtd-bring-up.md) and
`config/onboard.local.ini`.

The bench commissioning procedure for the RTD HAT is
[sequent-rtd-bring-up.md](sequent-rtd-bring-up.md).

The software is manual-first. Hardware outputs are commanded by the operator
while the ground link is healthy; pressure/thermal fallback is only used after
link loss.

## Final Component List

| Subsystem | Final component | Interface | Config keys |
|---|---|---|---|
| Stepper drivers | TMC2240 carriers | SPI mode 3 + STEP/DIR/EN GPIO | `motor0.*`, `motor1.*`, `pull.*` |
| Linear actuators | NEMA 17 external ball-screw linear stepper, 2.5 A, 48 mm | STEP/DIR/EN through TMC2240 | `stepper.*`, `motor*.samples` |
| Sample temperature | XF-931-FAR PT100 probes | Sequent Microsystems 8-channel RTD HAT, I2C `0x40 + stack` | `sensor.sequent_rtd_*` |
| UV | GUVA-S12SD analog UV sensor | Analog into ADS1115 | `sensor.uv_*` |
| ADC | Adafruit ADS1115 16-bit 4-channel PGA | I2C, STEMMA QT/Qwiic | `sensor.ads1115_i2c_addr` |
| Pressure / ambient T | Adafruit DPS310 precision pressure/altitude sensor | I2C, STEMMA QT/Qwiic | `sensor.dps310_i2c_addr` |
| Heater switching | Electrokit EKM014 UCC27524 4-channel MOSFET driver board | GPIO PWM inputs | `heater.output_lines` |
| Heaters | Polyimide film heaters | MOSFET-switched heater rail | `hardware.heater_count=6` |
| Logic rail | Pololu D24V50F5 5 V / 5 A regulator | 5 V DC | `power.logic_regulator_v=5.0` |
| Stepper rail | Pololu D42V110F12 12 V / 9 A regulator | 12 V DC | `power.stepper_regulator_v=12.0` |
| Wiring breakout | Pi-EzConnect Terminal Block Breakout HAT | Pass-through GPIO | BCM numbering |

There is no separate resistance instrument in the final BOM. The Sequent RTD
card reads PT100 element resistance alongside temperature on every channel,
and by default (`sensor.resistance_source=sequent_rtd`) that per-channel
resistance is what serializes on the telemetry `RESISTANCE=` field.
`disabled` and `simulated` remain available for compatibility testing;
`disabled` emits `-` placeholders.

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
| Motor 1 / STEP2 STEP | 18 | 24 | `motor1.step_line` |
| Motor 1 / STEP2 DIR | 38 | 20 | `motor1.dir_line` |
| Motor 1 / STEP2 EN | 40 | 21 | `motor1.enable_line` |
| I2C SDA / SCL | 3 / 5 | 2 / 3 | Fixed Pi I2C-1 |
| SPI0 MOSI / MISO / SCLK | 19 / 21 / 23 | 10 / 9 / 11 | Shared SPI0 bus |
| Sequent RTD HAT | n/a | I2C `0x40 + stack` | `sensor.sequent_rtd_stack` |

BCM 16 and BCM 25 (formerly RTD Click CS and DRDY) are freed and deliberately
unassigned; see [sequent-rtd-bring-up.md](sequent-rtd-bring-up.md#7-freed-pins).

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
8. SPI0 now serves only the TMC2240 backend, on `/dev/spidev0.0` with
   software-controlled CS on BCM 22/23. `pin-check` must pass before the
   service is started.

The ADS1115, DPS310, and Sequent RTD HAT share I2C-1 at addresses `0x48`,
`0x77`, and `0x40 + stack` respectively. Power the ADS1115 and DPS310 STEMMA
QT boards from 3.3 V so their I2C pull-ups cannot raise SDA or SCL above the
Pi's 3.3 V GPIO domain. The Sequent RTD HAT is a 5 V board powered from the
Pi's 5 V rail; its I2C signaling remains 3.3 V-safe.

## Sensors

### Sample Temperature: Sequent RTD HAT

All eight sample channels are populated by one Sequent Microsystems 8-channel
RTD HAT: eight XF-931-FAR PT100 probes wired into card channels 1-8, read over
I2C at `0x40 + stack`. See
[Sequent RTD Bench Bring-Up](sequent-rtd-bring-up.md) for wiring, register-map
verification, and calibration.

Relevant config:

```ini
hardware.sample_count=8
sensor.sequent_rtd_stack=0
sensor.sequent_rtd_channels=1,2,3,4,5,6,7,8
sensor.sequent_rtd_poll_ms=1000
sensor.sequent_rtd_expect_sensor_type=pt100
sensor.sequent_rtd_resistance_min_ohm=60.0
sensor.sequent_rtd_resistance_max_ohm=390.0
sensor.sequent_rtd_crosscheck_tol_c=2.0
sensor.resistance_source=sequent_rtd
```

`CHECK SEQUENT_RTD` actively reads the card's identity and temperature/
resistance registers (`DAQ132M`/`RTD_CLICK` are still accepted as legacy
aliases on the wire). Normal heater control is allowed only for heaters whose
mapped sample channel is valid and fresh. A channel is valid only when its
temperature and resistance are both finite, resistance falls inside the
configured plausibility window, and the card's reported temperature agrees
with the resistance-derived temperature within `sequent_rtd_crosscheck_tol_c`.

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
| DPS310 / ADS1115 I2C | Linux `i2c-dev` reads implemented; validate addresses and calibration on the assembled bus |
| Sequent RTD HAT I2C | Active read backend implemented; register map derived from vendor source and gated on bench verification, see [sequent-rtd-bring-up.md](sequent-rtd-bring-up.md) |

## Bring-Up Commands

```bash
sudo raspi-config nonint do_i2c 0
sudo raspi-config nonint do_spi 0
sudo reboot

i2cdetect -y 1
ls -l /dev/spidev*
gpioinfo gpiochip0
python3 scripts/hardware_setup.py plug-and-play \
  --config config/onboard.local.ini \
  --migrate-from config/onboard.ini \
  --yes
```

Expected I2C devices:

```text
0x40  Sequent RTD HAT, stack 0 (0x40 + stack)
0x48  ADS1115
0x77  DPS310, unless address jumper changes it
```

Expected SPI device:

```text
/dev/spidev0.0  shared bus for both TMC2240 drivers
```

After the onboard command server starts, run active checks:

```bash
python3 scripts/hardware_setup.py doctor --config config/onboard.local.ini
python3 scripts/hardware_setup.py rtd-check
```

Expected current temperature state:

```text
SEQUENT_RTD: OK with PT100 probes connected
```
