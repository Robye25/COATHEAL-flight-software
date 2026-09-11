# Hardware Reference (Schematic v4 Final BOM)

This document is the active hardware reference for the final component list
(electrical schematic v4, 2026-08-29). v4 kept the v3 pin map and topology
unchanged; it fixed the heater rail at 14.4 V (see the component table) and
fits no pull resistors on the heater or motor-driver control lines (see
[Boot-time GPIO states](#boot-time-gpio-states)).
Use it with [sequent-rtd-bring-up.md](sequent-rtd-bring-up.md),
[tmc5160-commissioning.md](tmc5160-commissioning.md), and
`config/onboard.local.ini`.

The bench commissioning procedures are
[sequent-rtd-bring-up.md](sequent-rtd-bring-up.md) (sample temperature and
the MAX31865 sample-resistance clicks) and
[tmc5160-commissioning.md](tmc5160-commissioning.md) (steppers and the SPI0
topology they share with the clicks).

The software is manual-first. Hardware outputs are commanded by the operator
while the ground link is healthy; pressure/thermal fallback is only used after
link loss.

## Final Component List

| Subsystem | Final component | Interface | Config keys |
|---|---|---|---|
| Stepper drivers | TMC5160 carriers (QHV5160 v2) | SPI mode 3, SPI-only position dribble — no STEP/DIR | `motor0.*`, `motor1.*`, `pull.*` |
| Linear actuators | NEMA 17 external ball-screw linear stepper, 2.5 A, 48 mm | Driven by TMC5160 over SPI | `stepper.*`, `motor*.samples` |
| Sample temperature | XF-931-FAR PT100 probes | Sequent Microsystems 8-channel RTD HAT, I2C `0x40 + stack` | `sensor.sequent_rtd_*` |
| Sample resistance | 2x MikroE RTD Click (MAX31865), 4-wire Kelvin per specimen | SPI0 native CS (CE0/CE1) | `sensor.max31865_*` |
| UV | GUVA-S12SD analog UV sensor | Analog into ADS1115 | `sensor.uv_*` |
| ADC | Adafruit ADS1115 16-bit 4-channel PGA | I2C, STEMMA QT/Qwiic | `sensor.ads1115_i2c_addr` |
| Pressure / ambient T | Adafruit DPS310 precision pressure/altitude sensor | I2C, STEMMA QT/Qwiic | `sensor.dps310_i2c_addr` |
| Heater switching | Electrokit EKM014 UCC27524 4-channel MOSFET driver board | GPIO PWM inputs | `heater.output_lines` |
| Heaters | Polyimide film heaters | MOSFET-switched heater rail | `hardware.heater_count=6` |
| Logic rail | Pololu D24V50F5 5 V / 5 A regulator | 5 V DC | `power.logic_regulator_v=5.0` |
| Stepper rail | Pololu D42V110F12 12 V / 9 A regulator | 12 V DC | `power.stepper_regulator_v=12.0` |
| Wiring breakout | Pi-EzConnect Terminal Block Breakout HAT | Pass-through GPIO | BCM numbering |

Schematic v3 adds a dedicated sample-resistance instrument: two MAX31865
clicks measuring the coating specimens' own resistance directly, distinct
from the Sequent RTD card's PT100 *element* resistance. By default
(`sensor.resistance_source=max31865_click`) the click resistance is what
serializes on the telemetry `RESISTANCE=` field, in the two
`sensor.max31865_sample_indices` slots only. `sequent_rtd` (PT100 element
resistance, all eight slots), `disabled` (`-` placeholders), and `simulated`
remain available.

## Final Pin Map

All GPIO numbers are BCM line numbers on `/dev/gpiochip0`. This table matches
the plan's Global Constraints v3 GPIO table exactly — treat any mismatch
elsewhere as the error, not this one.

| Function | BCM line | Config key |
|---|---:|---|
| Heater H1 | 19 | `heater.output_lines[0]` |
| Heater H2 | 13 | `heater.output_lines[1]` |
| Heater H3 | 6 | `heater.output_lines[2]` |
| Heater H4 | 5 | `heater.output_lines[3]` |
| Heater H5 | 24 | `heater.output_lines[4]` |
| Heater H6 | 23 | `heater.output_lines[5]` |
| Motor 0 / STEP1 CS (soft) | 22 | `motor0.cs_line` |
| Motor 0 / STEP1 EN | 20 | `motor0.enable_line` |
| Motor 1 / STEP2 CS (soft) | 27 | `motor1.cs_line` |
| Motor 1 / STEP2 EN | 21 | `motor1.enable_line` |
| I2C SDA / SCL | 2 / 3 | Fixed Pi I2C-1 |
| SPI0 MOSI / MISO / SCLK | 10 / 9 / 11 | Fixed SPI0, not configurable |
| SPI0 CE1 (hard) — MAX31865 click 1 / SAMPLE1, `/dev/spidev0.1` | 7 | reserved: `spi0_ce1` |
| SPI0 CE0 (hard) — MAX31865 click 2 / SAMPLE2, `/dev/spidev0.0` | 8 | reserved: `spi0_ce0` |
| Sequent RTD HAT UART TX | 14 | reserved: `sequent_hat uart_tx` |
| Sequent RTD HAT UART RX | 15 | reserved: `sequent_hat uart_rx` |
| Sequent RTD HAT RS485 DIR | 17 | reserved: `sequent_hat rs485_dir` |
| Sequent RTD HAT INTN | 26 | reserved: `sequent_hat intn` |
| Sequent RTD HAT (card itself) | n/a | I2C `0x40 + stack`, `sensor.sequent_rtd_stack` |

**No `step_line`/`dir_line` GPIO exists anywhere in this map — motion is
SPI-only.** BCM 14, 15, 17, and 26 are claimed by the Sequent HAT's
UART/RS485/INTN nets (present, reserved, unused by flight software — see the
spec's out-of-scope note); BCM 7 and 8 are claimed by the clicks' native
SPI0 chip-selects. The config-load GPIO validator rejects any heater or
motor line landing on any of these six reserved BCM numbers, naming the
reserved owner in the error. See
[TMC5160 Commissioning §4](tmc5160-commissioning.md#4-spi-topology--four-devices-one-bus)
for the full four-device SPI0 topology, including why the motors' CS is
software (BCM 22/27) while the clicks' CS is the kernel's native hardware CS.

BCM 16 and BCM 25 (formerly RTD Click CS and DRDY, from the pre-v3 retired
temperature path) are freed and deliberately unassigned; see
[sequent-rtd-bring-up.md](sequent-rtd-bring-up.md#7-freed-pins).

The diagram has no status LEDs, so `hal.status_led_enabled` and
`hal.mode_led_enabled` are both `false`.

## Wiring Sanity Requirements

Do not energize heaters or motors until these points are verified:

1. Use two EKM014 boards for six heaters. Each EKM014 has four channels.
2. Power each EKM014 driver supply within its specified 4.5-12 V range. Its
   control inputs accept Pi 3.3 V logic; do not power heater loads from the Pi.
3. Add an external pull-down to every `HEAT_EN` input so all heaters remain off
   while the Pi boots, reboots, or has its GPIO lines unclaimed. Schematic v4
   fits none, so the [boot-time GPIO states](#boot-time-gpio-states) below
   are what covers that window.
4. Add an external pull-up to each active-low TMC5160 `EN` input so both motors
   remain disabled during boot (also not fitted on v4; same mitigation). Power
   TMC5160 `VIO` from 3.3 V and the motor stage from the separate 12 V rail.
5. Tie Pi, EKM014, TMC5160, sensor, and regulator signal grounds together.
   Route heater and motor return current separately from sensor ground wiring.
6. Power the Pi from one controlled 5 V source. If the 5 V header is used,
   verify polarity and regulation before connecting it and do not also inject
   power from another source.
7. The final diagram provides no limit switches. Software position is unknown
   after reboot, so travel must remain mechanically constrained and low-speed
   commissioning must establish safe step limits before any full pull.
8. SPI0 carries four devices in v3: the two TMC5160 drivers on
   `/dev/spidev0.0` with software-controlled CS on BCM 22/27
   (`SPI_NO_CS` — mandatory, not optional, because CE0/CE1 are wired to the
   clicks), and the two MAX31865 clicks on native hardware CS
   (`/dev/spidev0.1`=CE1=click 1/SAMPLE1, `/dev/spidev0.0`=CE0=click
   2/SAMPLE2). `pin-check` must pass before the service is started.

The ADS1115, DPS310, and Sequent RTD HAT share I2C-1 at addresses `0x48`,
`0x77`, and `0x40 + stack` respectively. Power the ADS1115 and DPS310 STEMMA
QT boards from 3.3 V so their I2C pull-ups cannot raise SDA or SCL above the
Pi's 3.3 V GPIO domain. The Sequent RTD HAT is a 5 V board powered from the
Pi's 5 V rail; its I2C signaling remains 3.3 V-safe.

## Boot-time GPIO states

Schematic v4 fits no pull resistors on the EKM014 heater inputs or on the
TMC5160 chip-select and enable lines, so nothing on the board defines those
levels while no software drives them. The BCM2711 powers on with pull-UP on
BCM 0-8 and pull-DOWN on BCM 9-27, which for the final pin map means, from
power-on until `coatheal-onboard` claims its lines:

| Lines | BCM | Power-on pull | Consequence |
|---|---|---|---|
| Heaters H3, H4 | 6, 5 | up | driver inputs high: two heaters ON |
| Heaters H1, H2, H5, H6 | 19, 13, 24, 23 | down | off |
| TMC5160 CS0, CS1 | 22, 27 | down | both drivers selected: every SPI0 datagram (the MAX31865 clicks share the bus) is latched by both TMC5160s as a register write |
| TMC5160 EN0, EN1 | 20, 21 | down | both drivers enabled (active-low `EN`) |

Two software layers close that window; `deploy_onboard.sh` installs both.

1. **Firmware boot to service start: a managed `gpio=` block in
   `config.txt`.** `scripts/hardware_setup.py boot-gpio` derives it from the
   same INI the service runs on (`heater.output_lines`/`heater.active_high`,
   `motorN.cs_line`, `motorN.enable_line`/`enable_active_low`), so the two
   can never disagree, and `deploy_onboard.sh` writes it between managed
   marker comments in `/boot/firmware/config.txt` (idempotent; a changed
   block needs a reboot, and the deploy says so). For the flight pin map:

   ```
   gpio=5,6,13,19,23,24=op,dl,pd
   gpio=20,21,22,27=op,dh,pu
   ```

   `op,dl,pd` = output, driven low, pulled down; `op,dh,pu` = output, driven
   high, pulled up. Print the block without installing it with
   `python3 scripts/hardware_setup.py boot-gpio --config config/onboard.local.ini`.
2. **Service running, stopped, or restarting: a pull bias on every claimed
   line.** `RequestGpioOutput` (`hal/gpio_output.hpp`, `GpioBias`) requests
   each heater line with a pull toward OFF and each CS/EN line with a pull
   toward deselected/disabled. The SoC's pull register survives the line
   being released (the pinctrl driver only reverts the function to input),
   so after a service stop or crash the line rests at its safe level until
   the next power cycle. A kernel or libgpiod without bias support still
   gets the output, without the pull, and logs a warning.

Independently of both, the service starts its MAX31865 workers only after
each TMC5160 driver has claimed its chip-select line, so click traffic can no
longer be clocked into a still-selected driver during start-up.

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
```

`CHECK SEQUENT_RTD` actively reads the card's identity and temperature/
resistance registers (`DAQ132M`/`RTD_CLICK` are still accepted as legacy
aliases on the wire, for the *retired temperature path* only — unrelated to
the `MAX31865` selector below). Normal heater control is allowed only for
heaters whose mapped sample channel is valid and fresh. A channel is valid
only when its temperature and resistance are both finite, resistance falls
inside the configured plausibility window, and the card's reported
temperature agrees with the resistance-derived temperature within
`sequent_rtd_crosscheck_tol_c`.

### Sample Resistance: MAX31865 Dual-Click

Schematic v3 adds two MikroE RTD Click boards (MAX31865), each wired 4-wire
Kelvin directly to one coating specimen — a physically different measurement
from the Sequent RTD HAT's PT100 *element* resistance above. They sit on
SPI0's native hardware chip-selects (not the motors' software CS): CE1
(BCM 07) is click 1 / SAMPLE1 on `/dev/spidev0.1`; CE0 (BCM 08) is click 2 /
SAMPLE2 on `/dev/spidev0.0`. Device paths are fixed by the CE crossover, not
configurable. See
[Sequent RTD Bench Bring-Up §9-10](sequent-rtd-bring-up.md#9-max31865-sample-resistance-click-bring-up-blocking-gates)
for the bench gates (reference resistor value, coating resistance range
characterisation, and the sample-index mapping placeholder) and
[TMC5160 Commissioning §4](tmc5160-commissioning.md#4-spi-topology--four-devices-one-bus)
for the full SPI0 topology.

```ini
sensor.max31865_reference_ohm=470.0
sensor.max31865_poll_ms=1000
sensor.max31865_sample_indices=0,4
sensor.resistance_source=max31865_click
```

An out-of-range (saturated) specimen reading is a first-class, valid
measurement outcome — the adapter reports the channel invalid rather than a
plausible-looking number, and the click's bus health stays OK. There is
deliberately no plausibility window for coating resistance, unlike the
RTD HAT's PT100 window above: the expected range is unknown by design, and
the instrument's job is to measure and characterise it, not assume it.

`CHECK MAX31865` performs a real one-shot conversion on each click (not a
cached-health read) and reports `max31865_1`/`max31865_2` OK/FAIL plus their
error strings.

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

**Both motors use TMC5160 carriers, driven entirely over SPI — there is no
STEP/DIR wiring, and none is ever expected on this schematic.** Every
commanded microstep becomes one SPI write nudging the chip's internal ramp
generator (`XTARGET`) rather than a GPIO pulse; the ramp generator turns that
into the coil drive waveform in hardware. `MotionLock` serializes all motion,
including jogs and sequences, exactly as before — the pulse-level contract
above the driver (pacing thread, position tracking, pull cycles) is
unchanged by this swap. Heater GPIOs are forced low before motion begins and
remain inhibited while the lock is held. See
[TMC5160 Commissioning](tmc5160-commissioning.md) for the full driver
contract, SPI topology, current model, and bench gates.

```ini
stepper.steps_per_rev=200
stepper.default_step_hz=100.0
stepper.max_position_steps=200000

pull.max_step_hz=100.0
pull.accel_steps_per_s2=200.0
pull.microstep=4
pull.travel_full_steps=200
pull.hold_s=5.0

motor0.driver=tmc5160
motor0.cs_line=22
motor0.enable_line=20
motor0.sense_resistor_ohm=0.075

motor1.driver=tmc5160
motor1.cs_line=27
motor1.enable_line=21
motor1.sense_resistor_ohm=0.075
```

Motor 0 controls samples 0-3. Motor 1 controls samples 4-7. Current is set
from `run_current_a_rms` and `sense_resistor_ohm` (GLOBALSCALER/IRUN, no
TMC2240-style fixed peak-current range) — see
[TMC5160 Commissioning §6](tmc5160-commissioning.md#6-current-model-globalscaler--irun-two-regimes).

## Heater System

Six polyimide film heaters are switched through Electrokit EKM014 UCC27524
4-channel MOSFET driver boards. Two boards are expected when using six heater
channels.

```ini
hardware.heater_count=6
heater.output_lines=19,13,6,5,24,23
heater.pwm_frequency_hz=1.0
heater.active_high=true
power.max_active_heaters=3
power.max_thermal_w=15.0
power.heater_nominal_w=5.0
```

1 Hz software PWM (not 10 Hz) is the v3 owner-confirmed rate — these are 5 W
film heaters with high thermal inertia and no hardware PWM channel is wired
for this set. The 3-heater/15 W ceiling is an owner power-budget rule
(`power.max_active_heaters`/`max_thermal_w`), enforced by `HeaterScheduler`
regardless of how many channels request nonzero duty — see
[TMC5160 Commissioning §8, gate 7](tmc5160-commissioning.md#gate-7--max-3-heaters-ceiling-under-load)
for the bench observation of this ceiling under load.

`HEATERS_OFF` and `MotionLock` must be bench-tested with dummy loads before
connecting flight heaters.

## HAL Status

| Area | Status |
|---|---|
| Command protocol, manual-first state, telemetry queue | Implemented |
| TMC5160 SPI-only motion (position dribble) | GPIO software chip-select + register writes implemented; current model bench-verified against an assumed sense resistor, see [tmc5160-commissioning.md](tmc5160-commissioning.md) |
| Heater PWM | Implemented as a zero-safe libgpiod software PWM thread at 1 Hz; validate with dummy loads |
| DPS310 / ADS1115 I2C | Linux `i2c-dev` reads implemented; validate addresses and calibration on the assembled bus |
| Sequent RTD HAT I2C | Active read backend implemented; register map derived from vendor source and gated on bench verification, see [sequent-rtd-bring-up.md](sequent-rtd-bring-up.md) |
| MAX31865 dual-click SPI | Active one-shot read backend implemented (native CE, mode 1); reference resistor and coating-resistance range gated on bench verification, see [sequent-rtd-bring-up.md §9-10](sequent-rtd-bring-up.md#9-max31865-sample-resistance-click-bring-up-blocking-gates) |

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

Expected SPI devices:

```text
/dev/spidev0.0  CE0 native (MAX31865 click 2/SAMPLE2); also opened SPI_NO_CS
                by both TMC5160 drivers for their software-CS traffic
/dev/spidev0.1  CE1 native (MAX31865 click 1/SAMPLE1)
```

After the onboard command server starts, run active checks:

```bash
python3 scripts/hardware_setup.py doctor --config config/onboard.local.ini
python3 scripts/hardware_setup.py rtd-check
printf 'CHECK MAX31865\n' | nc 127.0.0.1 5000
printf 'CHECK MOTOR0\n' | nc 127.0.0.1 5000
printf 'CHECK MOTOR1\n' | nc 127.0.0.1 5000
```

Expected current temperature state:

```text
SEQUENT_RTD: OK with PT100 probes connected
```
