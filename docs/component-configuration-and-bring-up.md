# COATHEAL Rev C Component Configuration and Bring-Up

This is the authoritative wiring, configuration, and commissioning procedure
for the schematic v3 flight stack. Do not energize heaters or motor power
until the logic-only checks in this document pass.

For a complete start-to-finish operator procedure, including safe GPIO changes,
service installation, normal operation, and fault recovery, use
[COATHEAL Rev C Instruction Manual](rev-c-instruction-manual.md).

Sample temperature is acquired by one Sequent Microsystems 8-channel RTD HAT
over I2C, replacing the retired RTD Click (MAX31865/SPI) and DAQ-132M
(RS485/Modbus) paths **for temperature**. Use
[Sequent RTD Bench Bring-Up](sequent-rtd-bring-up.md) for register-map
verification, calibration, and other RTD-specific bench procedures — that
same document also carries the bring-up gates for the v3 MAX31865 dual-click
**sample-resistance** instrument (section 9), an unrelated device from the
retired RTD Click. Stepper bring-up (TMC5160, SPI-only) is
[TMC5160 Commissioning](tmc5160-commissioning.md). Config migration and
automated Pi checks remain in
[hardware_setup.py](../scripts/hardware_setup.py).

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
4. Join Pi, Sequent RTD HAT, ADS1115, DPS310, MOSFET boards, TMC5160 VIO, and
   regulator signal grounds.
5. Route motor and heater return currents separately from sensor ground wiring.
6. Fit an external pull-down on every active-high `HEAT_EN` line and an
   external pull-up on every active-low TMC5160 `EN` line.
7. Fit heatsinks to both TMC5160 carriers before motor power is applied.
8. Power each polyimide heater from a fused, current-limited rail matching its
   rated voltage. The component list does not define a universal heater
   voltage; do not assume 14.4 V without checking the heater label/datasheet.

## 3. Raspberry Pi Pin Map

Configuration uses BCM GPIO numbers, not physical header numbers. This is
the v3 GPIO map — see [hardware.md](hardware.md#final-pin-map) for the
authoritative full table (fixed buses, SPI0 chip-selects, and reserved
lines). The lines this guide configures directly, heaters and motors, are:

| Function | BCM | Configuration |
|---|---:|---|
| Heater H1 | 19 | `motor0.specimens` entry 1 (`ch1:19`) |
| Heater H2 | 13 | `motor0.specimens` entry 2 (`ch2:13`) |
| Heater H3 | 6 | `motor0.specimens` entry 3 (`ch3:6`) |
| Heater H4 | 5 | `motor0.specimens` entry 4 (`ch4:5`) |
| Heater H5 | 24 | `motor1.specimens` entry 1 (`ch5:24`) |
| Heater H6 | 23 | `motor1.specimens` entry 2 (`ch6:23`) |
| Motor 0 CS (soft) | 22 | `motor0.cs_line` |
| Motor 0 EN | 20 | `motor0.enable_line` |
| Motor 1 CS (soft) | 27 | `motor1.cs_line` |
| Motor 1 EN | 21 | `motor1.enable_line` |

(SPI0 CE0/CE1 for the MAX31865 clicks are covered in §6b below.)

**There is no STEP or DIR GPIO — the TMC5160 motors are SPI-only.** The
Sequent RTD HAT itself is I2C-only and consumes no Pi header GPIO beyond the
four reserved lines in [hardware.md](hardware.md#final-pin-map) (present on
the stacked HAT, unused by flight software). BCM 16 and 25 (formerly RTD Click CS and DRDY, from the retired
pre-v3 temperature path) are freed and deliberately unassigned; see
[Sequent RTD Bench Bring-Up](sequent-rtd-bring-up.md#7-freed-pins).
Status LEDs are disabled; their default line numbers (17, 27) now belong to
the Sequent HAT's RS485_DIR and motor 1's chip-select respectively.

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

Do not install `dtoverlay=spi0-2cs`. Both TMC5160 carriers share
`/dev/spidev0.0` with software CS on BCM 22 and BCM 27 (`SPI_NO_CS` —
mandatory because SPI0's native CE0/CE1 are wired to the MAX31865
sample-resistance clicks, not the motors). See
[TMC5160 Commissioning §4](tmc5160-commissioning.md#4-spi-topology--four-devices-one-bus)
for the full four-device SPI0 topology.

## 5. Guided Configuration

Create a local configuration:

```bash
python3 scripts/hardware_setup.py wizard \
  --config config/onboard.local.ini
```

The wizard reads `config/onboard.example.ini` as a template, applies the
final pin map and commissioning current, validates it, and writes the
result to `--config`. Edit `sensor.sequent_rtd_*` keys afterward
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
sensor.sequent_rtd_poll_ms=1000
sensor.sequent_rtd_expect_sensor_type=pt100
sensor.sequent_rtd_resistance_min_ohm=60.0
sensor.sequent_rtd_resistance_max_ohm=390.0
sensor.sequent_rtd_crosscheck_tol_c=2.0
```

Which card terminal each specimen's PT100 is on is part of the motor groups,
`motor0.specimens` / `motor1.specimens` (section 8).

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

## 6b. MAX31865 Sample-Resistance Clicks

Schematic v3 also wires two MikroE RTD Click boards (MAX31865), each 4-wire
Kelvin to one coating specimen — a physically separate instrument from the
RTD HAT above, measuring specimen resistance directly rather than PT100
element resistance. They sit on SPI0's **native hardware** chip-selects, not
GPIO: CE1 (BCM 07) is click 1/SAMPLE1 on `/dev/spidev0.1`; CE0 (BCM 08) is
click 2/SAMPLE2 on `/dev/spidev0.0`. Do not cross the wiring — the CE
numbering and the SAMPLE numbering intentionally do not match (CE1->SAMPLE1
but CE0->SAMPLE2, `spidev0.0`).

```ini
sensor.max31865_reference_ohm=470.0
sensor.max31865_poll_ms=1000
sensor.resistance_source=max31865_click
```

Click 1 reads the first specimen of `motor0.specimens`, click 2 the first of
`motor1.specimens` (section 8): list the specimen whose resistance you
monitor first in its motor's list.

Check it:

```bash
printf 'CHECK MAX31865\n' | nc 127.0.0.1 5000
```

Expect `max31865_1=OK` and `max31865_2=OK` in the reply. A saturated
(out-of-range) specimen still reports OK here — saturation is a valid
measurement of an out-of-range specimen, not a bus failure; the affected
sample's `RESISTANCE=` slot simply does not advance.

**Before trusting the reference-resistor default or any reading, complete
gates 4-5** in
[Sequent RTD Bench Bring-Up §9](sequent-rtd-bring-up.md#9-max31865-sample-resistance-click-bring-up-blocking-gates) —
the populated reference resistor (470 vs 400 Ω) must be confirmed against
the actual board, and the coating resistance range is unknown by design
until characterised at the bench. Which specimens the clicks read follows
the specimen lists; confirm it against the real specimen wiring — see the
same document, section 10.

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

Two four-channel EKM014 boards provide six used channels. Connect H1..H6 in
the pin-table order. Each motor group lists its specimens — the PT100's card
terminal and, when heated, the heater's BCM line
([Configuration: Motor groups](configuration.md#motor-groups-motorspecimens)).
The schematic v3 wiring is:

```ini
motor0.specimens=ch1:19,ch2:13,ch3:6,ch4:5
motor1.specimens=ch5:24,ch6:23,ch7,ch8
heater.active_high=true
heater.pwm_frequency_hz=1.0
power.max_active_heaters=3
power.max_thermal_w=15.0
```

Test each output with an LED or meter before attaching heaters. Once heaters
and PT100s are connected, pair them by measurement — this matters whenever the
harness does not follow the schematic — with
`python3 scripts/associate_heaters.py auto` on the Pi (bench mode, all eight
probes reading; see [Instruction Manual §10](rev-c-instruction-manual.md#10-changing-heater-to-pt100-mapping)).
It writes the PT100 terminal each heater warms into its specimen in
`motor0.specimens` / `motor1.specimens`; `show`, `watch` and `heat H<i>`
debug it. Heat cannot tell motor groups: `touch` switches each heater on
until you type the motor you feel it warming on (and finds the unheated
specimens by the PT100 you warm in your fingers), and `assign --motor0 …
--motor1 …` sets the pairs and the motor groups by hand. The heater lines are held low from power-on by the managed
`gpio=` block in `/boot/firmware/config.txt`, derived from the same lists:
after changing a heater line run `coatheal-deploy` and reboot (`show`, a
write by `auto` or `assign`, and `hardware_setup.py doctor` warn while the
block is stale).

A missing GPIO disables only that heater channel. Any invalid mapped PT100
forces duty to zero, including manual-duty commands. `power.max_active_heaters=3` is an
owner power-budget rule — the scheduler never energises a fourth heater
regardless of demand; see
[TMC5160 Commissioning §8, gate 7](tmc5160-commissioning.md#gate-7--max-3-heaters-ceiling-under-load)
for the bench observation of this ceiling under load, and
[TMC5160 Commissioning §8, gate 6](tmc5160-commissioning.md#gate-6--heater-map-walk-blocking)
for the GPIO↔heater↔sample walk.

## 9. TMC5160 and Motors

**Motion is SPI-only. There is no STEP/DIR wiring on this schematic.** For
each TMC5160 (QHV5160 v2) carrier:

1. Connect VIO to 3.3 V and VM to the fused 12 V motor rail.
2. Follow the carrier documentation for any mode straps; do not infer jumper
   positions from the retired TMC2240 carrier family.
3. Connect shared MOSI, MISO, and SCLK; use separate software CS lines
   (BCM 22 for motor 0, BCM 27 for motor 1).
4. Verify motor coil pairs with an ohmmeter. Connect one coil to A1/A2 and the
   other to B1/B2. Never connect/disconnect a motor while VM is powered.
5. **Read the actual sense-resistor value off each board before trusting the
   current model** — `motor*.sense_resistor_ohm=0.075` is an assumed typical
   value, not a measured one. The TMC5160 derives current continuously from
   GLOBALSCALER/IRUN and this resistor value; it has no TMC2240-style fixed
   peak-current range.

Commissioning configuration:

```ini
motor0.run_current_a_rms=0.8
motor1.run_current_a_rms=0.8
motor0.sense_resistor_ohm=0.075
motor1.sense_resistor_ohm=0.075
motor0.retry_ms=2000
motor1.retry_ms=2000
stepper.enable_on_boot=false
pull.microstep=4
```

The onboard software performs pipelined IOIN readback, requires TMC5160
`VERSION=0x30`, and verifies the registers it configures (GCONF and CHOPCONF
are read back and compared after every `Reinitialize()`). EN remains inactive
if verification fails. GSTAT (`0x01`) and DRV_STATUS (`0x6F`) are **not** read
by the flight software — they are diagnostics-only registers, read by
`scripts/spi_probe.py` (`read_tmc5160_set`) when you run the bench probe.
`motor*.current_range_a_peak` and `motor*.pulse_high_us` no longer exist as
keys.

See [TMC5160 Commissioning](tmc5160-commissioning.md) before applying motor
power — it also covers the four-device SPI0 topology shared with the
MAX31865 clicks (section 6b above) and the `SPI_NO_CS` requirement.

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
