# TMC2240 Pin Configuration and Commissioning

This document is the authoritative procedure for configuring and commissioning
the two TMC2240 motor channels in COATHEAL Rev C.

## 1. Supported Interface

The onboard application supports TMC2240 in SPI plus STEP/DIR mode:

- Linux `spidev` performs 40-bit, MSB-first, SPI mode 3 transactions.
- `libgpiod` controls software CS, STEP, DIR, and EN.
- Both drivers may share one SPI device because each has a separate CS GPIO.
- The driver must return IOIN `VERSION=0x40`.
- Register reads use the required pipelined second SPI transaction.
- EN stays inactive until SPI setup and register readback succeed.

The implementation does not use Arduino `TMCStepper`. That library is not the
hardware abstraction used by this Linux application. The required interfaces
are the Raspberry Pi kernel `spidev` driver and `libgpiod`.

Primary references:

- [Analog Devices TMC2240 product page](https://www.analog.com/en/products/TMC2240.html)
- [Analog Devices TMC2240 data sheet](https://www.analog.com/media/en/technical-documentation/data-sheets/tmc2240_datasheet.pdf)

## 2. Electrical Safety

1. Disconnect motor power before changing wiring.
2. Never connect or disconnect a motor while VM is energized.
3. Power TMC2240 VIO from a Pi-compatible 3.3 V logic rail.
4. Power VM from the fused, current-limited motor rail supported by the carrier.
5. Join Pi ground, TMC2240 logic ground, and motor-supply ground.
6. Fit the carrier's required bulk and bypass capacitors close to VM/GND.
7. Fit a heatsink and provide airflow before increasing motor current.
8. Add a hardware pull-up from active-low EN/DRV_ENN to VIO. This keeps the
   bridge disabled while the Pi boots or its GPIO is unclaimed.
9. Verify the exact carrier pinout and mode straps from its manufacturer. Pin
   labels and jumper positions are not standardized between TMC2240 carriers.
10. Verify IREF/full-scale-current hardware. TMC2240 uses integrated current
    sensing; it does not use external phase sense resistors.

## 3. Default Raspberry Pi Wiring

Numbers in configuration are GPIO line offsets. On Raspberry Pi 4
`/dev/gpiochip0`, these are BCM GPIO numbers, not physical header numbers.

### Shared SPI0 Signals

| Function | Physical pin | BCM | TMC2240 signal |
|---|---:|---:|---|
| 3.3 V logic | 1 or 17 | - | VIO |
| Ground | any GND | - | GND |
| SPI0 MOSI | 19 | 10 | SDI/MOSI |
| SPI0 MISO | 21 | 9 | SDO/MISO |
| SPI0 SCLK | 23 | 11 | SCK |

The application does not assign MOSI, MISO, or SCLK line numbers. Linux assigns
those pins when SPI0 is enabled. Select the bus with `motor*.spi_device`.

### Motor-Specific Signals

| Function | Physical pin | BCM | Configuration key |
|---|---:|---:|---|
| Motor 0 CSN | 15 | 22 | `motor0.cs_line` |
| Motor 0 STEP | 35 | 19 | `motor0.step_line` |
| Motor 0 DIR | 37 | 26 | `motor0.dir_line` |
| Motor 0 EN/DRV_ENN | 32 | 12 | `motor0.enable_line` |
| Motor 1 CSN | 16 | 23 | `motor1.cs_line` |
| Motor 1 STEP | 18 | 24 | `motor1.step_line` |
| Motor 1 DIR | 38 | 20 | `motor1.dir_line` |
| Motor 1 EN/DRV_ENN | 40 | 21 | `motor1.enable_line` |

Connect each motor coil pair to one bridge: one pair to A1/A2 and the other to
B1/B2. Identify pairs with an ohmmeter while all power is disconnected.

## 4. Enable Linux Interfaces and Libraries

Run on the Raspberry Pi:

```bash
sudo raspi-config nonint do_spi 0
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config libgpiod-dev gpiod
sudo reboot
```

After reboot:

```bash
ls -l /dev/spidev*
gpiodetect
gpioinfo gpiochip0
pkg-config --modversion libgpiod
```

The normal configuration expects `/dev/spidev0.0` and `/dev/gpiochip0`.
Do not configure a kernel chip-select overlay on BCM 22 or BCM 23. COATHEAL
opens spidev with `SPI_NO_CS` and controls both CSN signals through libgpiod.

## 5. Configure Both Motors

Create or edit `config/onboard.local.ini`. The complete default motor section is:

```ini
stepper.steps_per_rev=200
stepper.default_step_hz=100.0
stepper.max_position_steps=200000
stepper.enable_on_boot=false

pull.max_step_hz=100.0
pull.accel_steps_per_s2=200.0
pull.microstep=4
pull.travel_full_steps=200
pull.hold_s=5.0

motor0.driver=tmc2240
motor0.gpio_chip=/dev/gpiochip0
motor0.spi_device=/dev/spidev0.0
motor0.cs_line=22
motor0.step_line=19
motor0.dir_line=26
motor0.enable_line=12
motor0.invert_direction=false
motor0.enable_active_low=true
motor0.run_current_a_rms=0.8
motor0.current_range_a_peak=0
motor0.hold_current_frac=0.30
motor0.stealth_chop=true
motor0.spi_speed_hz=1000000
motor0.pulse_high_us=3
motor0.retry_ms=2000
motor0.samples=0,1,2,3

motor1.driver=tmc2240
motor1.gpio_chip=/dev/gpiochip0
motor1.spi_device=/dev/spidev0.0
motor1.cs_line=23
motor1.step_line=24
motor1.dir_line=20
motor1.enable_line=21
motor1.invert_direction=false
motor1.enable_active_low=true
motor1.run_current_a_rms=0.8
motor1.current_range_a_peak=0
motor1.hold_current_frac=0.30
motor1.stealth_chop=true
motor1.spi_speed_hz=1000000
motor1.pulse_high_us=3
motor1.retry_ms=2000
motor1.samples=4,5,6,7
```

## 6. Change Pins

Change these keys independently for either motor:

```ini
motor0.gpio_chip=/dev/gpiochip0
motor0.cs_line=22
motor0.step_line=19
motor0.dir_line=26
motor0.enable_line=12
```

Rules:

1. A line is identified by its `gpio_chip` plus line offset.
2. CS, STEP, DIR, and EN must be unique on the same GPIO chip.
3. They must not collide with heater, LED, or sensor GPIO outputs.
4. Do not assign BCM 9, 10, or 11; SPI0 owns those lines.
5. Do not assign BCM 2 or 3; I2C1 owns those lines.
6. Keep CSN high and EN inactive when idle.
7. Change `invert_direction`, not energized motor wiring, to reverse motion.
8. Change `enable_active_low` only if the carrier has an active-high enable
   stage. The TMC2240 DRV_ENN input itself is active low.

For a different SPI controller, enable it in the Pi device tree, verify the
resulting `/dev/spidevB.C` node, connect that controller's hardware pins, and
set both `motor*.spi_device` values. SPI mode remains fixed at mode 3 because
that is a TMC2240 protocol requirement.

Validate every edit:

```bash
./build/onboard/coatheal_onboard \
  --config config/onboard.local.ini --check-config
```

Invalid pins, duplicate assignments, unsupported current ranges, SPI speeds
above 10 MHz, and obsolete motor keys cause configuration loading to fail.

## 7. Current Configuration

Start at `0.8 A RMS`. Do not use the motor's 2.5 A nameplate value as the
driver setting: the TMC2240 data sheet specifies a typical maximum of
`2.1 A RMS` under its stated PCB and thermal conditions.

`motor*.current_range_a_peak=0` automatically selects the lowest TMC2240 peak
range that contains `run_current_a_rms * sqrt(2)`:

| Range code | Peak range |
|---:|---:|
| 0 | 1 A |
| 1 | 2 A |
| 2 | 3 A |

The software sets IRUN to 31 and computes `GLOBALSCALER` from requested peak
current and the selected range. For `0.8 A RMS`, auto mode selects the 2 A
peak range and `GLOBALSCALER=145`. Register value `0` represents full scale
(`256/256`). You may force exactly `1`, `2`, or `3` A:

```ini
motor0.current_range_a_peak=2
```

Configuration fails if the forced range cannot contain the requested current.
The carrier's IREF hardware may impose a lower limit than software expects, so
measure phase current during commissioning.

## 8. Build and Install

```bash
cd /bexus/code/coatheal
cmake -S . -B build -DCOATHEAL_STRICT=ON
cmake --build build --clean-first --parallel 2
ctest --test-dir build --output-on-failure
sudo ./scripts/install_onboard_service.sh \
  /bexus/code/coatheal \
  /bexus/code/coatheal/config/onboard.local.ini
```

Confirm the service uses the intended file:

```bash
cat /etc/coatheal/env
sudo systemctl restart coatheal-onboard.service
systemctl status coatheal-onboard.service --no-pager
journalctl -u coatheal-onboard.service -n 100 --no-pager
```

## 9. Read-Only Driver Checks

Leave the mechanism unloaded and keep the motor supply current-limited:

```bash
printf 'COMPONENTS\n' | nc 127.0.0.1 5000
printf 'CHECK MOTOR0\n' | nc 127.0.0.1 5000
printf 'CHECK MOTOR1\n' | nc 127.0.0.1 5000
```

A successful check proves that GPIO lines were claimed, IOIN returned version
`0x40`, and GCONF, DRV_CONF, GLOBALSCALER, IHOLD_IRUN, CHOPCONF, and PWMCONF
read back correctly. It does not prove that motor coils are wired correctly or
that the mechanism physically moves.

## 10. Supervised Motion Test

Clear the full mechanical travel. Test one motor at a time:

```bash
python3 scripts/hardware_setup.py motor-test \
  --motor 0 --steps 200 --speed 25 --confirm-motion

python3 scripts/hardware_setup.py motor-test \
  --motor 1 --steps 200 --speed 25 --confirm-motion
```

Or use the command port:

```bash
printf 'ARM\n' | nc 127.0.0.1 5000
printf 'STEPPER_ENABLE 0\n' | nc 127.0.0.1 5000
printf 'STEPPER_SET_SPEED 0 25\n' | nc 127.0.0.1 5000
printf 'STEPPER_MOVE 0 200\n' | nc 127.0.0.1 5000
printf 'STEPPER_MOVE 0 -200\n' | nc 127.0.0.1 5000
printf 'STEPPER_DISABLE 0\n' | nc 127.0.0.1 5000
```

Confirm direction, smooth motion, supply current, carrier temperature, and that
EN becomes inactive after disable. Relative moves are allowed before zeroing.
Absolute moves and sequences require:

```bash
printf 'SET_POSITION_ZERO 0\n' | nc 127.0.0.1 5000
```

## 11. Troubleshooting

### `CHECK MOTOR0` or `CHECK MOTOR1` fails

```bash
ls -l /dev/spidev0.0 /dev/gpiochip0
gpioinfo gpiochip0
journalctl -u coatheal-onboard.service -n 100 --no-pager
```

Check 3.3 V VIO, VM, common ground, SPI mode straps, CSN, SCK, SDI, SDO, EN,
and IREF. Check that UART mode is not selected. A returned version other than
`0x40` means the selected CS device is not a supported TMC2240.

### GPIO line is busy

Use `gpioinfo` to identify the consumer. Remove conflicting overlays or change
the relevant `motor*.gpio_chip` and line setting. Do not move a pin in the
configuration without moving the physical wire.

### SPI works but the motor does not move

Check VM, active-low EN polarity, coil pairs, STEP/DIR continuity, current
limit, mechanical freedom, and ball-screw loading. The software counts emitted
pulses but has no encoder and cannot prove physical displacement.

### Driver stops during motion

The backend disables EN on SPI failure, overtemperature warning/shutdown, short
to ground, or short to supply. Remove power and inspect wiring and cooling
before retrying. Do not bypass the fault check.

## 12. Acceptance Checklist

- [ ] Exact TMC2240 carrier pinout and mode straps are documented.
- [ ] VIO is 3.3 V and all signal grounds are common.
- [ ] EN has a hardware inactive pull-up.
- [ ] VM is fused and current-limited for first power-up.
- [ ] Motor coil pairs are identified with power disconnected.
- [ ] `--check-config` reports `Config OK`.
- [ ] Both targeted `CHECK MOTORx` commands pass.
- [ ] Both motors complete a 200-microstep out-and-back test at 25 Hz.
- [ ] Direction and software travel limits are verified.
- [ ] Phase current and carrier temperature are measured at `0.8 A RMS`.
- [ ] Driver-fault shutdown and service restart behavior are verified.
