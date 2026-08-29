# Sequent RTD HAT Bench Bring-Up

This is the bench commissioning procedure for the Sequent Microsystems
8-channel RTD HAT, the sole PT100/PT1000 sample-temperature source in the
final schematic v3 BOM. It replaces the retired RTD Click (MAX31865/SPI) and
DAQ-132M (RS485/Modbus) bring-up paths **for temperature**.

This document also carries the bench commissioning gates for the schematic
v3 **MAX31865 dual-click sample-resistance instrument** (sections 9-10) — a
physically separate pair of devices on SPI0, not the RTD HAT's I2C bus, that
measures coating-specimen resistance directly rather than PT100 element
resistance. It is grouped into this document, rather than a separate one,
because it is the sample-resistance measurement path this schematic revision
ships with; for the stepper/SPI bring-up it shares a bus with, see
[TMC5160 Commissioning](tmc5160-commissioning.md).

The register offsets in `onboard/include/coatheal/hal/sequent_rtd_adapter.hpp`
are **derived from vendor source** ([`SequentMicrosystems/rtd-rpi`](https://github.com/SequentMicrosystems/rtd-rpi)),
not measured against a live card. Step 2 below is a blocking gate: do not
trust any reading, and do not proceed to the rest of this document, until it
passes on real hardware.

For wiring, configuration keys, and the rest of the v3 commissioning
sequence, see [Component Configuration and Bring-Up](component-configuration-and-bring-up.md).

## 1. DIP Switch Stack Addressing

The card selects its I2C address with the ID0/ID1/ID2 DIP switches, encoding a
stack level `0..7`. The resulting address is:

```text
address = 0x40 + stack
```

so stack `0` is `0x40`, stack `7` is `0x47`. `sensor.sequent_rtd_stack` in the
onboard config must match the DIP switch setting.

Confirm the card answers at the expected address before anything else:

```bash
i2cdetect -y 1
```

Expect to see the card's address (`0x40` by default, `stack=0`) alongside the
DPS310 (`0x77`) and ADS1115 (`0x48`). If nothing answers, check the ribbon/HAT
seating and 5 V power before suspecting software.

## 2. Register Map Verification (BLOCKING GATE)

**Do not trust any reading from this card, and do not continue past this
section, until this check passes.**

The offsets below are derived the same way the vendor firmware derives them,
not hand-typed, but they have never been checked against a live card:

```cpp
kRtdVal1    = 0    //  0 : 8 x float32 temperatures
kDiagTemp   = 32   // 32 : int8 die temperature
kDiag5V     = 33   // 33 : uint16 LE millivolts
kRevHwMajor = 55
kRevHwMinor = 56
kRevMajor   = 57
kRevMinor   = 58
kRtdRes1    = 59   // 59 : 8 x float32 resistances (NOT 4-byte aligned)
kRtdReinit  = 91   // 91 : uint32 ADC reinit count
kCardType   = 99   // 99 : uint8
kPt1000     = 133  // 133 : uint8, mask 0x0f
```

`kRtdRes1 = 59` is deliberately not 4-byte aligned. That is expected for
byte-addressed memory, and it is exactly the kind of derivation that must be
confirmed rather than assumed.

**Procedure:**

1. Wire a precision 100 Ω resistor across card channel 1 (3-wire, per the
   card's terminal labeling).
2. Stop the onboard service so nothing else is touching the bus:
   `sudo systemctl stop coatheal-onboard.service`.
3. Dump bytes 0-104 from the card's I2C memory (`0x40 + stack`) in one read,
   or in small reads if your tool caps transfer length.
4. Confirm all of the following:

| Check | Expected | Offset |
|---|---|---|
| Channel 1 temperature | float32 ≈ `0.0` | 0 |
| Channel 1 resistance | float32 ≈ `100.0` | 59 |
| Firmware revision bytes | plausible small integers (not `0x00 0x00` or `0xff 0xff`) | 57-58 |
| Card type | `>= 1` | 99 |

   Note: `100.0 Ω` is not an arbitrary reference value — it is `R0`, the
   PT100 standard's defined resistance at `0.0 C`. A precision 100 Ω resistor
   therefore should make *both* checks land together: offset 59 reads ~100 Ω
   because that is what is physically wired, and offset 0 reads ~0.0 C
   because that is what the card's CVD conversion of exactly `R0` produces.
   If one lands correctly and the other does not, suspect a channel
   miscount or an off-by-one in the block offsets, not measurement noise.

5. **Do not proceed if the offsets disagree.** A mismatch means the derived
   map is wrong for the hardware revision in hand, and every downstream
   reading (including validation logic and cross-checks) is unverified.

**Record the observed values here:**

- Observed `card_type` (offset 99): `____` *(fill in at bench)*
- Observed firmware revision (offsets 57-58): `____.____` *(fill in at bench)*
- Offset 59 read with 100 Ω on channel 1: `____ Ω` *(fill in at bench)*
- Gate result: PASS / FAIL *(fill in at bench)*

The vendor source gates sensor-type access on `card_type >= 1` ("available
only for hardware version >= 5.0"); there is no published enumeration of
valid `card_type` values beyond that, so `Probe()` gates on `>= 1` rather than
a specific magic constant. Recording the real value here lets a tighter gate
be added later if the observed hardware always reports a higher number.

## 3. Burst-Read Confirmation

Per poll, the adapter tries one 32-byte burst read at offset 0 (temperatures)
and one at offset 59 (resistances), for a time-coherent snapshot across all
eight channels in a single I2C transaction.

**Procedure:**

1. Read 32 bytes starting at offset 0 in one transaction.
2. Read the same range as eight consecutive 4-byte reads (one per channel).
3. Confirm both produce identical bytes.

If the firmware refuses reads longer than 4 bytes, the burst read fails, and
the adapter automatically falls back to per-channel 4-byte reads and **latches
that mode permanently** for the life of the process — it does not retry
bursts. This is visible in `ComponentSummary` as `sequent_rtd_burst=0` (burst
mode active is `sequent_rtd_burst=1`).

**Record the observed mode here:**

- `sequent_rtd_burst` observed at bench: `____` *(fill in: 1 = burst, 0 = fallback)*

Either mode is a valid, supported outcome — the fallback exists precisely so
firmware that refuses long reads still works. This step is about knowing
which mode the real card lands in, not about requiring burst mode.

## 4. Diagnostic Byte Confirmation

Offsets 32-34 are diagnostics only, never used for control or safety logic:

- Offset 32: `int8`, card die temperature in °C.
- Offsets 33-34: `uint16` little-endian, 5 V rail in millivolts (expect
  something near 5000).

These interpretations are **inferred** from the register map's implied
widths, not confirmed from vendor source the way the temperature/resistance
blocks are. Confirm at the bench:

1. Read offset 32 as a signed byte; it should be a plausible ambient-adjacent
   temperature (roughly room temperature plus a small self-heating offset),
   not a wild value like `-128` or `127`.
2. Read offsets 33-34 as a little-endian `uint16`; it should be close to
   `5000` (5.000 V), not near `0` or overflowing `65535`.
3. Read offset 91 as a little-endian `uint32` (`kRtdReinit`, the card's ADC
   re-initialisation counter). Expect a small, stable number on a card that
   has just powered up. Read it a second time after a minute of steady
   polling: a counter that climbs while nothing is being disturbed means the
   card's ADC keeps resetting itself, which is a hardware/power complaint
   worth chasing before flight even though nothing in flight software reads
   this value.

Sanity criteria, all three diagnostics-only by design:

| Offset | Interpretation | Sane | Suspicious |
|---|---|---|---|
| 32 | `int8` °C (`card_temp_c`) | room temperature plus a few °C of self-heating | `-128`, `127`, or a value that swings tens of degrees between reads |
| 33-34 | `uint16` LE mV (`rail_5v`) | ≈ `4750`..`5250` | near `0`, near `65535`, or drifting under heater load |
| 91 | `uint32` LE (`adc_reinit_count`) | small and unchanging while idle | monotonically climbing during steady polling |

If either interpretation is wrong, the consequence is bounded by design: a
wrong guess here degrades a diagnostic log line (`card_temp_c`, `rail_5v` in
`SequentRtdAdapter::Reading`). It cannot affect a control value — those come
only from the temperature and resistance blocks confirmed as a blocking gate
in step 2.

**Record the observed values here:**

- Offset 32 (die temp, °C) — `card_temp_c`: `____` *(fill in at bench)*
- Offsets 33-34 (rail mV) — `rail_5v`: `____` *(fill in at bench)*
- Offset 91 (`adc_reinit_count`) at power-up: `____` *(fill in at bench)*
- Offset 91 after ~1 min of steady polling: `____` *(fill in at bench)*
- Interpretations confirmed / rejected: `____` *(fill in at bench)*

If any interpretation turns out to be wrong, fix the decode in
`SequentRtdAdapter::ReadAll` and this table together — but do not treat it as
a flight blocker: these three fields feed log lines only, never a control or
safety decision.

## 4b. Address Collision Check (0x41)

`0x40 + stack` overlaps the retired INA3221's default addresses: `0x40` is its
`kDefaultAddrA` and `0x41` its `kDefaultAddrB`
(`onboard/include/coatheal/hal/ina3221_adapter.hpp`). With the RTD card at
stack 0 it occupies `0x40`, and nothing on the flight stack should be
answering at `0x41` at all.

**Procedure** (card at stack 0, onboard service stopped):

```bash
i2cdetect -y 1
```

Confirm all of the following:

1. `0x40` answers — that is the RTD card.
2. **`0x41` does not answer.** Anything replying there is either a second RTD
   card someone set to stack 1, or a real INA3221 that was never removed. Both
   are a wiring problem to resolve before flight: the address space is shared
   and the software has no way to tell the two devices apart.
3. The only other expected addresses are `0x77` (DPS310) and `0x48` (ADS1115).

**Record here:**

- Devices seen on `i2cdetect -y 1`: `____` *(fill in at bench)*
- `0x41` silent? YES / NO *(fill in at bench)*

## 5. Sensor Type Check

Offset 133 encodes the card's configured sensor type, masked with `0x0f`:

- `0` = PT100
- `1` (nonzero) = PT1000

`Probe()` reads this and compares it against `sensor.sequent_rtd_expect_sensor_type`.
**The probe refuses to come up on a mismatch** — this catches a card
configured for PT1000 with PT100 probes wired to it, which would otherwise
read plausibly wrong (finite, in-range, just incorrect) rather than obviously
wrong.

**`pt100` is the only value the config accepts.** `Probe()` handles both, and
the card supports both, but nothing downstream of the probe does: the
card-vs-derived-temperature cross-check in `ApplyValidation` hardcodes the
PT100 Callendar-Van Dusen curve, and the
`sensor.sequent_rtd_resistance_min_ohm`/`_max_ohm` window is a PT100 window
(a PT1000 element sits near 1000 Ω at 0 °C, an order of magnitude outside it).
A `pt1000` config would therefore load, probe successfully, and then mark
every channel invalid on every poll — every heater clamped, no diagnostic
pointing at the cause. `config.cpp` and `scripts/hardware_setup.py` both
reject `pt1000` at load with a message saying exactly that. Wiring PT1000
probes is a code change (CVD curve plus window), not a config change.

The sensor type is also unverifiable, and the probe refuses for that reason
too, whenever `card_type < 1` (hardware older than version 5.0) — offset 133
is only meaningful on hardware new enough to expose it.

Confirm at the bench that offset 133 (masked with `0x0f`) matches your
physically wired sensor type and matches the configured
`sensor.sequent_rtd_expect_sensor_type`.

## 6. Calibration (Bench-Only)

Flight software **never writes to this card**. `SequentRtdAdapter` holds an
`I2cBus`, and `I2cBus` has no write method — there is no code path, by
construction, through which the onboard process could touch a calibration or
configuration register. All calibration happens once, on the bench, through
the vendor `rtd` CLI tool.

Two-point calibration per channel:

```bash
# 1. Short the channel's input (0 Ω reference)
rtd <stack> cal <channel> 0

# 2. Wire a precision 100 Ω resistor to the channel
rtd <stack> cal <channel> 100
```

To restore factory calibration:

```bash
rtd <stack> calrst <channel>
```

Calibration is out of scope for flight software validation — it is a bench
procedure performed with the vendor tool before the card is ever wired into
the flight stack, not something the onboard service participates in.

## 7. Freed Pins

Removing RTD Click frees two GPIO lines that it previously used:

| Pin | Was | Status |
|---|---|---|
| BCM 16 | RTD Click CS | Released, deliberately **unassigned** |
| BCM 25 | RTD Click DRDY | Released, deliberately **unassigned** |

Reassigning these pins to a new purpose is a separate hardware decision, out
of scope for this migration. In schematic v3, SPI0 carries four devices: the
two TMC5160 stepper drivers (`/dev/spidev0.0`, software CS on BCM 22/27) and
the two MAX31865 sample-resistance clicks (hardware CS on BCM 07/08 — see
[TMC5160 Commissioning §4](tmc5160-commissioning.md#4-spi-topology--four-devices-one-bus)
for the full topology). The Sequent RTD card itself is I2C-only and does not
touch SPI0 at all.

## 8. Mission-Envelope Resistance Survey

The shipped plausibility window is `60.0 .. 390.0` Ω. Through the PT100 CVD
curve that spans roughly **−102 °C to +845 °C** — far wider than anything this
payload should ever see, and wider than the sensor range the mission actually
cares about. It is a broken-probe check (open, shorted, miswired), not a
thermal guard; the thermal guard is the `heater.max_sample_temp_c` over-temp
latch at 85 °C. Nothing is wrong with shipping the wide window, but a window
derived from real hardware across the real flight band would catch a drifting
or partially-shorted probe that the wide one waves through.

**Procedure** (per channel, with the probes that will actually fly):

1. Bring the channel to each set point below and let it settle.
2. Record the card's reported resistance (offset 59 block) and its reported
   temperature (offset 0 block) together.
3. Repeat for every channel — probe-to-probe spread is part of what the
   window has to cover.

| Set point | Expected R (PT100 nominal) | Observed R, ch1..ch8 |
|---|---:|---|
| −60 °C (flight-band cold end) | ≈ 76.3 Ω | `____` *(fill in at bench)* |
| −20 °C | ≈ 92.2 Ω | `____` *(fill in at bench)* |
| 0 °C (ice point) | 100.0 Ω | `____` *(fill in at bench)* |
| +25 °C (room) | ≈ 109.7 Ω | `____` *(fill in at bench)* |
| +80 °C (flight-band hot end) | ≈ 130.9 Ω | `____` *(fill in at bench)* |

**Derive and record the replacement window:**

- Lowest observed resistance across all channels: `____ Ω` *(fill in at bench)*
- Highest observed resistance across all channels: `____ Ω` *(fill in at bench)*
- Proposed `sensor.sequent_rtd_resistance_min_ohm`: `____` *(fill in at bench)*
- Proposed `sensor.sequent_rtd_resistance_max_ohm`: `____` *(fill in at bench)*

Leave headroom on both ends for probe tolerance and lead resistance — the
window's job is to reject broken hardware, not to second-guess a cold sample.
Until this survey is done, the `60.0 .. 390.0` defaults stand.

## 9. MAX31865 Sample-Resistance Click Bring-Up (BLOCKING GATES)

Two MikroE RTD Click boards (MAX31865), one per coating specimen, sit on
SPI0's **native** chip-selects — CE1 (BCM 07, `/dev/spidev0.1`) is click 1 /
SAMPLE1; CE0 (BCM 08, `/dev/spidev0.0`) is click 2 / SAMPLE2. Unlike the two
TMC5160 motor drivers sharing this same bus, the clicks use the kernel's
normal hardware chip-select — no `SPI_NO_CS`, no software CS GPIO. See
[TMC5160 Commissioning §4](tmc5160-commissioning.md#4-spi-topology--four-devices-one-bus)
for the full four-device SPI0 topology and why the motors, not the clicks,
need the software-CS treatment.

Conversion: `R = code * R_ref / 32768`. Saturation (a fault bit set, or the
ADC code sitting near full scale) is a **first-class result, not an error to
paper over** — the adapter reports `valid=false, out_of_range=true` rather
than a plausible-looking number, specifically so gate 5 below can
characterise a coating resistance range nobody has measured yet without the
software inventing a number for it.

### Gate 4 — Click Reference Resistor Value (BLOCKING)

`sensor.max31865_reference_ohm` defaults to `470.0` Ω — the MikroE RTD Click's
nominal reference resistor value. The retired pre-migration code assumed
`400` Ω instead. **Do not trust any resistance reading from either click
until the populated reference resistor value is confirmed against the actual
board**, not the nominal datasheet/BOM value:

1. With the clicks unpowered and unplugged from the Pi, read the reference
   resistor's marked value directly off each board (component near the RTD
   terminal block, usually labelled `R_REF` or similar in the MikroE
   silkscreen).
2. Confirm both clicks use the same populated value — a board with a
   different reference resistor populated than its sibling would need a
   value **per channel**, which the current single
   `sensor.max31865_reference_ohm` key cannot express (a config-level
   limitation to flag if this happens, not a bench workaround to invent).
3. Set `sensor.max31865_reference_ohm` to the confirmed value.

**Record here:**

- Click 1 (SAMPLE1) reference resistor, read off the board: `____ Ω`
  *(fill in at bench — expected 470, confirm against the actual part)*
- Click 2 (SAMPLE2) reference resistor, read off the board: `____ Ω`
  *(fill in at bench)*
- Both clicks match: YES / NO *(fill in at bench)*
- `sensor.max31865_reference_ohm` set to: `____`
- Gate 4 result: PASS / FAIL *(fill in at bench)*

### Gate 5 — Coating Resistance Range Characterisation (BLOCKING)

The coating specimens' resistance range is **unknown and untested** — this is
stated as ground truth in the schematic v3 design, not an oversight. Nothing
in software assumes a plausibility window for this measurement the way the
RTD HAT's `sequent_rtd_resistance_min_ohm`/`_max_ohm` window does for PT100
element resistance; the instrument is required to measure and report,
including reporting "out of range," rather than assume a window in advance.

1. With both clicks wired 4-wire Kelvin to their specimens (SAMPLE1 -> click
   1, SAMPLE2 -> click 2 — do not cross them), read resistance via
   `printf 'CHECK MAX31865\n' | nc 127.0.0.1 5000` or the telemetry
   `RESISTANCE=` field (see [Configuration Reference](configuration.md) for
   `sensor.max31865_sample_indices`, which selects which two of the eight
   wire slots the clicks fill).
2. Record the resistance across the specimen(s) actually wired at the bench,
   across whatever range of specimen states (as-deposited, after thermal
   cycling, after mechanical pull) is practical to exercise during
   commissioning.
3. If a reading saturates (`valid=false, out_of_range=true` at the adapter
   level; on the wire this shows as the channel's slot **not** advancing from
   its `-`/last-good value while `resistance_ok` on `CHECK MAX31865` stays
   healthy — saturation is a valid measurement of an out-of-range specimen,
   not a bus failure), record that the specimen exceeded the instrument's
   measurable range at `R_ref`, and roughly how far past it saturated if
   determinable (e.g. by temporarily raising `sensor.max31865_reference_ohm`
   on the bench to shift the measurable window, then restoring it before
   flight).
4. This procedure is explicitly bench-only characterisation, not a software
   change — the plan's open "untested-range question" is answered by what
   gets recorded here, not by picking a plausibility window in code.

**Record here:**

- SAMPLE1 resistance observed (or "saturated"), by specimen state: `____`
  *(fill in at bench)*
- SAMPLE2 resistance observed (or "saturated"), by specimen state: `____`
  *(fill in at bench)*
- Any saturation observed: YES / NO *(fill in at bench)*
- If YES, approximate range past which saturation begins: `____`
  *(fill in at bench, if determinable)*
- Gate 5 result: PASS / FAIL *(fill in at bench — FAIL only if the clicks
  cannot produce a valid reading on a specimen known to be in a sane state,
  not merely because the range turns out to be wide or saturates easily)*

## 10. Sample-Index Mapping (`max31865_sample_indices`) — Placeholder, Fill In At Bench

`sensor.max31865_sample_indices` defaults to `0,4` — entry 0 feeds click 1 /
SAMPLE1, entry 1 feeds click 2 / SAMPLE2. **This default is an
owner-flagged placeholder**: `0,4` is simply "the first sample index of each
motor's group" (`motor0.samples` starts at 0, `motor1.samples` starts at 4),
chosen because *some* valid default was needed at config-load time, not
because sample 0 and sample 4 are confirmed to be where the two click-wired
specimens physically live.

Determining the real mapping is a bench/integration task, not a software
task — record which physical specimen positions SAMPLE1 and SAMPLE2 actually
correspond to once the clicks are wired into the finished mechanism, then set
this key to match. The two entries must be distinct and each less than
`hardware.sample_count` (validated at config load); nothing else constrains
them.

**Record here:**

- Physical specimen position SAMPLE1 (click 1, CE1) actually measures, as a
  software sample index: `____` *(fill in at bench/integration —
  placeholder default is `0`)*
- Physical specimen position SAMPLE2 (click 2, CE0) actually measures, as a
  software sample index: `____` *(fill in at bench/integration —
  placeholder default is `4`)*
- Config line to set once confirmed:
  `sensor.max31865_sample_indices=____,____ ` *(fill in at bench)*

## Useful Operator Commands

Read-only I2C presence probe (reads the firmware revision byte at offset 57,
the same register `doBoardInit` reads upstream):

```bash
python3 scripts/spi_probe.py --rtd-stack 0
```

Pass `--skip-rtd` to omit the RTD presence check when probing only the
TMC5160 SPI devices (`spi_probe.py` does not currently probe the MAX31865
clicks; use `CHECK MAX31865` below for that).

Active health check through the onboard command server:

```bash
python3 scripts/hardware_setup.py doctor --config config/onboard.local.ini
python3 scripts/hardware_setup.py rtd-check
```

`rtd-check` sends `CHECK SEQUENT_RTD` and requires both `overall=OK` and a
`sequent_rtd=OK` token in the response. The token's case is not stable — a
real card produces lowercase `sequent_rtd=OK`, a simulated build echoes the
requested selector and produces `SEQUENT_RTD=OK;simulated=1` — so the match is
case-insensitive, and `doctor` applies the same rule. `doctor` runs this
alongside `COMPONENTS`,
`CHECK PWM`, `CHECK MOTOR0`, and `CHECK MOTOR1`.

Direct command-link check:

```bash
printf 'CHECK SEQUENT_RTD\n' | nc 127.0.0.1 5000
```

`DAQ132M` and `RTD_CLICK` are still accepted on the wire as legacy aliases for
`SEQUENT_RTD` (see [Wire Protocol](protocol.md)), but the reply always reports
`sequent_rtd=OK`/`sequent_rtd=FAILED`, never `rtd_click=OK` or `daq132m=OK`.
Note that `RTD_CLICK` selects the *retired* MAX31865 temperature path alias
for `SEQUENT_RTD` — it is unrelated to the current, unrelated-purpose
`MAX31865` selector below, which reads the two v3 sample-resistance clicks.

### Per-channel harness diagnosis (`sequent_rtd_ch`)

`COMPONENTS` and `CHECK SEQUENT_RTD` both carry a per-channel diagnosis
(added 2026-08-29 after the bench read `DEGRADED` with zero usable
channels):

```text
sequent_rtd_valid=0/8;sequent_rtd_ch=S0:ch1:OPEN:-366.0|S1:ch2:OPEN:366.0|S2:ch3:SHORT:0.2|...
```

Each entry is `S<sample>:ch<card channel>:<FAULT>:<ohms>` — the `ch<n>`
number matches the HAT's terminal blocks. Fault classes:

| Fault | Meaning | Typical cause |
|---|---|---|
| `OK` | Resistance in the plausible window and consistent with the card's own temperature | — |
| `OPEN` | The card's ±366.000 Ω full-scale sentinel, an above-window reading, or a non-finite value | No probe on the terminals, broken lead, loose screw terminal |
| `SHORT` | Below the plausible window (a PT100 never reads under ~80 Ω in the mission envelope) | Shorted leads, probe wired across the wrong terminals |
| `MISMATCH` | In-window resistance whose PT100-derived temperature disagrees with the card's reading by more than `sensor.sequent_rtd_crosscheck_tol_c` | 2-wire probe on 3-wire terminals (or vice versa), wrong sensor type, drifting probe |

Component-state semantics: the card polls fine but **zero** channels
validate → `sequent_rtd=FAILED` (`NO_VALID_CHANNELS`); **some** validate →
`DEGRADED` (`PARTIAL_CHANNELS`); all eight → `OK`. `CHECK SEQUENT_RTD`'s
`sequent_rtd=OK` verdict remains a *bus/instrument conversation* check —
read `sequent_rtd_valid=` in the same reply for probe health.

The onboard also journals the diagnosis whenever the fault pattern changes
(at most once a minute), so `journalctl -fu coatheal-onboard` shows each
channel flip to `OK` live while the harness is being re-terminated.

Bench state 2026-08-29: all 8 channels bad — `ch3` SHORT (0.2 Ω), the
other seven OPEN (±366 Ω). A real PT100 reads ~109 Ω at room temperature.
This is probe-harness wiring, not card or software.

MAX31865 sample-resistance click check:

```bash
printf 'CHECK MAX31865\n' | nc 127.0.0.1 5000
```

Expect `max31865_1=OK;max31865_1_error=NONE;max31865_2=OK;max31865_2_error=NONE`
in the reply (real hardware) alongside `overall=OK`. A saturated-but-healthy
specimen still reports `max31865_1=OK`/`max31865_2=OK` — saturation is a
valid measurement outcome, not a check failure (gate 5 above). `CHECK
MAX31865` performs a real one-shot conversion on each click, the same as
`CHECK SEQUENT_RTD` performs a real card read — neither is a cached-health
readback.

## Configuration Reference

```ini
sensor.sequent_rtd_stack=0                   # 0..7 -> I2C 0x40..0x47
sensor.sequent_rtd_channels=1,2,3,4,5,6,7,8  # card channel per logical sample
sensor.sequent_rtd_poll_ms=1000
sensor.sequent_rtd_expect_sensor_type=pt100
sensor.sequent_rtd_resistance_min_ohm=60.0
sensor.sequent_rtd_resistance_max_ohm=390.0
sensor.sequent_rtd_crosscheck_tol_c=2.0

# MAX31865 dual-click sample-resistance instrument (schematic v3, section 9-10
# above). Device paths are fixed by hardware (CE1/GP07 = click 1 = SAMPLE1 =
# /dev/spidev0.1; CE0/GP08 = click 2 = SAMPLE2 = /dev/spidev0.0), not
# configurable.
sensor.max31865_reference_ohm=470.0
sensor.max31865_poll_ms=1000
sensor.max31865_sample_indices=0,4           # OWNER-FLAGGED PLACEHOLDER, see section 10

# max31865_click is the v3-shipped default -- coating/specimen resistance
# from the two clicks. sequent_rtd (PT100 element resistance from the RTD
# HAT above), disabled (`-` on the wire), and simulated remain accepted.
sensor.resistance_source=max31865_click
```

`sensor.sequent_rtd_expect_sensor_type` accepts `pt100` only — see section 5
for why `pt1000` is rejected at load rather than accepted and silently broken.

The `60.0 .. 390.0` Ω window above maps through the PT100 CVD curve to roughly
−102 °C to +845 °C, far wider than the mission envelope; the real thermal
guard is the `heater.max_sample_temp_c` over-temp latch at 85 °C. Section 8's
bench survey is expected to narrow it. This window applies only to the RTD
HAT's own `sequent_rtd` resistance path — the MAX31865 click instrument
deliberately has no equivalent plausibility window (section 9, gate 5): its
job is to measure and report an unknown coating-resistance range, not assume
one.

See [Configuration Reference](configuration.md) for the full key list and
validation rules.

## Sign-Off

Do not consider the register map trustworthy, and do not rely on this card
for flight thermal control, until every blank in section 2 (and ideally
sections 3-5) is filled in with a bench observation, not an assumption.

- [ ] Section 2 (register map) gate passed and values recorded.
- [ ] Section 3 (burst mode) observed and recorded.
- [ ] Section 4 (diagnostic bytes, including `adc_reinit_count`) observed and
      recorded.
- [ ] Section 4b (address collision) confirmed: `0x40` answers, `0x41` silent.
- [ ] Section 5 (sensor type) confirmed against physical wiring.
- [ ] Section 6 (calibration) performed if the card ships uncalibrated.
- [ ] Section 8 (resistance survey) completed and a mission-envelope window
      proposed. Not a flight blocker on its own — the shipped `60.0 .. 390.0`
      window is safe, just loose — but the survey is the only thing that can
      tighten it.
- [ ] Section 9, gate 4 (MAX31865 reference resistor value) PASSED and
      `sensor.max31865_reference_ohm` set to the confirmed value on both
      clicks.
- [ ] Section 9, gate 5 (coating resistance range characterisation) PASSED
      and observations recorded, including any saturation behaviour.
- [ ] Section 10 (`max31865_sample_indices` physical mapping) confirmed
      against the real specimen wiring and `sensor.max31865_sample_indices`
      updated from the `0,4` placeholder if the mapping differs.
