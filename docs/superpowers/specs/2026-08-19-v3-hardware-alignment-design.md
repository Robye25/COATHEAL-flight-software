# Schematic v3 Hardware Alignment — Design

**Date:** 2026-08-19
**Status:** Approved for planning
**Authority:** `COATHEAL_ElectronicsSchematic_v3.pdf` (final electronics wiring, confirmed by project owner).
Supersedes the pin map and stepper/resistance assumptions the repo inherited from earlier revisions.
Builds on `2026-08-17-sequent-rtd-migration-design.md` (unchanged: the Sequent HAT remains the sole
sample-temperature source).

---

## 1. Ground truth from schematic v3 (owner-confirmed)

- **Steppers:** two QHV5160 v2 boards (TMC5160). `REFL/STEP`, `REFR/DIR`, `DIAG0/1`, encoder pins
  all unconnected. **Motion is SPI-only via the 5160's internal ramp generator. STEP/DIR will never
  be wired.** CS: DRV1=GP22, DRV2=GP27 (schematic's DRV2 "CS_DRV1" label was a typo, fixed by owner).
  EN: DRV1=GP20, DRV2=GP21. VM=+12 V, VIO=+3V3.
- **Sample-resistance instrument:** two MikroE RTD Click boards (MAX31865) measuring the coating
  specimens' own resistance via 4-wire Kelvin connections (SAMPLE1, SAMPLE2 are 2-terminal specimens).
  DRDY unconnected on both. CS wiring crosses the numbering: **CE1 (GP07) = RTD1/SAMPLE1 →
  `/dev/spidev0.1`; CE0 (GP08) = RTD2/SAMPLE2 → `/dev/spidev0.0`.** Expected coating resistance
  range: **unknown, untested** — the software must measure and report rather than assume.
- **Heaters:** six 5 W @ 14.4 V polyimide heaters, low-side-switched by two Electrokit EKM014 boards
  (VL=+5 V, VH=+14.4 V), active-high inputs. GPIO map: **H1=GP19, H2=GP13, H3=GP06, H4=GP05,
  H5=GP24, H6=GP23.** No hardware PWM available on this set as a whole → software PWM (already the
  code's mechanism). **Owner requirement: at most 3 heaters active simultaneously.**
- **Temperatures:** 8 PT100s on the Sequent HAT's terminals, channels 1–8 = logical samples 0–7,
  heaters on samples 0–5, samples 6–7 unheated. (Matches existing code exactly; no change.)
- **Buses:** I2C = `/dev/i2c-1` (GP02/03): Sequent HAT 0x40, DPS310 0x77, ADS1115 0x48 (dedicated to
  the GUVA-S12D UV sensor on A0; no other ADC exists). SPI = **SPI0** (GP10/09/11) — the owner's
  "spi1" remark was a naming slip; the schematic's pins are authoritative and GP19–21 (where SPI1
  would live) carry heater/enable nets.
- **Reserved by the Sequent HAT** (stacked): GP14/15 (UART↔RS485), GP17 (RSDIR), GP26 (INTN).
  Unused by flight software but never assignable to anything else.
- **Power:** 28 V gondola → 12 V (steppers), 14.4 V (heaters), 5 V (Pi + MOSFET logic). Comms via
  Ethernet.

### Why the current code cannot run on this hardware

The repo's pin map predates v3. As-is: heater PWM on GP17/27 would fight the HAT's RS485-DIR and
DRV2's chip select; stepper STEP pulses on GP19/24 would **pulse heaters H1/H5**; motor1's CS on
GP23 would chip-select through heater H6's MOSFET input. The TMC2240 driver's probe
(IOIN VERSION==0x40) fails against a TMC5160 (0x30), so motors would never enable — but the GPIO
hazards above happen before any driver probe. Package 1 is therefore a precondition for ever
powering the board with flight code.

---

## 2. Package 1 — Pin map, power policy, PWM (bounded)

Config-level only; no new drivers.

| Key | Old | New |
|---|---|---|
| `heater.output_lines` | 17,18,27,5,6,13 | **19,13,6,5,24,23** |
| `motor0.cs_line` / `enable_line` | 22 / 12 | 22 / **20** |
| `motor1.cs_line` / `enable_line` | 23 / 21 | **27** / 21 |
| `motor0/1.step_line`, `dir_line` | 19/26, 24/20 | **removed** (keys rejected; `migrate_config` drops them) |
| `motorN.driver` | `tmc2240` | **`tmc5160`** (sole accepted value; `tmc2240` rejected with "retired") |
| `power.max_active_heaters` | 4 | **3** |
| `power.max_thermal_w` | 20.0 | **15.0** |
| `heater.pwm_frequency_hz` default | 10.0 | **1.0** (software PWM; 5 W heaters have high thermal inertia) |

Additional rules:
- The GPIO-collision validator gains **named reserved claims** for GP14, GP15, GP17, GP26
  ("sequent_rtd_hat uart/rsdir/intn") so any future assignment collides loudly at config load.
- GP07/GP08 (CE1/CE0) reserved as "spi0 chip-selects (max31865)".
- `scripts/hardware_setup.py` `FINAL_PIN_VALUES`, validators, and both INIs updated to match;
  `spi_probe.py`'s TMC2240 IOIN check becomes a TMC5160 check (expected VERSION **0x30**).
- Docs: hardware.md pin tables, configuration.md, bring-up guide.

Power arithmetic recorded: 3 × 5 W = 15 W thermal ceiling; per-heater nominal 5 W stays truthful at
the 14.4 V rail (owner-confirmed rating). `energy_budget_wh` semantics unchanged.

## 3. Package 2 — TMC5160 SPI-motion driver (architectural)

**Decision: position-dribble behind the existing pulse-level seam**, not a motion-level rewrite.

`StepperDriver` (`hal/stepper_driver.hpp`) is pulse-level: the channel's pacing thread calls
`Step(direction)` once per microstep and owns speed/accel/position/pull-cycle logic, `MotionLock`,
and heater inhibit. All of that is flight-reviewed. A `Tmc5160Driver : StepperDriver` maps each
`Step()` to an XTARGET increment of one configured microstep; the 5160's ramp generator (RAMPMODE=0,
generous VMAX/AMAX so it always keeps up with ≤`max_step_hz`=100 Hz dribble) smooths execution.
Position truth stays in software (as today, pulses == position); XACTUAL is read for health
cross-check only. The alternative — letting the 5160 own motion profiles via XTARGET moves — would
rewrite `stepper_channel`/`stepper_controller` wholesale for no mission benefit at 100 Hz rates.

Driver contract:
- `Enable(bool)`: EN GPIO (active-low `DRV_ENN` assumed, `enable_active_low=true`, bench-confirmed)
  + TOFF gating in CHOPCONF.
- Probe/`ActiveCheck`: IOIN `VERSION == 0x30`, register write-readback (GCONF, CHOPCONF,
  IHOLD_IRUN, GLOBALSCALER), ramp-generator sanity (XACTUAL readable).
- `SetMicrostep(divisor)`: MRES field; the Step→ΔXTARGET scale factor **is a bench-gated constant**
  (datasheet-derived at implementation; verified by the one-revolution test: `steps_per_rev ×
  microstep` calls = exactly one shaft turn).
- Init also zeros XACTUAL/XTARGET, sets run/hold currents from `run_current_a_rms` /
  `hold_current_frac` via GLOBALSCALER+IRUN (same current model the TMC2240 code used).
- On `Enable(false)`: freeze by XTARGET=XACTUAL before dropping TOFF/EN (no coast-past).

**SPI transport:** new `SpiBus` seam (mirroring `I2cBus`: `Open(device, mode, hz)`,
`Transfer(tx, rx, len)`, `Close()`; production `LinuxSpiBus`, test `FakeSpiBus` with a scripted
transfer log). **Both packages 2 and 3 use this one seam.** `LinuxSpiBus` sets **`SPI_NO_CS`** when
the caller does soft CS (motors, GPIO 22/27): the kernel otherwise asserts CE0/CE1 — physically
wired to the clicks — on every spidev transfer. This is mandatory, not an optimisation.
`spi_bus_lock` continues to serialise the shared bus.

Retired: `tmc2240_driver.{hpp,cpp}`, `GpioStepDirStepperDriver`, `gpio_step_dir_driver.cpp`, the
pulse-jitter bench's GPIO expectations reviewed (bench measures pacing-thread jitter and remains
meaningful), `docs/tmc2240-*` superseded by a TMC5160 commissioning doc.

## 4. Package 3 — MAX31865 sample-resistance instrument (architectural)

Clean-room driver (not a paste-back of the pre-migration code) in the Task-1 style:
`hal/max31865_adapter.{hpp,cpp}` over the shared `SpiBus` seam, unit-tested via `FakeSpiBus`.

- Two channels on **native CE** (`spidev0.1`→SAMPLE1, `spidev0.0`→SAMPLE2; kernel CS framing is
  correct here, no `SPI_NO_CS`). SPI mode 1, ≤1 MHz.
- 4-wire configuration; one-shot conversions with bias enabled only around the conversion window
  (limits self-heating current through the specimen).
- Conversion: `R = code × R_ref / 32768`. `R_ref` configurable, **default 470.0 Ω** (MikroE RTD
  Click nominal; old repo assumed 400 — bench gate confirms the populated value).
- **Saturation/out-of-range is a first-class output:** full-scale ADC code or MAX31865 fault bits →
  channel invalid + health `DEGRADED`/`OUT_OF_RANGE`, never a plausible-looking number. This is what
  lets the bench characterise the untested coating range safely.
- `SensorManager` gains a `Max31865Loop` worker (same cache/health pattern as `SequentRtdLoop`)
  filling `sample_resistance_ohm_` for the two monitored sample indices; unmonitored samples emit
  the existing `-` convention on the wire.
- Config: `sensor.resistance_source` gains **`max31865_click` (new default)**; `sequent_rtd`,
  `disabled`, `simulated` remain accepted. New keys: `sensor.max31865_reference_ohm` (470.0),
  `sensor.max31865_poll_ms` (1000), `sensor.max31865_sample_indices` (two entries, **default `0,4`**
  = first sample of each motor's group — placeholder until integration fixes the physical mapping;
  validated: two distinct entries in [0,8)).
- **No new telemetry wire fields or tokens.** Click health drives the existing `RESISTANCE_OK`
  STATUS bit and `ComponentSummary`/`CHECK` diagnostics. (A third `COMPONENT_STATE` break for a
  2-channel instrument is not worth another deploy-together constraint.)
- `NotePullCompleted`'s simulated decay remains the `simulated` path; the `owns_resistance` guard
  generalises: the worker writes only when `resistance_source == "max31865_click"`.

## 5. Cross-cutting

- **SPI topology (final):** SPI0 carries four devices — clicks on hardware CE0/CE1, TMC5160s on
  soft CS GP22/GP27 with `SPI_NO_CS`. All transfers serialise through `spi_bus_lock`.
- Mode/speed per device: MAX31865 mode 1 ≤1 MHz; TMC5160 mode 3 (SPI mode 3 per datasheet) ≤4 MHz
  (config `spi_speed_hz` stays).
- Telemetry: no wire-format changes in any package.
- The Sequent HAT temperature path is untouched by all three packages.

## 6. Bench gates (extend `docs/sequent-rtd-bring-up.md` or sibling commissioning doc)

1. TMC5160 IOIN VERSION reads 0x30 on both drivers; EN polarity confirmed (motor de-energised at
   boot).
2. One-revolution test fixes the Step→ΔXTARGET scale; direction sign matches `invert_direction`.
3. `SPI_NO_CS` verified: motor SPI traffic with clicks connected produces no click reads/faults.
4. Click reference resistor value read off the boards (470 vs 400 Ω) and set in config.
5. Coating resistance measured across specimens; if it exceeds ~R_ref the instrument saturates —
   this characterises requirement "range unknown".
6. Heater map walk: energise H1..H6 one at a time at low duty, confirm GPIO↔heater↔sample mapping.
7. Max-3-heaters ceiling observed under load (scheduler behaviour on real rails).

## 7. Out of scope

- The HAT's RS485/Modbus wiring (GP14/15/17): present, reserved, unused by flight software.
- Encoder/StallGuard features of the TMC5160.
- Any change to the Sequent RTD temperature path, wire protocol, or ground station.
- Physical characterisation itself (the software only enables it).
