# TMC5160 Commissioning (Schematic v3)

This is the authoritative bring-up procedure for the two TMC5160 stepper
channels in the schematic v3 electronics. It supersedes and replaces
`docs/tmc2240-pin-configuration-and-commissioning.md`, which described the
retired TMC2240 STEP/DIR carriers and no longer matches the wiring in hand.

**Motion is SPI-only. There is no STEP/DIR wiring in v3, and there never will
be** — the QHV5160 v2 boards' `REFL/STEP`, `REFR/DIR`, `DIAG0/1`, and encoder
pins are all physically unconnected on the schematic. Every motion command
reaches the motor exclusively through SPI register writes to the TMC5160's
internal ramp generator. Section 7 below explains why and how.

For sample-temperature bring-up (Sequent RTD HAT) and the MAX31865
sample-resistance click instrument, see
[Sequent RTD Bench Bring-Up](sequent-rtd-bring-up.md), which also carries the
resistance-instrument bench gates for this same v3 electronics revision.

## 1. Supported Interface

The onboard application drives both TMC5160 channels purely over SPI:

- Linux `spidev` performs 40-bit, MSB-first, SPI mode 3 transactions (write =
  register address OR `0x80`).
- `libgpiod` controls a software chip-select (CS) and the EN line only —
  there is no STEP, DIR, or DRDY GPIO for either motor.
- Both drivers share `/dev/spidev0.0`; each has its own CS GPIO, opened with
  the kernel's native chip-select disabled (`SPI_NO_CS` — see section 4).
- `Tmc5160Driver::Reinitialize()` requires IOIN `VERSION == 0x30` before
  trusting the part. **`0x30` is the TMC5160. `0x40` (the old TMC2240 gate)
  must FAIL here** — a `0x40` reading on this bus means the wrong chip, the
  wrong CS, or a wiring mixup with the SPI0 native chip-selects (which belong
  to the MAX31865 clicks, not the motors).
- Register reads use the required pipelined second SPI transaction (a TMC51xx
  read returns the *previous* transaction's data).
- EN stays inactive (motor de-energised) until GPIO claim, SPI open, and
  register readback all succeed.

Registers this driver touches (all TMC5160 datasheet addresses):

| Register | Address | Purpose |
|---|---|---|
| GCONF | `0x00` | Global config; `en_pwm_mode` (StealthChop) clear by default = spreadCycle, `motorN.stealth_chop=true` sets it |
| IOIN | `0x04` | `VERSION` field, bits 31:24 |
| GLOBALSCALER | `0x0B` | Current DAC coarse scale |
| IHOLD_IRUN | `0x10` | Run/hold current + hold delay |
| TPOWERDOWN | `0x11` | Stand-still power-down delay |
| RAMPMODE | `0x20` | `0` = positioning mode |
| XACTUAL | `0x21` | Ramp generator's actual position |
| VSTART | `0x23` | Ramp start velocity |
| A1 | `0x24` | First acceleration |
| V1 | `0x25` | First-phase velocity threshold |
| AMAX | `0x26` | Max acceleration |
| VMAX | `0x27` | Max velocity |
| DMAX | `0x28` | Max deceleration |
| D1 | `0x2A` | First deceleration |
| VSTOP | `0x2B` | Ramp stop velocity |
| XTARGET | `0x2D` | Ramp generator's target position |
| CHOPCONF | `0x6C` | `MRES` (bits 27:24), `TOFF` (bits 3:0) |
| DRV_STATUS | `0x6F` | Driver status/diagnostics — read by `scripts/spi_probe.py`, not by the flight software |

Primary reference: [Analog Devices TMC5160A data sheet](https://www.analog.com/media/en/technical-documentation/data-sheets/TMC5160A_datasheet_rev1.17.pdf).

## 2. Electrical Safety

1. Disconnect motor power (VM) before changing wiring.
2. Never connect or disconnect a motor while VM is energised.
3. Power TMC5160 VIO from the Pi's 3.3 V logic rail; VM from the fused,
   current-limited 12 V motor rail (Pololu D42V110F12).
4. Join Pi ground, TMC5160 logic ground, and motor-supply ground.
5. Fit the carrier's required bulk and bypass capacitors close to VM/GND.
6. Fit a heatsink and provide airflow before increasing motor current.
7. Add a hardware pull-up from active-low EN/`DRV_ENN` to VIO so the bridge
   stays disabled while the Pi boots or its GPIO is unclaimed.
8. Verify the exact QHV5160 v2 board pinout and any mode straps against its
   own documentation — do not assume TMC2240-carrier pin labels apply.
9. **Read the board's actual sense-resistor value before trusting the current
   model in section 6.** `motor*.sense_resistor_ohm=0.075` is an assumed
   typical value for this board family, not a measured one.

## 3. v3 Pin Map

All numbers are BCM GPIO line offsets on `/dev/gpiochip0`, matching the plan's
Global Constraints GPIO table exactly.

| Function | BCM | Configuration key |
|---|---:|---|
| Motor 0 (STEP1) CS | 22 | `motor0.cs_line` |
| Motor 0 (STEP1) EN | 20 | `motor0.enable_line` |
| Motor 1 (STEP2) CS | 27 | `motor1.cs_line` |
| Motor 1 (STEP2) EN | 21 | `motor1.enable_line` |
| SPI0 MOSI / MISO / SCLK | 10 / 9 / 11 | fixed Pi SPI0, not configurable |

There is no `step_line`, `dir_line`, or `pulse_high_us` key. Those keys are
**rejected at config load** (`"unknown motor config key"`) — they were
retired along with the TMC2240 driver, not merely deprecated.

BCM 22 and 27 are **software** chip-selects, driven by `libgpiod`, not the
kernel's native SPI chip-select. That is a deliberate consequence of section
4's SPI topology, not a simplification.

## 4. SPI Topology — Four Devices, One Bus

SPI0 on this schematic carries **four** devices, not two:

```text
/dev/spidev0.0  (SPI0, kernel CE0 = BCM 08)  <-- MAX31865 click 2 (SAMPLE2), native CE
                                                  ALSO the device node both TMC5160
                                                  drivers open with SPI_NO_CS (soft CS)
/dev/spidev0.1  (SPI0, kernel CE1 = BCM 07)  <-- MAX31865 click 1 (SAMPLE1), native CE

Motor 0 (TMC5160) --- soft CS BCM 22 --- shares /dev/spidev0.0, SPI_NO_CS
Motor 1 (TMC5160) --- soft CS BCM 27 --- shares /dev/spidev0.0, SPI_NO_CS
```

| Device | CS mechanism | Device node | Mode | Speed |
|---|---|---|---:|---:|
| MAX31865 click 1 (SAMPLE1) | hardware CE1 (BCM 07) | `/dev/spidev0.1` | 1 | ≤1 MHz |
| MAX31865 click 2 (SAMPLE2) | hardware CE0 (BCM 08) | `/dev/spidev0.0` | 1 | ≤1 MHz |
| Motor 0 (TMC5160) | software CS (BCM 22) | `/dev/spidev0.0`, `SPI_NO_CS` | 3 | ≤4 MHz |
| Motor 1 (TMC5160) | software CS (BCM 27) | `/dev/spidev0.0`, `SPI_NO_CS` | 3 | ≤4 MHz |

**Why `SPI_NO_CS` is mandatory, not an optimisation:** CE0 and CE1 are
physically wired to the two MAX31865 clicks. If the motor drivers opened
`/dev/spidev0.0` without `SPI_NO_CS`, the kernel would assert the native CE0
line on *every* motor SPI transfer — glitching whichever click sits on that
chip-select mid-conversion — while the motor's own addressing is done
entirely through the software CS toggled around each 5-byte TMC5160
datagram. `LinuxSpiBus::Open(..., no_cs=true)` ORs `SPI_NO_CS` into the mode
bits for exactly this reason.

**How the four devices actually share the bus.** Motors and clicks run on
independent threads and *do* contend for SPI0 — there is no "one at a time by
construction". Two mechanisms make that safe, both in
`onboard/src/hal/spi_bus_lock.hpp` and `spi_bus.hpp`:

1. **One lock per physical controller.** `SpiBusLock` is keyed by
   `SpiControllerKey()`, which collapses `/dev/spidev0.0` and
   `/dev/spidev0.1` to a single `spi0` key — they are two chip-select views
   of one set of SCLK/MOSI/MISO wires. Each driver holds it for exactly one
   indivisible bus unit: the motor's `cs-low → [settings re-apply + data
   ioctl] → cs-high` triplet, or one complete MAX31865 register
   conversation. The lock is deliberately **not** held across the MAX31865
   one-shot's 10 ms settle and 65 ms conversion waits — those gaps are
   CS-framed by the native CE line, and holding through them would starve
   both motors for ~150 ms per specimen poll.
2. **Per-transfer mode re-application.** The kernel keeps one
   `struct spi_device` per node, so mode/speed set by *any* opener apply to
   *every* opener of it. `SpiBus::Transfer` therefore re-asserts the calling
   opener's own mode/speed/bits (three cheap ioctls, no bus traffic)
   immediately before each data ioctl, inside the lock hold. Without it,
   "last opener wins" would leave click 2 mute (its CE never asserts under
   `SPI_NO_CS`) or run the motors at the wrong CPOL/CPHA with CE0 firing on
   every datagram.

Both motors sharing one device node (`/dev/spidev0.0`) alongside click 2 is
correct and intentional: the device node only selects which kernel SPI
controller instance to open, not which physical chip is addressed — that is
what CS (hardware or software) does.

## 5. Configuration Reference

```ini
# --- Motor 0 / STEP1 (motor0.specimens, v3 pinout CS on BCM 22, EN on BCM 20) ---
# v3: TMC5160, SPI-only motion — no STEP/DIR.
motor0.driver=tmc5160
motor0.gpio_chip=/dev/gpiochip0
motor0.spi_device=/dev/spidev0.0
motor0.cs_line=22
motor0.enable_line=20
# Owner 2026-09-15: motor 0 turns opposite to motor 1 for the same command;
# flipped so "+" is the same physical direction on both. coatheal-deploy pins it.
motor0.invert_direction=true
motor0.enable_active_low=true
motor0.run_current_a_rms=0.8
motor0.hold_current_frac=0.30
motor0.stealth_chop=false
motor0.spi_speed_hz=1000000
motor0.sense_resistor_ohm=0.075
motor0.retry_ms=2000
motor0.specimens=ch1:19,ch2:13,ch3:6,ch4:5

# --- Motor 1 / STEP2 (motor1.specimens, v3 pinout CS on BCM 27, EN on BCM 21) ---
motor1.driver=tmc5160
motor1.gpio_chip=/dev/gpiochip0
motor1.spi_device=/dev/spidev0.0
motor1.cs_line=27
motor1.enable_line=21
motor1.invert_direction=false
motor1.enable_active_low=true
motor1.run_current_a_rms=0.8
motor1.hold_current_frac=0.30
motor1.stealth_chop=false
motor1.spi_speed_hz=1000000
motor1.sense_resistor_ohm=0.075
motor1.retry_ms=2000
motor1.specimens=ch5:24,ch6:23,ch7,ch8

pull.microstep=4
pull.max_step_hz=50.0
pull.accel_steps_per_s2=200.0
```

`motor*.driver` accepts only `tmc5160` and `simulated`. `tmc2240` is rejected
at load with an error naming it retired.

`motor*.stealth_chop` selects GCONF's `en_pwm_mode` (StealthChop) bit:
`false` (the default since 2026-09-11) writes `GCONF = 0x00000000` —
spreadCycle, with the CHOPCONF `TOFF`/`TBL`/`HSTRT` values above as its
chopper parameters — and `true` writes `GCONF = 0x00000004`. Every other
GCONF bit stays at its reset value, so GCONF is exactly this bit or nothing.
The value is carried through `Tmc5160Config::stealth_chop` and confirmed by
the driver's existing GCONF readback verify during `Reinitialize()`, so a
chip that does not accept it fails bring-up loudly rather than running in the
wrong chopper mode. The flight bend runs at ≤ 0.5 mm/s
(`stepper.max_speed_mm_s`), where spreadCycle's torque headroom matters and
StealthChop's silence does not; set `true` only for a bench experiment.
`MOTOR_DEBUG` reports the live mode as `stealth=0|1`.

`motor*.current_range_a_peak` and `motor*.pulse_high_us` no longer exist —
the TMC5160 does not use TMC2240-style fixed peak-current ranges (section 6
below), and there is no STEP pulse to time.

## 6. Current Model: GLOBALSCALER / IRUN, Two Regimes

The TMC5160 has no integrated current-range selector like the TMC2240. Peak
phase current is set continuously from a fixed full-scale sense voltage
(`V_fs = 0.325 V`, datasheet-fixed) and the board's sense resistor:

```text
I_peak = (GLOBALSCALER/256) * ((IRUN+1)/32) * (V_fs / R_sense)
I_peak_max = V_fs / R_sense      (GLOBALSCALER=256, IRUN=31)
```

`Tmc5160Driver::CalculateCurrent()` picks GLOBALSCALER and IRUN in two
regimes so the finer-resolution register (GLOBALSCALER, 8-bit) does as much
of the work as it can:

- **Below 50 % of `I_peak_max`:** IRUN pinned at 31 (max), GLOBALSCALER scaled
  down to hit the target. Below ~12.5 % of `I_peak_max`, GLOBALSCALER would
  need to drop under its sane floor of 32 — instead GLOBALSCALER is pinned at
  32 and IRUN is reduced to carry the remainder.
- **Above 50 % of `I_peak_max`:** GLOBALSCALER pinned at 256 (full scale),
  IRUN reduced from 31 to hit the target.
- **Below ~1.5 % of `I_peak_max`:** rejected outright at config/init time
  rather than silently overcurrenting the motor by double digits of percent.
  This is a deliberate loud-refusal design, not a bug to work around — pick a
  request inside the deliverable range instead.
- **Above `I_peak_max`:** rejected outright — no choice of GLOBALSCALER/IRUN
  can reach a current the sense resistor cannot deliver.

`IHOLD` scales relative to the *chosen* IRUN (not a fixed 0..31 range):
`hold_current_frac=0.0` gives `IHOLD=0`; `hold_current_frac=1.0` gives
`IHOLD==IRUN` (full run current held).

**Ceiling table for the assumed `sense_resistor_ohm=0.075`:**

| Quantity | Value |
|---|---:|
| `I_peak_max = V_fs / R_sense` | `0.325 / 0.075` ≈ **4.333 A peak** |
| `run_current_a_rms` ceiling (`I_peak_max / sqrt(2)`) | ≈ **3.06 A_rms** |
| Absolute flat ceiling (independent of sense resistor) | `3.1 A_rms` (config-enforced backstop) |
| Low-current rejection floor | ~1.5 % of `I_peak_max` = **0.065 A_peak** (≈ **0.046 A_rms**) — see note below |

**Note on the low-current floor.** `0.065` is a **peak** current
(1.5 % × 4.333 A peak), not an RMS one — ≈ 0.046 A_rms. It is also not the
actual accept/reject threshold: `CalculateCurrent` rejects when the value it
*can* deliver at the GLOBALSCALER floor exceeds the request by more than
10 %, and IRUN is a 5-bit ladder, so acceptance in this region is **ragged**
rather than a clean cut. Measured against the shipped implementation at
`R_sense = 0.075 Ω`, the first crossover into acceptance is at
≈ **0.032–0.033 A_rms**, but isolated rejection bands persist above it (e.g.
≈ 0.042 and ≈ 0.054 A_rms); requests above ≈ **0.055 A_rms** are accepted
without exception. Treat anything under ~0.06 A_rms as "may be rejected —
check the load log", not as a guaranteed floor. All of these rescale with
`R_sense`.

**This table is only correct if the board's actual sense resistor is really
0.075 Ω.** Read it off the board (silkscreen value or QHV5160 v2
documentation) at the bench and record it below — every number above rescales
directly if it is not 0.075 Ω.

- Sense resistor value read off motor 0's board: `____ Ω` *(fill in at bench)*
- Sense resistor value read off motor 1's board: `____ Ω` *(fill in at bench)*
- `motor*.sense_resistor_ohm` set to match: `____` *(fill in at bench — update
  both INIs if it differs from the assumed 0.075)*

## 7. Why No STEP/DIR — Position Dribble

The existing `StepperDriver` interface (`hal/stepper_driver.hpp`) is
pulse-level: the channel's pacing thread calls `Step(direction)` once per
configured microstep and owns speed, acceleration, absolute position,
pull-cycle logic, `MotionLock`, and heater inhibit. All of that machinery is
flight-reviewed and unchanged by this driver.

`Tmc5160Driver` implements that same pulse-level contract by mapping each
`Step()` call onto one `ΔXTARGET` write into the chip's own ramp generator
(`RAMPMODE=0`, positioning mode) rather than by toggling a STEP pin:

```text
Step(fwd) -> target_ += (fwd XOR invert_direction) ? +Δ : -Δ
          -> write XTARGET = target_   (one 5-byte SPI datagram)
```

The ramp generator then smooths that single-microstep nudge into the actual
coil drive waveform in hardware, at a demanded rate the pacing thread paces
(≤ the speed ceiling: 100 full-steps/s = `stepper.max_speed_mm_s` 0.5 mm/s at
the 1 mm lead, or `pull.max_step_hz` if that is lower). `Δ = 256 / microstep_divisor`
because XTARGET always counts in the ramp generator's fixed 256
internal-microsteps-per-fullstep resolution, regardless of the configured
`MRES`/microstep divisor.

**`VMAX` is set generously above the worst-case dribble demand so the ramp
generator is never the limiting factor:** at the constant's 100 Hz sizing
basis (twice the 50 Hz flight ceiling), the worst-case XTARGET demand is
`100 * 256` = 25,600 internal-microsteps/s.
`VMAX` is programmed to roughly 73,000 internal-microsteps/s — about a
**2.9× margin** over that worst case (not the 4× the raw
`kVmax = 4 * 100 * 256` constant's arithmetic might suggest; VMAX's register
unit is `f_clk/2^24` internal-microsteps/s, not a 1:1 count, so the actual
margin is smaller than the constant's face value).

**Position truth stays entirely in software**, exactly as it did with the
retired STEP/DIR driver — pulse count is position. XACTUAL is read back only
for health/diagnostic cross-check; it is never the source of truth for
`StepperChannel`'s own position tracking, `SET_POSITION_ZERO`, or bend
sequences. This is a deliberate scope decision: letting the TMC5160 own
motion profiles via its own XTARGET moves would have required rewriting
`stepper_channel`/`stepper_controller` for no benefit at the ≤50 Hz rates
this mechanism actually runs.

### XACTUAL/XTARGET Re-Init Transient (Known, Bounded)

`Reinitialize()` — run on first bring-up and on every re-probe after a fault
(`ActiveCheck()`, `CHECK MOTORn`, or the pull cycle's `driver_retry_ms`
re-probe) — unconditionally writes `XACTUAL=0` then `XTARGET=0` as two
separate SPI datagrams, then resets the driver's own software `target_` to
0. If the chip's ramp generator has any outstanding travel between those two
writes (e.g. a re-probe happening while the mechanism has already moved off
its last-known position), it can execute a small uncommanded step in the
window between the two writes landing — empirically bounded to roughly
**0.03 of a full step**.

This is a known, bounded, cosmetic effect at the sub-microstep scale, not a
safety issue: the mechanism's software travel limits and `MotionLock` are
unaffected, and it can only ever move the shaft by a fraction of one full
step, once, at the moment of a re-probe. It is recorded here rather than
fixed in code because the two writes cannot be merged into a single atomic
SPI transaction on this bus. If a bench observation ever shows a
materially larger transient than this, treat that as a live regression, not
an expected result.

## 8. Bench Gates (Blocking)

Do not apply motor power, and do not trust a `CHECK MOTORn` result, until
gates 1-3 below pass. Gates 6-7 are heater-side but bench-gated alongside the
stepper work because they exercise the same v3 GPIO map and power rail this
document already has you probing.

### Gate 1 — IOIN VERSION and EN Polarity (BLOCKING)

1. With motor power connected but the mechanism unloaded, read IOIN (`0x04`)
   on both drivers via `spi_probe.py` (section 9) or `CHECK MOTOR0`/`CHECK
   MOTOR1`.
2. Confirm `IOIN.VERSION == 0x30` on **both** drivers. `0x40` (TMC2240) or
   any other value means the wrong chip is answering that CS, or CE0/CE1
   traffic is bleeding onto the motor's soft-CS transaction (see gate 3).
3. With the onboard service running and `stepper.enable_on_boot=false`
   (the flight default), confirm both motors are **de-energised at boot** —
   EN GPIO in its inactive state, no holding torque on either shaft, before
   any `STEPPER_ENABLE` command is sent.

**Record here:**

- Motor 0 observed `IOIN.VERSION`: `____` *(fill in at bench — must be 0x30)*
- Motor 1 observed `IOIN.VERSION`: `____` *(fill in at bench — must be 0x30)*
- Both motors de-energised at boot before `ARM`/`STEPPER_ENABLE`: YES / NO
  *(fill in at bench)*
- Gate 1 result: PASS / FAIL *(fill in at bench)*

### Gate 2 — One-Revolution Test: Step→ΔXTARGET Scale and Direction (BLOCKING)

`DeltaXtarget(divisor) = 256 / divisor` is a datasheet-derived constant, not
bench-measured. Confirm it against a real revolution before trusting any
commanded travel:

1. Mark the shaft (or an attached full-travel reference) at a known
   orientation.
2. With `pull.microstep` at its configured value (default `4`) and
   `stepper.steps_per_rev=200`, command exactly
   `steps_per_rev * microstep` `Step()` calls in one direction
   (`STEPPER_MOVE <id> <steps_per_rev*microstep>` using **microsteps at the configured divisor** (µ4: 800 per revolution), i.e.
   `STEPPER_MOVE <id> 800` at the default µ4 — the command takes microsteps;
   nothing multiplies by the divisor for you).
3. Confirm the mark returns to the same orientation — exactly one shaft
   revolution, no more, no less.
4. Reverse and repeat to confirm the return trip also lands on the mark.
5. Confirm the *sign* of physical rotation matches the commanded direction
   (`STEPPER_MOVE <id> <positive>` should rotate the direction this bench
   procedure defines as forward). If it does not, set
   `motor*.invert_direction=true` for that motor — **do not** rewire the
   motor phases to fix a direction sign. (Bench 2026-09-15: motor 0 ran
   opposite to motor 1, so the flight config sets
   `motor0.invert_direction=true`; recheck both after any rewiring.)

**Record here:**

- Motor 0: full steps commanded for one revolution: `____`
  *(fill in at bench — expected `steps_per_rev`=200 at the command layer)*
- Motor 0: shaft returned to the mark after one revolution: YES / NO
- Motor 0: commanded-positive direction matches physical-forward: YES / NO
  *(if NO, `motor0.invert_direction` set to: `____`)*
- Motor 1: full steps commanded for one revolution: `____`
- Motor 1: shaft returned to the mark after one revolution: YES / NO
- Motor 1: commanded-positive direction matches physical-forward: YES / NO
  *(if NO, `motor1.invert_direction` set to: `____`)*
- Gate 2 result: PASS / FAIL *(fill in at bench)*

### Gate 3 — SPI_NO_CS Verification (BLOCKING)

Confirm motor SPI traffic does not disturb the MAX31865 clicks sharing this
bus:

1. Wire both MAX31865 clicks (section 4) and confirm they read healthily
   (`CHECK MAX31865`, or see
   [Sequent RTD Bench Bring-Up](sequent-rtd-bring-up.md)) with the motors
   idle.
2. With both clicks still connected, run sustained motor SPI traffic —
   `STEPPER_ROTATE`, a bend sequence, or repeated `CHECK MOTOR0`/`CHECK
   MOTOR1` — for at least one minute.
3. During and immediately after that motor traffic, confirm the clicks show
   **no new fault bits, no reading gaps, and no `MAX31865_1`/`MAX31865_2`
   health degradation** in `COMPONENTS`/`CHECK MAX31865`. Any correlation
   between motor SPI activity and click faults means `SPI_NO_CS` did not
   take effect — check `LinuxSpiBus::Open`'s `no_cs` argument and the kernel
   SPI mode readback before proceeding.
4. **Motor-side check (the direction step 3 cannot see).** Steps 1–3 only
   catch corruption of the *clicks*. Interleaved click traffic can equally
   corrupt a *motor* datagram — a garbage 40-bit word latched on CS rise
   can carry bit 7 set and land as a WRITE to whatever register byte 0
   happens to name. Before the burst, record `GCONF` (`0x00`), `CHOPCONF`
   (`0x6C`) and `IHOLD_IRUN` (`0x10`) for both motors with
   `scripts/spi_probe.py`. Immediately after the burst, read all three
   again. **They must be bit-for-bit unchanged.** Any difference is a
   corrupted motor write and fails this gate regardless of click health —
   re-check that both drivers take `SpiBusLock` around every transfer and
   that `SpiControllerKey()` collapses both device nodes to one key.

**Record here:**

- Click health before motor SPI burst: `____`
- Click health during/after motor SPI burst: `____`
- Any correlated fault/degradation observed: YES / NO *(must be NO to pass)*
- motor0 GCONF / CHOPCONF / IHOLD_IRUN before: `____` / `____` / `____`
- motor0 GCONF / CHOPCONF / IHOLD_IRUN after:  `____` / `____` / `____`
- motor1 GCONF / CHOPCONF / IHOLD_IRUN before: `____` / `____` / `____`
- motor1 GCONF / CHOPCONF / IHOLD_IRUN after:  `____` / `____` / `____`
- All six register pairs identical: YES / NO *(must be YES to pass)*
- Gate 3 result: PASS / FAIL *(fill in at bench)*

### Gate 6 — Heater Map Walk (BLOCKING)

Confirm the v3 GPIO↔heater↔sample mapping before any closed-loop or
multi-heater operation. See
[Hardware Reference](hardware.md#heater-system) for the pin table
(`H1..H6 = BCM 19,13,6,5,24,23`).

1. With heaters unloaded or on current-limited dummy loads, energise **one
   heater at a time** at low duty (`heater.debug_max_duty` or lower) via
   `SET_HEATER_DUTY <index> <low_duty>` or `HEATER_TEST`.
2. For each of H1..H6 (the heated specimens of `motor0.specimens` /
   `motor1.specimens` in order, H0..H5 in software), confirm:
   - the correct physical GPIO line toggles (meter or LED on the expected
     BCM line only);
   - the correct EKM014 channel switches;
   - the correct physical sample position warms (thermally, or via its
     PT100: `scripts/associate_heaters.py` measures that pairing and writes
     each heater's PT100 terminal into its specimen in the lists).
3. Confirm no *other* heater or GPIO activates during any single-heater test.
4. **Heater-inhibit latency — scope check (BLOCKING).** With one heater
   energised at a duty that is clearly ON (say 0.5), command a PULL on the
   motor owning that sample and scope or logic-analyse the heater's BCM line
   against the motor's CS line. The heater GPIO must go low **within one PWM
   SLICE of the PULL, not within one period.** At
   `heater.pwm_frequency_hz = 1.0` a period is 1000 ms and a slice is
   1000/100 = **10 ms**, so the acceptance bound is ~10 ms (allow a couple of
   slices for scheduler jitter on a loaded Pi; anything approaching 1000 ms
   means the duty is being sampled once per period again — see
   `RenderPwmPeriod` in `onboard/include/coatheal/hal/pwm_controller.hpp`).
   This step exists because the software-PWM worker thread cannot run on the
   Windows dev host (`COATHEAL_HAS_LIBGPIOD` is undefined there): the unit
   tests cover the slice-timing decision the loop delegates to, and this gate
   is the only place the real GPIO drop is measured end-to-end.

**Record here:**

- Heater GPIO fall time after PULL command: `____` ms
  *(must be within a few slices of ~10 ms, NOT ~1000 ms)*

| Heater | BCM | Confirmed correct GPIO | Confirmed correct sample |
|---|---:|---|---|
| H1 | 19 | `____` | `____` |
| H2 | 13 | `____` | `____` |
| H3 | 6 | `____` | `____` |
| H4 | 5 | `____` | `____` |
| H5 | 24 | `____` | `____` |
| H6 | 23 | `____` | `____` |

- Gate 6 result: PASS / FAIL *(fill in at bench)*

### Gate 7 — Max-3-Heaters Ceiling Under Load

`power.max_active_heaters=3` / `power.max_thermal_w=15.0` is the owner's
power-budget rule, enforced by `HeaterScheduler`. Confirm the scheduler
actually holds this ceiling on real rails, not just in config validation:

1. Command four or more heaters to nonzero duty simultaneously (via
   individual `SET_HEATER_DUTY` calls or closed-loop targets set to force
   demand on more than three channels at once).
2. Confirm no more than **three** heaters are ever simultaneously energised,
   observed either electrically (current draw / continuity on the rail) or
   via telemetry `HEATER_DUTY=`.
3. Confirm the scheduler's selection is sane under sustained load (it does
   not thrash or leave a channel starved indefinitely) — this is a
   scheduler-behaviour observation, not a specific selection algorithm this
   document mandates.

**Record here:**

- Max simultaneous energised heaters observed under forced 4+ demand: `____`
  *(must be ≤ 3 to pass)*
- Scheduler behaviour sane under sustained multi-channel demand: YES / NO
- Gate 7 result: PASS / FAIL *(fill in at bench)*

## 9. Useful Operator Commands

Read-only SPI diagnostic (no register writes, no motor movement):

```bash
python3 scripts/spi_probe.py --skip-rtd
```

Omit `--skip-rtd` to also probe the Sequent RTD HAT's I2C presence in the
same run. `spi_probe.py` opens `/dev/spidev0.0` with `SPI_NO_CS` and toggles
each motor's software CS itself — see the script's `DEVICES` tuple and
in-line comments for why.

Active health check through the onboard command server:

```bash
python3 scripts/hardware_setup.py doctor --config config/onboard.local.ini
printf 'CHECK MOTOR0\n' | nc 127.0.0.1 5000
printf 'CHECK MOTOR1\n' | nc 127.0.0.1 5000
```

Supervised motion test, mechanism clear of any obstruction:

```bash
python3 scripts/hardware_setup.py motor-test \
  --motor 0 --steps 200 --speed 25 --confirm-motion

python3 scripts/hardware_setup.py motor-test \
  --motor 1 --steps 200 --speed 25 --confirm-motion
```

Or through the command port directly:

```bash
printf 'ARM\n' | nc 127.0.0.1 5000
printf 'STEPPER_ENABLE 0\n' | nc 127.0.0.1 5000
printf 'STEPPER_SET_SPEED 0 25\n' | nc 127.0.0.1 5000
printf 'STEPPER_MOVE 0 200\n' | nc 127.0.0.1 5000
printf 'STEPPER_MOVE 0 -200\n' | nc 127.0.0.1 5000
printf 'STEPPER_DISABLE 0\n' | nc 127.0.0.1 5000
```

`SET_POSITION_ZERO 0` is required after every onboard restart before absolute
moves, homing, pull cycles, or bend sequences; relative
`STEPPER_MOVE`/`STEPPER_ROTATE` are allowed before zeroing.

## 10. Troubleshooting

### `CHECK MOTOR0` / `CHECK MOTOR1` fails, or `IOIN.VERSION` is not `0x30`

```bash
ls -l /dev/spidev0.0 /dev/gpiochip0
gpioinfo gpiochip0
journalctl -u coatheal-onboard.service -n 100 --no-pager
```

Check VIO (3.3 V), VM, common ground, SPI mode-3 straps, the soft-CS GPIO
(BCM 22/27), MISO/MOSI/SCLK continuity, EN, and that no kernel
`spi0-2cs`-style chip-select overlay is installed (it would fight the
software CS on BCM 22/27 and/or reserve those lines outright).

### `IOIN.VERSION` reads `0x40`

That is a TMC2240 answering, not a TMC5160 — either the wrong carrier is
wired, or the probe is inadvertently reading a click's CE line. Re-check
wiring against section 3/4 before assuming a chip fault.

### GPIO line is busy

```bash
gpioinfo gpiochip0
```

Identify the consumer and free it (remove a conflicting overlay, or change
the relevant `motor*.gpio_chip`/line and move the physical wire — never move
one without the other).

### SPI works but the motor does not move

Check VM, active-low EN polarity, coil pairs (identify with an ohmmeter,
power disconnected), current settings against section 6, mechanical freedom,
and travel limits. The software counts `Step()` calls and can read XACTUAL
for a health cross-check, but has no encoder and cannot prove physical shaft
displacement — that is what gate 2's marked-shaft procedure is for.

### Motor stops mid-sequence / `CHECK MOTORn` intermittently fails

The backend marks itself unhealthy on any SPI transfer failure or IOIN/GCONF/
CHOPCONF readback mismatch, and re-probes on the next `driver_retry_ms`
interval or explicit `CHECK`. Expect the small re-init transient described in
section 7 on each such re-probe; do not chase it as a fault in isolation.

## 11. Acceptance Checklist

- [ ] QHV5160 v2 carrier pinout confirmed against its own documentation, not
      assumed from TMC2240 labels.
- [ ] VIO is 3.3 V and all signal grounds are common.
- [ ] EN has a hardware inactive pull-up on both channels.
- [ ] VM is fused and current-limited for first power-up.
- [ ] Motor coil pairs identified with power disconnected.
- [ ] Actual sense-resistor value read off both boards and recorded (section
      6); `motor*.sense_resistor_ohm` matches if it differs from 0.075.
- [ ] `--check-config` reports `Config OK`.
- [ ] Gate 1 (IOIN VERSION `0x30`, EN polarity) PASSED on both drivers.
- [ ] Gate 2 (one-revolution scale + direction) PASSED on both drivers.
- [ ] Gate 3 (`SPI_NO_CS` verification against the clicks) PASSED.
- [ ] Gate 6 (heater map walk) PASSED.
- [ ] Gate 7 (max-3-heaters ceiling under load) PASSED.
- [ ] Both motors complete a supervised out-and-back test.
- [ ] Direction and software travel limits verified.
- [ ] Phase current and carrier temperature measured at the commissioning
      `run_current_a_rms`.
- [ ] Driver-fault shutdown and service-restart behaviour verified.

## Bench note 2026-08-28: enable-line verification in both directions

Raw probe (service stopped, soft-CS, `SPI_NO_CS` — without it CE0 selects
the MAX31865 click and every register reads `0xFF..`): both modules answer
`VERSION 0x30`, `SD_MODE=0`; motor 0's `DRV_ENN` follows GPIO 20 within
2 ms in both directions; motor 1's `DRV_ENN` reads 0 whatever GPIO 21
does, while the GPIO 21 pad itself drives and reads back correctly and is
not shorted (pull-up/pull-down continuity identical to GPIO 20). The
firmware therefore verifies `DRV_ENN` after *disabling* too: a line that
cannot raise `DRV_ENN` is reported as `motorN_warn=` by `CHECK` and once
in the journal, without failing the motor. A boot-time
`TMC5160_VERSION mismatch got=0x0` means the chip was unpowered (VS/12 V
rail) when the service started; it is re-probed every `motorN.retry_ms`
while idle and clears on its own once the rail is up.

## Bench note 2026-08-29 — the chip resets and forgets its configuration

Symptom: `STEPPER_ENABLE` and `STEPPER_MOVE` are acknowledged, telemetry
position and `pulses` advance, `CHECK` says OK — and nothing turns.
`MOTOR_DEBUG` showed `chopconf=0x10410150` (the TMC5160 **power-on reset
value**, which the firmware never writes), `toff=0`, `xtarget` climbing
while `xactual=0`, and `RAMPSTAT` with `velocity_reached=1` + `vzero=1`:
the ramp generator was running at its target speed of **zero**, because a
reset had wiped `VMAX`, `AMAX`, the currents and `TOFF`. The chip resets
when VM (12 V) or VCC_IO dips — a bench supply on a low current limit is the
usual cause once the coils draw run current.

The firmware now clears `GSTAT` after configuring the chip and re-reads it
on every `Enable(true)`, every 64 steps, and once a second while enabled
and idle; on `GSTAT.reset` it rewrites
the whole configuration, restores the chopper, logs
`[tmc5160] … chip reset detected …`, counts it (`CHECK` → `motorN_warn`,
`MOTOR_DEBUG` → `resets=`), and the console's Debug tab names it in the
verdict. A motor that keeps stalling with a rising `resets` count is a
power-supply problem, not a software one.

## Bench note 2026-08-29 (later) — the actual root cause: motion-controller units

**The TMC5160's XACTUAL/XTARGET/VMAX count in the microstep resolution
selected by CHOPCONF.MRES, not in 1/256-full-step units.** The driver set
MRES from the configured divisor (6 = ¼ step) and wrote `XTARGET += 256 /
divisor = 64` per pulse, so every "one microstep" pulse commanded 64
quarter-steps = **16 full steps (28.8°) in a ~7 ms slam** at AMAX=0xFFFF.
No rotor can follow that: the motor buzzed in place, the position counter
ran 64× ahead of reality, StallGuard sat near stall, the stealthChop
regulator pinned at 255 and the shared 12 V rail collapsed under an
8 rev/s demand, resetting both chips. The tell-tale in every trace:
after each 64-unit hop `MSCNT` returned to exactly 32 — a hop of 4096
sine-table counts — and the 2026-08-26 probe (+1000 units → MSCNT +512)
fits only the MRES unit.

Fix (`42588a7`): MRES is pinned to 0 (native 256 microsteps) so one unit is
1/256 full step, `DeltaXtarget(divisor) = 256/divisor` is exactly one
configured microstep and the chip interpolates in between — the
datasheet's recommended use of the internal ramp generator. The configured
divisor keeps its meaning as the firmware's step size, so telemetry
positions, BEND distances and pull travel are unchanged.

Verified with the fix, 0.8 A, stealthChop, both motors: `MSCNT` advances
64 per pulse, `STEPPER_MOVE <id> 400` = half a revolution in 2.0 s,
`800` = one revolution in 3 s at 100 full-steps/s, `pwm_scale_sum` 21–33
throughout (was 255), no open-load flags, **no chip resets**. The earlier
"12 V rail sags under 0.8 A" reading was a consequence of the over-speed
demand, not a supply defect — re-check the supply only if `resets` climbs
again at real speeds.

Two more defects found on the way, both fixed:

1. **The pulse thread accelerated per step, not per second**
   (`StepperChannel::PulseThreadBody` fed the ramp a fixed 1 ms per
   iteration while each iteration sleeps one pulse period) — a 5–7 s crawl
   before reaching 100 Hz. Fixed in `d06319c`: the ramp integrates measured
   time; 400 microsteps take 2.0 s.
2. **(Re)initialisation always wrote TOFF=3**, so a disabled motor whose EN
   pin does not reach DRV_ENN (motor 1) was energised at boot and after
   every CHECK. Fixed in `42588a7`: TOFF follows the enabled state.

Units reminder: `STEPPER_MOVE <id> <n>` and `STEPPER_MOVETO` take
**microsteps at the configured divisor** (µ4: 800 = one revolution ≈ 1–2 mm
of pull); `MOTOR_DEBUG` reports the chip's XACTUAL in 1/256 full steps
(51,200 per revolution) and MSCNT in sine-table counts (1024 per 4 full
steps).
