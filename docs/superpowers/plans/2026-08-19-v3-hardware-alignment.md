# Schematic v3 Hardware Alignment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Align the flight software with the final electronics wiring (schematic v3): new GPIO map + power policy, a TMC5160 SPI-motion stepper driver, and a MAX31865 dual-click sample-resistance instrument.

**Architecture:** Package 1 is config-only. Packages 2–3 share one new `SpiBus` transport seam (mirroring `I2cBus`): the TMC5160 driver implements the existing pulse-level `StepperDriver` interface via XTARGET position-dribble; the MAX31865 adapter is a clean-room driver feeding a new `SensorManager` worker. No telemetry wire-format changes anywhere.

**Tech Stack:** C++17/CMake, plain `assert` tests; Python 3 unittest for ground station tooling. Linux spidev/libgpiod behind seams so everything unit-tests on the Windows dev host.

**Spec:** `docs/superpowers/specs/2026-08-19-v3-hardware-alignment-design.md` (authority: schematic v3, owner-confirmed).

## Global Constraints

- Branch: create `feature/v3-hardware-alignment` from `main` before Task 1.
- Build recipe (PATH is broken by default; build into `<repo>/build`, never `%TEMP%`):
  ```powershell
  $mingw = "C:\Users\gutes\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
  $bt = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
  $env:Path = "$mingw;$bt\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$bt\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;" + $env:Path
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER="$mingw\g++.exe"
  cmake --build build; ctest --test-dir build
  cd ground-station; python -m unittest discover -s tests
  ```
  `BAD_COMMAND` from ctest = Windows Smart App Control blocking an unsigned exe — delete that one exe, rebuild that target; never a test failure. Genuine failures are assertion aborts.
- Baseline at branch: C++ 13/13, Python 77 exit 0. Both suites green at the end of every task.
- **Test-quality bar (15 findings on the previous plan were tests that could not fail):** for every new test, prove load-bearingness — temporarily delete/mutate the named behaviour, observe the assertion message, revert in the SAME step. Report the message. Assert both directions where a policy has two outcomes. A value rejected by two independent mechanisms tests neither — isolate.
- Commit as soon as the tree is green (agents have been killed mid-task by account limits). Never leave temporary mutations in the tree between steps.
- Wire protocol: NO changes to DATA/STATUS/COMPONENT_STATE formats or tokens in any task.
- The Sequent RTD temperature path (`sequent_rtd_adapter`, `SequentRtdLoop`) is untouched in all tasks.
- v3 GPIO map (BCM), the single source of truth:
  heaters H1..H6 = **19,13,6,5,24,23**; motor0 CS/EN = **22/20**; motor1 CS/EN = **27/21**;
  SPI0 CE1(GP07)=RTD1/SAMPLE1=`/dev/spidev0.1`, CE0(GP08)=RTD2/SAMPLE2=`/dev/spidev0.0`;
  reserved (HAT): **14,15,17,26**; reserved (SPI CE): **7,8**. No STEP/DIR lines exist.
- TMC5160 facts: IOIN `VERSION==0x30`; SPI mode 3, 40-bit datagrams, write = `addr|0x80`; ramp
  generator internal resolution 256 µsteps/fullstep (Step→ΔXTARGET = `256/microstep_divisor`,
  bench-gated); registers: GCONF 0x00, IOIN 0x04, GLOBALSCALER 0x0B, IHOLD_IRUN 0x10,
  TPOWERDOWN 0x11, RAMPMODE 0x20 (0=positioning), XACTUAL 0x21, VSTART 0x23, A1 0x24, V1 0x25,
  AMAX 0x26, VMAX 0x27, DMAX 0x28, D1 0x2A, VSTOP 0x2B, XTARGET 0x2D, CHOPCONF 0x6C
  (MRES 27:24, TOFF 3:0), DRV_STATUS 0x6F.
- MAX31865 facts: SPI mode 1, ≤1 MHz; config reg 0x00 (write 0x80): VBIAS bit7, 1SHOT bit5,
  3WIRE bit4 (=0, we are 4-wire), FAULTCLR bit1, 50HZ bit0 (=1); RTD MSB/LSB 0x01/0x02 (15-bit
  code, LSB bit0 = fault flag); fault status 0x07; `R = code × R_ref / 32768`; one-shot timing:
  bias on → ≥10 ms settle → 1SHOT → ≥65 ms → read → bias off.

---

### Task 1: v3 pin map, power policy, PWM, driver identity (C++ config + INIs)

**Files:**
- Modify: `onboard/include/coatheal/config.hpp` (MotorConfig, HeaterOutputConfig/PowerConfig defaults)
- Modify: `onboard/src/config.cpp` (defaults, parse branches, validation, GPIO claims)
- Modify: `config/onboard.example.ini`, `config/onboard.debug.ini`
- Modify: `tests/unit/test_suite.cpp` (WriteTempConfig baseline + assertions)

**Interfaces:**
- Consumes: nothing.
- Produces: `MotorConfig` WITHOUT `step_line`/`dir_line`/`pulse_high_us`, WITH new `double sense_resistor_ohm = 0.075;` and `driver` default `"tmc5160"`; heater/power/pwm defaults per the table below. Later tasks rely on these exact names.

Changes (all four files kept in lockstep — the INIs and the test baseline emit these keys, so partial edits fail the suite loudly):

| Item | New value |
|---|---|
| `heaters.output_lines` default + INIs | `{19, 13, 6, 5, 24, 23}` |
| `heaters.pwm_frequency_hz` default + INIs | `1.0` (software PWM; heaters have high thermal inertia) |
| `power.max_active_heaters` / `max_thermal_w` | `3` / `15.0` (owner rule: never more than 3 heaters) |
| motors[0]: cs/enable | `22` / `20` |
| motors[1]: cs/enable | `27` / `21` |
| `motorN.driver` | default `"tmc5160"`; validation accepts ONLY `tmc5160` and `simulated`; `tmc2240` rejected with error naming it retired |
| `motorN.step_line`, `dir_line`, `pulse_high_us` | struct fields, defaults, parse branches, and claim_gpio uses all deleted (unknown-key rejection then guards stale INIs) |
| `motorN.sense_resistor_ohm` | new key, default `0.075`, validated `> 0.0 && < 1.0` |

GPIO claim table additions in the validator (same claim_gpio mechanism, fixed owners):
`14 "reserved: sequent_hat uart_tx"`, `15 "reserved: sequent_hat uart_rx"`, `17 "reserved: sequent_hat rs485_dir"`, `26 "reserved: sequent_hat intn"`, `7 "reserved: spi0_ce1 (max31865 sample1)"`, `8 "reserved: spi0_ce0 (max31865 sample2)"`. A heater or motor line colliding with any of these must fail config load with the reserved owner named in the error.

- [ ] Step 1: update `tests/unit/test_suite.cpp`: baseline emits the new keys/values (drop step/dir/pulse_high_us lines, add sense_resistor_ohm), assertions updated (`max_thermal_w==15.0`, `max_active_heaters==3`, `output_lines=={19,13,6,5,24,23}`, `driver=="tmc5160"`); add negative tests: `motor0.driver=tmc2240` rejected (fragment `"retired"`), `motor0.step_line=19` rejected as unknown key, `heater.output_lines=17,...` rejected with fragment `"sequent_hat"` (reserved collision), `motor0.sense_resistor_ohm=0` rejected. Run: expect compile/assert failures (baseline emits keys the parser still accepts — negative tests fail first).
- [ ] Step 2: implement all config.hpp/config.cpp changes above.
- [ ] Step 3: update both INIs to the new map (comment each motor block: "v3: TMC5160, SPI-only motion — no STEP/DIR").
- [ ] Step 4: build; full C++ suite green. **Python suite will go red in `test_hardware_setup.py` (reads the example INI) — expected; Task 2 owns it; list the failures in the report.**
- [ ] Step 5: load-bearingness pass per the Global bar (at minimum: delete one reserved-claim entry and observe the collision test fail; restore).
- [ ] Step 6: commit (`feat!: adopt schematic v3 pin map and 3-heater power policy`).

### Task 2: Python mirrors — hardware_setup.py + spi_probe.py

**Files:**
- Modify: `scripts/hardware_setup.py`, `scripts/spi_probe.py`
- Modify: `ground-station/tests/test_hardware_setup.py`

**Interfaces:**
- Consumes: Task 1's key set exactly.
- Produces: green Python suite; `RETIRED_SENSOR_KEYS`-style handling extended to motor keys.

- [ ] Step 1: `FINAL_PIN_VALUES`: heaters `"19,13,6,5,24,23"`, motor0 `cs 22 / enable 20`, motor1 `cs 27 / enable 21`, both `driver "tmc5160"`, add `sense_resistor_ohm "0.075"`, delete step/dir entries; add `heater.pwm_frequency_hz "1.0"` if the table carries it today (match Task 1's INI content byte-for-byte where keys overlap).
- [ ] Step 2: extend the retired-key filter with `motor0/1.step_line`, `dir_line`, `pulse_high_us`, and add migration test input/absence assertions for them (same pattern as the sensor keys; the template-regression test needs no change).
- [ ] Step 3: validator: drop step/dir GPIO checks; add reserved-pin collision checks mirroring Task 1's list (exact-fragment tests, unique across all validator error strings — grep to prove).
- [ ] Step 4: `spi_probe.py`: TMC2240 IOIN probe becomes TMC5160 (`expected VERSION 0x30`, message text updated); soft-CS transfers must open spidev with **SPI_NO_CS** (document in a comment why: CE0/CE1 are wired to the clicks). Keep the Sequent I2C probe untouched.
- [ ] Step 5: both suites green; load-bearingness pass on new negative tests; commit.

### Task 3: `SpiBus` seam + `FakeSpiBus`

**Files:**
- Create: `onboard/include/coatheal/hal/spi_bus.hpp`, `onboard/src/hal/spi_bus.cpp`
- Create: `tests/unit/fake_spi_bus.hpp`, `tests/unit/test_spi_bus.cpp`
- Modify: `onboard/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  ```cpp
  class SpiBus {
   public:
    virtual ~SpiBus() = default;
    // no_cs: caller frames the transaction with its own GPIO chip-select; the
    // kernel must NOT assert CE0/CE1 (they are wired to the MAX31865 clicks).
    virtual bool Open(const std::string& device, std::uint8_t mode,
                      std::uint32_t speed_hz, bool no_cs) = 0;
    virtual bool Transfer(const std::uint8_t* tx, std::uint8_t* rx,
                          std::size_t len) = 0;
    virtual void Close() = 0;
    virtual bool available() const = 0;
  };
  class LinuxSpiBus : public SpiBus { ... };  // spidev; guarded like LinuxI2cBus
  ```
  `FakeSpiBus`: rejects `Transfer` unless opened (LESSON: the I2C fake shipped more permissive than the real bus and cost a fix round — build the guard in from the start); records `open_no_cs()`, `open_mode()`, `open_device()`; scripted exchange queue `Expect(tx_bytes, rx_bytes)` with strict length+content match and a `mismatch_count()`; `FailNextTransfers(n)`.
- `LinuxSpiBus::Open` sets `SPI_IOC_WR_MODE` with `SPI_NO_CS` OR-ed in when `no_cs`, plus speed/bits; on non-Linux hosts `available()==false`, `Open` fails cleanly.

- [ ] Steps: failing tests first (open-gate, scripted exchange match, mismatch detection, fail injection, no_cs recording, non-Linux availability) → header/impl → CMake → green → load-bearing pass → commit.

### Task 4: `Tmc5160Driver`

**Files:**
- Create: `onboard/include/coatheal/tmc5160_driver.hpp`, `onboard/src/tmc5160_driver.cpp`
- Create: `tests/unit/test_tmc5160_driver.cpp`
- Modify: `onboard/CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `SpiBus`/`FakeSpiBus` (Task 3), `StepperDriver` seam (unchanged), `MotorConfig` incl. `sense_resistor_ohm` (Task 1).
- Produces:
  ```cpp
  struct Tmc5160Config {  // built from MotorConfig by the factory (Task 5)
    std::string spi_device; std::uint32_t spi_speed_hz; std::string gpio_chip;
    std::size_t cs_line; std::size_t enable_line;
    bool invert_direction; bool enable_active_low;
    double run_current_a_rms; double hold_current_frac; double sense_resistor_ohm;
    int microstep; int retry_ms;
  };
  class Tmc5160Driver : public StepperDriver {
   public:
    // bus/cs/en injectable; nullptr cs/en = test mode (no GPIO on host).
    Tmc5160Driver(Tmc5160Config cfg, SpiBus* bus,
                  GpioOutput* cs, GpioOutput* en);
    // StepperDriver overrides + bool Reinitialize(); (ActiveCheck -> Reinitialize)
    static std::uint8_t EncodeMres(int divisor);           // 256->0 ... 1->8
    static std::uint32_t DeltaXtarget(int divisor);        // 256/divisor
    static bool CalculateCurrent(double a_rms, double sense_ohm,
                                 double hold_frac, std::uint32_t* globalscaler,
                                 std::uint8_t* irun, std::uint8_t* ihold);
  };
  ```
- Behaviour:
  - `Reinitialize()`: IOIN read, require `VERSION==0x30` (byte 31:24) else unhealthy with error `TMC5160_VERSION`; write GCONF, CHOPCONF (MRES from divisor, TOFF=3), GLOBALSCALER/IHOLD_IRUN from `CalculateCurrent`, TPOWERDOWN, RAMPMODE=0, VSTART=0, VSTOP=10, A1=AMAX=0xFFFF, V1=0, D1=DMAX=0xFFFF, VMAX = `4 × 100(hz) × 256` (follows a ≤100 Hz dribble with 4× margin); XACTUAL=XTARGET=0; readback-verify GCONF+CHOPCONF.
  - `Step(fwd)`: `target_ += (fwd^invert ? +Δ : -Δ)`; write XTARGET. Every SPI datagram: take `SpiBusMutex(spi_device)`, assert soft CS low via `cs` GPIO, 5-byte transfer, CS high. In test mode (null cs) skip GPIO, still transfer.
  - `Enable(false)`: read XACTUAL → write XTARGET=XACTUAL (freeze, no coast) → TOFF=0 → EN GPIO inactive. `Enable(true)`: EN active → Reinitialize if not healthy → TOFF=3.
  - `pulses_issued()` counts Step calls (parity with old drivers).
- Tests (each with a stated mutation that must fail it): `EncodeMres` table (256→0…1→8, invalid divisor rejected); `DeltaXtarget` (÷: 4→64, 16→16, 256→1); `CalculateCurrent` against two hand-computed cases (2.8 A peak limit respected; GLOBALSCALER in [32..256] band); version-gate probe (rx scripted `0x30`→healthy, `0x40`→unhealthy — the TMC2240 answer must FAIL); Step writes `XTARGET = addr 0xAD|0x80` with correct 32-bit payload for +3/−2 steps at divisor 4 (scripted exchange, both directions, invert_direction case); Enable(false) sequence order (XACTUAL read then XTARGET write then TOFF=0) via the exchange log; transfer-failure → unhealthy + `ActiveCheck` re-probes.

- [ ] Steps: failing tests → implement → green → load-bearing pass (mandatory mutations: drop the version gate; swap Δ sign; skip the freeze write) → commit.

### Task 5: Integration — factory swap, retire TMC2240/GPIO-pulse stack

**Files:**
- Modify: `onboard/src/system_controller.cpp` (`:175-199` factory)
- Delete: `onboard/include/coatheal/tmc2240_driver.hpp`, `onboard/src/tmc2240_driver.cpp`, `onboard/src/hal/gpio_step_dir_driver.cpp`; remove `GpioStepDirStepperDriver` from `hal/stepper_driver.hpp`
- Modify: `onboard/CMakeLists.txt`, `tests/unit/test_suite.cpp` (CHECK-alias/driver-name tests), `tests/unit/test_stepper_rev_c.cpp` and `tests/unit/stepper_test.cpp` if they name the deleted classes (grep first)
- Modify: `onboard/src/main.cpp` if it references deleted headers (grep)

**Interfaces:**
- Consumes: `Tmc5160Driver` (Task 4). Factory: `driver=="simulated"` → `SimulatedStepperDriver`; `"tmc5160"` → `LinuxSpiBus` (owned per driver) + `GpioOutput` cs/en + `Tmc5160Driver`. `Tmc5160Config` built from `MotorConfig` field-for-field.
- Produces: a tree with zero references to `Tmc2240`, `GpioStepDir`, `step_line`, `dir_line`, `pulse_high_us` outside docs/ and git history. `SimulatedStepperDriver` remains the test seam for all stepper_channel/controller tests (they must pass unchanged — the pulse-level seam is exactly why).

- [ ] Steps: grep-driven inventory first (report it) → swap factory → delete → fix references → both suites green → commit (`feat!: drive steppers via TMC5160 SPI motion (position dribble)`).

### Task 6: `Max31865Adapter`

**Files:**
- Create: `onboard/include/coatheal/hal/max31865_adapter.hpp`, `onboard/src/hal/max31865_adapter.cpp`
- Create: `tests/unit/test_max31865_adapter.cpp`
- Modify: CMake files.

**Interfaces:**
- Consumes: `SpiBus`/`FakeSpiBus`.
- Produces:
  ```cpp
  class Max31865Adapter {
   public:
    struct Options { std::string spi_device; double reference_ohm = 470.0;
                     std::uint32_t spi_speed_hz = 500000; };
    struct Reading { double resistance_ohm = 0.0; bool valid = false;
                     bool out_of_range = false; std::uint8_t fault_bits = 0; };
    Max31865Adapter(SpiBus* bus, const Options& options);
    bool Probe(std::string* error);              // config-reg write/readback
    bool ReadOneShot(Reading* out, std::string* error);
    static double CodeToOhms(std::uint16_t code, double reference_ohm);
  };
  ```
- Behaviour: native-CE device (open with `no_cs=false`), mode 1. `ReadOneShot`: bias on → settle → 1SHOT → wait → read 0x01/0x02 → bias off. LSB bit0 set OR code ≥ 32760 (near full scale) → `valid=false, out_of_range=true`, read+report fault reg 0x07, issue FAULTCLR. **Saturation must never yield a plausible number** — `resistance_ohm` still reported (for diagnostics) but `valid=false`. The wait uses injected-clock-free fixed sleeps ≤80 ms (worker thread context, not control loop).
- Tests: `CodeToOhms` (code 8192, ref 470 → 117.5 Ω exact; ref 400 case too); scripted one-shot sequence order (bias, 1shot, read, bias-off — exchange log); fault-bit → out_of_range + FAULTCLR written; near-full-scale code → out_of_range; probe readback mismatch → false; transfer failure → false. Mutations required: drop the fault-bit check; drop the full-scale check; reorder bias-off before read.

- [ ] Steps: failing tests → implement → green → load-bearing pass → commit.

### Task 7: SensorManager wiring — `Max31865Loop`, `resistance_source=max31865_click`

**Files:**
- Modify: `onboard/include/coatheal/sensor_manager.hpp`, `onboard/src/sensor_manager.cpp`
- Modify: `onboard/include/coatheal/config.hpp`, `onboard/src/config.cpp` (3 new sensor keys; `resistance_source` accepted set += `max31865_click`, default flips to it)
- Modify: `config/*.ini`, `tests/unit/test_suite.cpp` (keys), `tests/unit/test_sensor_manager_rev_c.cpp`
- Modify: `scripts/hardware_setup.py` + its tests (mirror the enum + new keys; exact-fragment rules)

**Interfaces:**
- Consumes: `Max31865Adapter` (Task 6).
- Produces: config keys `sensor.max31865_reference_ohm` (470.0), `sensor.max31865_poll_ms` (1000), `sensor.max31865_sample_indices` (default `0,4`; exactly two distinct entries in [0, sample_count)); `SensorManager` constructor gains trailing test seams `SpiBus* click1_override = nullptr, SpiBus* click2_override = nullptr` (production call site compiles unchanged — the proven `rtd_bus_override` pattern); worker `Max31865Loop` (poll both clicks, fill `sample_resistance_ohm_[index]`, health `max31865_health_`); `ReadSnapshot`'s dispatch gains a `max31865_click` branch: vector from cache, `resistance_ok_ = clicks_bus_ok_` (both adapters' last conversation succeeded — saturation is NOT a bus failure, it is a valid measurement of an out-of-range specimen: channel invalid, bus ok). Unmonitored samples stay 0.0 → wire `-`. Click health feeds `ComponentSummary`/`ActiveCheck` (component names `MAX31865_1/2`, plus CHECK selector `MAX31865`) — **no COMPONENT_STATE token, no wire change** (`CHECK` argument surface is a command, not the frame format).
- Device paths fixed by v3: click index 0 (SAMPLE1) = `/dev/spidev0.1`, click index 1 (SAMPLE2) = `/dev/spidev0.0` — constants with a comment naming the CE crossover, not config.
- Tests (seam-injected, bounded steady_clock deadlines — no fixed sleeps in asserts; stop workers before return): healthy fakes → `resistance_ok()` true and the two monitored indices carry scripted values while others stay 0.0; failing bus → `resistance_ok()` false; saturated reading → `resistance_ok()` TRUE but the affected index reported with its `-`/invalid convention (assert the distinction — this is the finding-class both directions rule); config negative tests for the three new keys (unique fragments).

- [ ] Steps: failing tests → config keys → adapter members/worker/dispatch → both suites green → load-bearing pass (drop the `max31865_click` dispatch branch → healthy-path test must fail; wire `clicks_bus_ok_` dead → recovery test must fail) → commit.

### Task 8: Docs + commissioning gates + final sweep

**Files:**
- Create: `docs/tmc5160-commissioning.md` (supersedes `docs/tmc2240-pin-configuration-and-commissioning.md` — delete it, repoint inbound links)
- Modify: `docs/sequent-rtd-bring-up.md` (add the resistance-instrument + heater-walk gates), `docs/hardware.md` (pin tables), `docs/configuration.md` (all new/changed keys incl. 3-heater rule and 1 Hz PWM), `docs/onboard.md`, `docs/architecture.md`, `README.md`
- Modify: `docs/protocol.md` ONLY if the CHECK argument list gains `MAX31865` (argument surface, not frame format).

Content requirements: the seven bench gates from spec §6 verbatim-in-substance, each with a fill-in blank where a value is measured (Step→ΔXTARGET one-rev result, EN polarity, click reference value, coating resistance range, observed VERSION bytes); the v3 GPIO table matching Global Constraints exactly; the SPI topology diagram-in-text (4 devices, which CS is hard/soft, where SPI_NO_CS applies); "no STEP/DIR — motion is SPI" stated prominently; stale-reference sweep with per-hit justification (`tmc2240`, `step_line`, `rtd click`-as-temperature, old heater pins) using the refined past-tense/alias gate from the previous plan.

- [ ] Steps: write/update docs → sweep grep with per-hit justification in the report → both suites green (docs can't break them; run anyway as the branch-tip check) → commit.

---

## Self-review notes

- Task ordering is dependency-clean: 1→2 (key set), 3→4→5 (seam→driver→factory), 3→6→7 (seam→adapter→manager), 8 last. 4 and 6 are independent after 3; the executor may parallelise only if the controller's process allows (it does not — sequential per SDD).
- The one deliberately open value (`max31865_sample_indices` default `0,4`) is owner-flagged as a placeholder in the spec; config-only to change.
- `pulse_jitter_bench` exercises the pacing thread, not the GPIO backend — survives Task 5 unchanged; Task 5's grep inventory must confirm.
- Plan tests specify values + required mutations rather than full listings where the previous plan's pre-written tests proved defect-prone; the load-bearing pass is mandatory in every task.
