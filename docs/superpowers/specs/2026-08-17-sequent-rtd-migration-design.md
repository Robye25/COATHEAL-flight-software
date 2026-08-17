# Sequent Microsystems RTD HAT Migration — Design

**Date:** 2026-08-17
**Status:** Approved for planning
**Scope:** Replace both existing sample-temperature acquisition paths with the
Sequent Microsystems RTD Data Acquisition 8-Layer Stackable HAT.

---

## 1. Motivation and decisions

The flight software currently selects a sample-temperature source at runtime via
`sensor.sample_temperature_source`, with two implementations:

| Source | Bus | Real channels |
|---|---|---|
| `rtd_click_max31865` (default) | SPI `/dev/spidev0.0` + GPIO CS/DRDY | 1, fanned into an 8-slot array |
| `daq132m_modbus` | RS485 / Modbus RTU over serial | 8 |

The project has selected the [Sequent Microsystems RTD Data Acquisition
card](https://sequentmicrosystems.com/products/rtd-data-acquisition-card-for-rpi)
as the flight sensor. It provides eight independent PT100/PT1000 channels over
I2C on a single stackable HAT.

Decisions taken during design (each explicitly chosen, not defaulted):

1. **Sole source.** The Sequent card replaces *both* existing paths. The
   MAX31865/RTD Click and DAQ-132M Modbus implementations are deleted, along
   with their config keys, validation, documentation, and tests.
2. **One card, configurable stack and channel map.** A single card, but the
   stack level and the card-channel-to-logical-sample mapping are configurable
   so a miswired or dead channel can be remapped in the field without a rebuild.
   Multi-card aggregation is explicitly out of scope.
3. **Read both temperature and resistance.** Temperature drives control and
   safety; raw resistance is read alongside and used for plausibility and
   open/short detection.
4. **Bench-only calibration.** Flight software never writes calibration
   registers. Calibration is performed once on the bench with the vendor `rtd`
   CLI; the software reads back card identity and firmware revision so an
   uncalibrated or swapped card is visible in diagnostics.
5. **Card watchdog left untouched.** No writes to any `I2C_MEM_WDT_*` register.
   Recovery is handled in software by the existing `ComponentHealth` /
   `stale_after_ms` machinery and worker retry.
6. **Extracted HAL driver.** The register conversation lives in its own adapter
   behind a transport seam so it is unit-testable without hardware.

### Consequences beyond the sensor itself

- RTD acquisition leaves the SPI bus, removing it as a contender for the
  `spi_bus_lock` shared with the TMC2240 stepper drivers.
- GPIO 16 (RTD Click CS) and GPIO 25 (RTD Click DRDY) are freed. This design
  documents them as available but deliberately does **not** reassign them;
  that is a separate hardware decision.
- The `COMPONENT_STATE` telemetry field changes. See §7.

---

## 2. Hardware reference

Verified against vendor sources
([`SequentMicrosystems/rtd-rpi`](https://github.com/SequentMicrosystems/rtd-rpi)),
not from marketing copy.

- 8 channels, PT100 (−200…765 °C) or PT1000 (−50…500 °C), 3-wire.
- 24-bit delta-sigma ADC per channel; 0.1 % factory accuracy, 0.01 % after
  two-point field calibration; max ~40 conversions/second.
- I2C, address `SLAVE_OWN_ADDRESS_BASE + stack` where
  `SLAVE_OWN_ADDRESS_BASE = 0x40` and `stack ∈ [0,7]`, i.e. `0x40`…`0x47`.
- 5 V, 50 mA. Sensor type is selected in software; no jumpers.
- Presence is proven exactly as `doBoardInit` does: read
  `REVISION_MAJOR_MEM_ADD` and require success.
- Temperature conversion, from `rtdChGet`: a plain `memcpy` of four bytes into a
  `float` at `RTD_VAL1_ADD + sizeof(float) * (channel - 1)`. Native-endian
  float32, **no scaling divisor**, channels 1-indexed.

### Address-space hazard

`Ina3221Adapter::kDefaultAddrA` and `kDefaultAddrB` are `0x40` and `0x41` —
precisely the Sequent stack-0 and stack-1 addresses. `Ina3221Adapter::ReadChannel`
is a stub returning `false` and the final BOM sets `resistance_source=disabled`,
so nothing currently answers at those addresses and there is no live conflict.
The collision must be recorded in code, not left as tribal knowledge, because
re-enabling the INA3221 later would silently contend with the RTD card.

Live I2C devices after this change: DPS310 `0x77`, ADS1115 `0x48`,
Sequent RTD `0x40 + stack`.

---

## 3. Module boundary

Two new files:

```
onboard/include/coatheal/hal/sequent_rtd_adapter.hpp
onboard/src/hal/sequent_rtd_adapter.cpp
```

The adapter is injected into `SensorManager` the same way `RtcAdapter` and
`Ina3221Adapter` already are.

```cpp
namespace coatheal {

// Minimal transport seam. LinuxI2cBus is the production implementation,
// lifted from the existing free functions in sensor_manager.cpp.
// FakeI2cBus in tests serves canned register images.
class I2cBus {
 public:
  virtual ~I2cBus() = default;
  virtual bool Open(int address) = 0;
  virtual bool ReadRegisters(std::uint8_t reg, std::uint8_t* data,
                             std::size_t size) = 0;
  virtual void Close() = 0;
};

// 8-channel PT100/PT1000 acquisition on a Sequent Microsystems
// stackable RTD HAT. Byte-addressed I2C memory at 0x40 + stack.
class SequentRtdAdapter {
 public:
  static constexpr std::size_t kChannelCount = 8;
  static constexpr int kAddressBase = 0x40;
  static constexpr int kStackMin = 0;
  static constexpr int kStackMax = 7;

  struct Identity {
    std::uint8_t card_type = 0;
    std::uint8_t fw_major = 0;
    std::uint8_t fw_minor = 0;
    std::uint8_t hw_major = 0;
    std::uint8_t hw_minor = 0;
    bool pt1000 = false;      // decoded from I2C_SENSORS_TYPE / I2C_MEM_PT1000
  };

  struct Reading {
    std::array<double, kChannelCount> temperature_c{};
    std::array<double, kChannelCount> resistance_ohm{};
    std::array<bool, kChannelCount> channel_valid{};
    double card_temp_c = 0.0;
    double rail_5v = 0.0;
    std::uint32_t adc_reinit_count = 0;
  };

  // Deliberately not SensorHardwareConfig: the HAL must not depend on the
  // application config struct, or the seam buys nothing.
  struct Options {
    int stack = 0;
    bool expect_pt1000 = false;
    double resistance_min_ohm = 60.0;
    double resistance_max_ohm = 390.0;
    double crosscheck_tol_c = 2.0;
    std::array<std::uint8_t, kChannelCount> channel_map{1, 2, 3, 4,
                                                        5, 6, 7, 8};
  };

  SequentRtdAdapter(I2cBus* bus, const Options& options);

  bool Probe(Identity* out, std::string* error);
  bool ReadAll(Reading* out, std::string* error);

  bool healthy() const;
  bool burst_mode() const;   // false once fallback has engaged
};

}  // namespace coatheal
```

`SensorManager` retains only a `SequentRtdLoop()` polling worker plus the cache
and health bookkeeping, matching the existing `DpsLoop` / `AdsLoop` shape.

**Rationale for the seam.** The current MAX31865 path is testable only through
the static `Max31865CodeToResistance` helper; its actual register conversation
has no test at all. With eight float32 channels, a second float32 resistance
block, a 1-indexed channel map, and stack-address arithmetic, an untestable read
path is where defects will hide. The seam lets all of that be exercised on a
developer machine with no Pi attached.

---

## 4. Register map

The vendor `enum` is mirrored as a `constexpr` chain so offsets are *derived the
same way upstream derives them* rather than hand-typed as magic numbers. A
firmware map change then becomes a one-line edit instead of an audit.

```cpp
inline constexpr int kRtdVal1    = 0;                 //  0 : 8 x float32
inline constexpr int kDiagTemp   = kRtdVal1 + 8 * 4;  // 32 : card die temp
inline constexpr int kDiag5V     = kDiagTemp + 1;     // 33 : 5V rail
// bytes 35..54 are the I2C_MEM_WDT_* block - deliberately never touched
inline constexpr int kRevHwMajor = 55;
inline constexpr int kRevHwMinor = 56;
inline constexpr int kRevMajor   = 57;
inline constexpr int kRevMinor   = 58;
inline constexpr int kRtdRes1    = 59;                // 59 : 8 x float32
inline constexpr int kRtdReinit  = kRtdRes1 + 8 * 4;  // 91
inline constexpr int kCardType   = 99;
inline constexpr int kSensorType = 130;
inline constexpr int kPt1000     = 133;
```

These values are **derived from vendor source, not measured**. `kRtdRes1 = 59`
is notably not 4-byte aligned — legal for byte-addressed memory, and precisely
the sort of derivation that must be confirmed against a live card before it is
trusted. See §9.

---

## 5. Data path

Per poll the worker performs two 32-byte bursts — temperatures at `kRtdVal1`,
resistances at `kRtdRes1` — plus a short read covering the diagnostic bytes.
One burst per block yields a time-coherent snapshot across channels, which
matters because the thermal controller compares channels against one another
for the uniformity check.

If the firmware does not honour reads longer than four bytes, the adapter falls
back automatically to eight per-channel 4-byte reads, latches that mode, and
reports it through `burst_mode()` so the degradation is visible in diagnostics
rather than silent.

`SensorManager` reuses the existing `ReadI2cRegisters` semantics (write register
pointer, then read *n* bytes against `/dev/i2c-1`), which is the same shape as
the vendor's `i2cMem8Read`.

### Per-channel validation

Applied in order; the first failure marks the channel invalid:

1. Temperature and resistance are both finite (reject NaN and infinity).
2. Resistance lies within
   `[sequent_rtd_resistance_min_ohm, sequent_rtd_resistance_max_ohm]`.
   An open sensor reads far high; a short reads near zero.
3. Card-reported temperature agrees with
   `Pt100TemperatureFromResistance(resistance)` to within
   `sequent_rtd_crosscheck_tol_c`.

Step 3 is what earns the "read both" decision its keep: a channel whose
converted temperature disagrees with its own raw resistance is marked invalid
rather than silently trusted. `Pt100TemperatureFromResistance` is retained from
the existing codebase for this purpose; `Max31865CodeToResistance` is deleted.

---

## 6. Configuration

### Removed (23 keys)

All nine `sensor.rtd_click_*` keys and all fourteen `sensor.daq132m_*` keys.

`sensor.sample_temperature_source` is **also removed**. With a sole source it
selects nothing, and the simulated path is already reached through
`runtime.use_simulated_sensors`. A one-valued selector is vestigial config that
later reads as an extension point which does not exist.

### Added (7 keys)

```ini
sensor.sequent_rtd_stack=0                   # 0..7 -> I2C 0x40..0x47
sensor.sequent_rtd_channels=1,2,3,4,5,6,7,8  # card channel per logical sample
sensor.sequent_rtd_poll_ms=1000
sensor.sequent_rtd_expect_sensor_type=pt100
sensor.sequent_rtd_resistance_min_ohm=60.0
sensor.sequent_rtd_resistance_max_ohm=390.0
sensor.sequent_rtd_crosscheck_tol_c=2.0
```

### Changed

`sensor.resistance_source` changes meaning from the retired INA3221 path to
`sequent_rtd`, resolving the collision of two unrelated things both called
"resistance."

### Validation rules (`config.cpp` and `hardware_setup.py`)

- `sequent_rtd_stack` in `[0,7]`; reject otherwise with the address it implies.
- `sequent_rtd_channels` has exactly `hardware.sample_count` entries, each in
  `[1,8]`, with no duplicates.
- `sequent_rtd_resistance_min_ohm < sequent_rtd_resistance_max_ohm`.
- `sequent_rtd_expect_sensor_type` is `pt100` or `pt1000`.
- Existing RTD-Click GPIO conflict checks (`rtd_click_drdy_line` etc.) are
  deleted along with the keys they guarded.

`SensorManager` is the only place that knows about both worlds: it translates
these validated INI values into a `SequentRtdAdapter::Options` at construction.
The adapter never sees `SensorHardwareConfig`.

Sensor type is **read and verified, never written**, consistent with the
bench-only calibration decision. `Probe` reads `I2C_SENSORS_TYPE` /
`I2C_MEM_PT1000` and refuses to come up on mismatch. This catches the specific
failure of a card configured for PT1000 with PT100 probes wired to it, which
would otherwise read plausibly wrong rather than obviously wrong.

---

## 7. Health, validity, and telemetry

### Validity policy

`sample_temp_ok_` was a single bool inherited from a one-channel source. With
eight independent channels it becomes a policy:

> `sample_temp_ok_` is true when every channel referenced by
> `heaters.temperature_channels` is valid.

Not "all eight." `HardwareConfig` records that samples 6 and 7 are pulled but
unheated, so losing one of them is a data-quality event, not a heater-safety
event, and must not be able to trip the thermal path. Per-channel validity still
reaches ground through the existing `SensorSnapshot::sample_temp_valid` vector,
so a dead unheated channel is visible without being dangerous.

### Telemetry struct

`SensorSnapshot` loses `ComponentHealth daq132m` and `ComponentHealth rtd_click`
and gains a single `ComponentHealth sequent_rtd`.
`SensorSnapshot::sample_resistance_ohm` stops being a compatibility field that
always reports `0.0` and carries real per-channel resistance.

### Wire protocol change

`COMPONENT_STATE` currently carries `DAQ132M` and `RTD_CLICK` tokens. Both
collapse into one `SEQUENT_RTD` token. **This is a breaking change to the
telemetry protocol**: onboard and ground station must deploy together, as there
is no mixed-version window in which an old ground station parses a new frame
correctly. `docs/protocol.md` must be updated in the same change.

### Card diagnostics

Die temperature, 5 V rail, `RTD_REINIT_COUNT`, and the burst/fallback mode are
surfaced through `ComponentSummary()` and `ActiveCheck`, **not** the DATA frame.
`RTD_REINIT_COUNT` is the valuable one: a climbing ADC re-initialisation count is
an early warning of a card struggling in the cold, visible before channels begin
dropping out. They are kept off the wire because
`tests/downlink_bandwidth_test.cpp` establishes frame width as a budgeted
resource.

---

## 8. Blast radius

Deletions from `sensor_manager.cpp` (currently 1321 lines), measured:

| Function | Lines |
|---|---:|
| `ReadDaq132m` | 113 |
| `ReadRtdClickMax31865` | 143 |
| `DaqLoop` | 71 |
| `RtdClickLoop` | 42 |
| `AppendRtdClickDiagnostics` | 20 |
| `Max31865CodeToResistance` | 4 |
| **Total removed** | **393** |

`Pt100TemperatureFromResistance` (26 lines) is retained as the cross-check.
Adding the ~60-line polling worker leaves `sensor_manager.cpp` near 990 lines.

| Area | Files | Change |
|---|---|---|
| New driver | 2 new | `hal/sequent_rtd_adapter.{hpp,cpp}` + `I2cBus` seam |
| Onboard core | `sensor_manager.{hpp,cpp}`, `config.{hpp,cpp}`, `telemetry.{hpp,cpp}`, `system_controller.cpp` | Deletions above; health field collapse |
| Build | `onboard/CMakeLists.txt`, `tests/CMakeLists.txt` | New source and test targets |
| Config | `config/onboard.example.ini`, `config/onboard.debug.ini` | 23 keys out, 7 in |
| Scripts | `scripts/hardware_setup.py`, `scripts/spi_probe.py` | Drop RTD/DRDY conflict checks; add I2C presence probe |
| Ground station | `app/gui/panels_info.py` (`:123`, `:220`), `app/protocol.py` | Token rename |
| Tests | 4 C++, 3 Python | See §9 |
| Docs | 11 files + `README.md` | `docs/rev-c-rtd-click-plug-and-play.md` (460 lines) replaced wholesale |

### Migration of deployed configs

`hardware_setup.py` already contains a `migrate_config` routine with stale-key
removal, exercised by `test_hardware_setup.py:84`. Extend it to drop the 23
retired keys and inject the 7 new defaults, so existing deployed INI files
upgrade rather than hard-fail on first boot.

---

## 9. Testing

### New: `tests/unit/test_sequent_rtd_adapter.cpp`

Driven by `FakeI2cBus` serving canned register images. Runs on a developer
machine with no hardware attached. Covers:

- float32 decode and byte order against known bit patterns.
- 1-indexed channel addressing (`kRtdVal1 + 4*(ch-1)`) and non-identity channel
  remapping.
- Stack-to-address arithmetic, including rejection of stack `< 0` and `> 7`.
- Burst read and per-channel fallback producing identical results from identical
  register bytes; fallback latching and being reported.
- NaN and infinity rejection.
- Resistance outside the plausibility window marking a channel invalid.
- Cross-check disagreement between reported temperature and
  `Pt100TemperatureFromResistance` marking a channel invalid.
- `Probe` refusing a sensor-type mismatch and an unexpected card type.
- Absent card (read failure at `kRevMajor`) reported as a probe failure, not a
  crash.

### Updated

C++: `test_sensor_manager_rev_c.cpp`, `test_suite.cpp` (config assertions at
`:382` and `:474`), `test_telemetry_rev_c.cpp`, `test_safety_rev_c.cpp` for the
new validity policy.

Python: `test_protocol.py` (token rename, `:58-70`, `:171-192`),
`test_gui_smoke.py` (`:59`), `test_hardware_setup.py` (`:40-46`, `:84-117`).

### Bench verification — required before trusting the map

Two assumptions in this design come from reading vendor source rather than from
measurement, and neither can be settled by unit tests:

1. The derived register offsets, particularly `kRtdRes1 = 59`.
2. Whether the firmware honours 32-byte burst reads.

**First bench step** is therefore a raw dump of bytes 0–104 from a live card with
a known precision resistor on channel 1, checked against the derived map, before
any other bring-up step is trusted. This gates everything downstream.

---

## 10. Out of scope

- Multi-card stacking and aggregation beyond a single card.
- Reassigning the freed GPIO 16 and GPIO 25.
- Any write path to card flash, including calibration and sensor type.
- Any use of the card's `I2C_MEM_WDT_*` watchdog.
- The card's RS485/Modbus port and its LED threshold registers.
- Re-enabling the INA3221 resistance instrument.
