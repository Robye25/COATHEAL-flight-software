# Rev B.1 — Verification log & resumable session context

**Branch:** `rev-b-integration`
**Started:** 2026-04-17
**Purpose:** single source of truth to resume this verification pass if the
conversation context is lost. Read top-to-bottom.

---

## 0. Canonical Rev B.1 facts (as validated by the team)

Validated against the statement checklist issued 2026-04-17. The text below is
the authoritative baseline against which agents verify; if the implementation
contradicts any line here, the implementation must be corrected (or this line
corrected after an explicit scope discussion).

### Mission & science
- BEXUS high-altitude balloon experiment; thermal-healing coating research.
- Microcracks are induced **mechanically** by pulling samples, not by the
  retired +70 °C activation ramp.
- **Sample electrical resistance is measured over time** through the INA3221
  pair (I2C 0x40 + 0x41) to detect crack formation.

### Samples & heaters
- **8 samples** total, in two groups of four.
- Motor 0 pulls samples 0–3; motor 1 pulls samples 4–7.
- **6 flight heaters** on samples 0–5. Samples 6 and 7 are pulled but
  unheated.
- Heaters are 5 W @ 24 V DC polyimide film. **16 heaters procured: 6 flight +
  10 spares** (user correction, 2026-04-17).
- No electronics-box heater and no box PID.
- Sample-temperature floor +5 °C across ASCENT/FLOAT/DESCENT (only thermal
  policy). 0.5 °C hysteresis; PID off at T ≥ 5.0 °C.

### Motion
- 2 × OMC **17E19S2504BSM5-150RS** NEMA-17, integrated ball-screw, 1 mm lead.
  200 full-steps/rev → 1 mm linear per revolution.
- **2 × TMC2240** stepper drivers — *not* TMC5160 (user correction,
  2026-04-17). SPI-configured; STEP/DIR/EN operation. **Must be actually
  integrated in software**, not stubbed.
- Max 100 full-steps/s (≈30 rpm), trapezoidal accel/decel at 200 steps/s²,
  default microstep 4× (800 µsteps/rev).
- `MotionLock` serialises motors; `HeaterScheduler` zeros all duty while the
  lock is held (`HEATER_INHIBITED` STATUS bit).

### Sensors
- **MS5803-01BA** on I2C, 10–1300 mbar range. **This is both the ambient
  pressure sensor AND the box temperature sensor** (the chip is mounted
  inside the electronics box, so its temperature reading is the box interior;
  the gondola is unpressurised, so its pressure reading is ambient). Both
  readings must be collected / stored / sent in telemetry.
- **GUVA-S12SD** analog UV through **ADS1015** 12-bit ADC on I2C.
- 8 × Labfacility **XF-931 PT100** probes through 2 × 4-channel **Modbus-RTU
  PT100 collectors** sharing one USB-RS485 adapter on `/dev/ttyUSB0`.
- **INA3221 × 2** science instrument for sample resistance: 2 chips ×
  3 channels = 6 sample channels. Samples 6 & 7 render as `-`.
- **DS3231** I2C RTC for UTC.
- **Pololu D24V50F5** (5 V / 5 A) from the 28.8 V gondola rail; XT60.

### Phase FSM
- `kBoot → kAscent → kFloat → kDescent → kLanded` (+ `kStopped`).
- Pressure transitions: 100 mbar (ASCENT→FLOAT), 300 mbar (FLOAT→DESCENT),
  800 mbar (DESCENT→LANDED). No timed float hold.

### Telemetry (Rev B.1 wire format)
- `DATA,<session>,<seq>,<ts>,<rtc_valid>,<ambient_T>,<ambient_P>,<uv>,<sample_0..sample_7>,HEATER_DUTY=<6>,RESISTANCE=<8 or ->,PHASE=,MODE=,STATUS=,STEPPER0=,STEPPER1=`
- `<ambient_T>` = MS5803 temperature = **box-interior temperature** (the
  sensor is in the box). Ground station should label this "Box / Ambient T".
- `EVT,PULL,...` per completed pull cycle.
- STATUS: `SD|USB|I2C|SPI|LINK|T_AMBIENT|P_AMBIENT|UNIFORMITY|OVERTEMP|ENERGY|RS485|HEATER_ACTIVE/INHIBITED|RESISTANCE` bits.

### Build & run (Pi reference)
- Remote: `ssh coatheal@169.254.10.10` — password held out-of-repo (supplied
  to verification agents via prompt; never committed).
- Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --parallel`.
- Tests: `ctest --test-dir build --output-on-failure`.
- Bench-mode binary: `./build/onboard/coatheal_onboard --config config/onboard.debug.ini`.
- GS CLI: `python ground-station/main.py telemetry-server --bind 0.0.0.0 --port 4000 --no-discovery-enabled`.
- GS GUI: `python ground-station/gui_app.py --host 127.0.0.1`.
- Python tests: `python -m unittest discover -s ground-station/tests -p "test_*.py"`.

### Known stubs / pending (pre-verification pass)
- All HAL adapters are stubs (no real I2C / SPI / RS485 transactions).
- `Tmc5160Driver` class exists; must be **renamed to `Tmc2240Driver`** and
  **actually wired** for SPI configuration (not stubbed) during verification.
- `config.cpp` accepts `motor0.*`, `motor1.*`, `pull.*` but ignores them.
- GPIO pin map for the 6-channel MOSFET module not finalised.
- `SystemController::Initialize` hard-codes Rev B motion-envelope defaults.

---

## 1. User corrections applied 2026-04-17

| # | Original statement | Corrected to |
|---|---|---|
| B.7 | "16 heaters: 8 flight + 8 spares" | **6 flight + 10 spares** |
| B.8 | "No box temperature sensor" | **MS5803-01BA doubles as box temperature sensor.** Data must be collected/stored/sent (it already is, as `ambient_temp_c`; semantic clarified in docs). |
| C.11 | "Both motors driven by TMC5160 (QHV5160)" | **Both motors driven by TMC2240.** Driver **must be integrated in software** (SPI config etc.) — not left as a stub. |
| D (general) | — | **Verification agents must ensure every sensor in the BOM is actually implemented end-to-end: data collected, stored, sent over the wire.** |

Baseline commit with these textual corrections: `<filled in after commit>`.

---

## 2. Verification team plan

Three parallel agents dispatched from this branch, each with paramiko SSH to
the Pi for build / test / bench-mode execution. Each operates in its own
worktree for isolation; the orchestrator merges results.

### Agent A — Correctness Auditor
Personality: pedantic senior reviewer.
**Scope:**
- Rename all `Tmc5160Driver` references → `Tmc2240Driver`; file rename; class
  rename; CMakeLists; docs.
- **Implement the TMC2240 SPI configuration pass** (real SPI writes against
  `/dev/spidev*`): GCONF, IHOLD_IRUN, CHOPCONF, TPOWERDOWN at minimum.
  `StepperChannel` must actually use the configured driver on non-simulated
  builds.
- Static review: thread safety, lifetimes, resource leaks, narrow casts.
- `cmake --build` + `ctest --output-on-failure` on the Pi.
- Report red/green matrix; patch any breakage on the branch.

### Agent B — Performance & Realtime Reviewer
Personality: jitter-obsessed flight engineer.
**Scope:**
- Measure tick-loop CPU on the Pi at 1 Hz and 5 Hz.
- Audit hot paths for allocations, long lock holds, fsync cost, disk-queue
  backpressure.
- Verify `StepperChannel` pulse spacing under the trapezoidal ramp is within
  ±5 % of commanded rate on real hardware (or simulated, if no motors
  attached).
- Confirm systemd `WatchdogSec=10` margin at worst-case tick.
- Apply tight fixes only; no refactor.

### Agent C — Integration Tester
Personality: paranoid end-to-end tester.
**Scope:**
- Drive bench-mode end-to-end: `coatheal_onboard` + `gui_app.py` on the same
  Pi (GUI via X11 or headless CLI).
- Exercise every command: PING, STATUS, FORCE_START, FORCE_STOP, HEATERS_OFF,
  RESET_CTRL, SHUTDOWN_SAFE, SET_TICK_HZ, RADIO_SILENCE / RADIO_RESUME, every
  STEPPER_*, PULL_ARM, PULL_EXECUTE. Confirm ACK for each.
- Verify the heater/motor mutex: force a pull, assert duties go to zero on
  the wire, `HEATER_INHIBITED` bit flips.
- Simulate link loss (stop the GS, wait, restart) and verify queue replay +
  dedup.
- Verify `EVT,PULL` round-trips to the GS CSV.
- Ensure **every sensor** in the BOM populates its field in the DATA frame
  (even as simulated values): ambient_temp, ambient_pressure, uv, sample_0..7,
  resistance_0..7 (with `-` for samples 6/7). STATUS bits match the sim
  (no `_FAIL` in bench mode).

Each agent reports into section 4 of this log.

---

## 3. Operational log

(Append timestamped bullets as the pass progresses.)

- 2026-04-17 — log file created. User validated 42-statement checklist with
  3 corrections + 1 scope directive. Baseline corrections committed as
  `<commit>`. Agents A/B/C dispatched with SSH credentials (held out-of-repo).

---

## 4. Agent reports

### Agent A — Correctness Auditor
**Status:** DONE 2026-04-17. Branch: `rev-b-integration`.

**Scope delivered.**
1. **Real TMC2240 SPI writes.** `onboard/src/tmc2240_driver.cpp` now issues the
   5-byte datagram `[addr|0x80][b3..b0]` via `SPI_IOC_MESSAGE(1)` against
   `/dev/spidev1.x` in SPI mode 3 (CPOL=1/CPHA=1), 8 bits/word, 1 MHz. fd is
   opened in the ctor and released in the dtor (RAII), with
   non-copy/non-move enforced. Errors are logged to stderr, never thrown —
   `healthy()` stays `false` and the caller falls back. Non-Linux hosts
   (`__has_include(<linux/spi/spidev.h>)` guard) still compile with the SPI
   path stubbed, so CI stays green.
2. **SystemController wiring.** `Initialize()` now constructs a
   `Tmc2240Driver` per motor (spidev1.0 / spidev1.1) on non-simulated builds.
   If SPI bring-up fails, a loud `[system]` warning is logged and a plain
   `GpioStepDirStepperDriver` is swapped in so the tick loop keeps running
   for GS diagnostics.
3. **Static fix (high severity).** `StepperChannel::ArmPullCycle` read
   `enabled_` without holding `mu_` — TOCTOU against `SetEnabled`. Now
   wrapped in a lock.

**Pi build & test.** `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake
--build build --parallel 3` succeeded (EXIT=0) on `coatheal@169.254.10.10`
(g++ 14.2.0, cmake 3.31.6). `ctest --output-on-failure` reports
**10/10 passed, 0 failed** (0.17 s wall). Matrix:

| # | Test | Result |
|---|------|--------|
| 1 | coatheal_unit_tests | PASS |
| 2 | coatheal_state_machine_tests | PASS |
| 3 | coatheal_safety_tests | PASS |
| 4 | coatheal_downlink_bandwidth | PASS |
| 5 | coatheal_status_led_test | PASS |
| 6 | coatheal_stepper_tests | PASS |
| 7 | coatheal_phase_rev_b_tests | PASS |
| 8 | coatheal_safety_rev_b_tests | PASS |
| 9 | coatheal_stepper_rev_b_tests | PASS |
| 10 | coatheal_telemetry_rev_b_tests | PASS |

**Remaining static-review findings (non-blocking).**
- **HIGH — fixed**: TOCTOU on `StepperChannel::enabled_` in `ArmPullCycle`
  (`onboard/src/stepper_channel.cpp:355`). Fixed inline.
- **MED**: `HeaterScheduler::Schedule` allocates 3–4 `std::vector<double>`
  per tick in the hot path (`onboard/src/heater_scheduler.cpp:48,59,62,73,88`).
  Move to pre-allocated members.
- **MED**: `StepperChannel::SetMicrostep` rescales `position_`/`target_` via
  `int64 * double` then `llround`; loses precision at large positions
  (`onboard/src/stepper_channel.cpp:332-334`). Rarely hit in flight, but
  should scale in integer math.
- **LOW**: `TelemetryClient::SendFrameAwaitAck` uses a `thread_local`
  back-off counter that never resets on successful send
  (`telemetry_client.cpp:576-578` — owned by Agent B, not touched here).
- **LOW**: `CommandServer::RunLoop` tight-loops `continue` on `accept()`
  EINTR/EAGAIN without a sleep or `errno` inspection
  (`command_server.cpp:111-115`). Safe today because `Stop()` closes the
  listen fd and breaks the loop, but a spurious EAGAIN burns CPU.

### Agent B — Performance & Realtime Reviewer
_Filled in on completion._

### Agent C — Integration Tester
_Filled in on completion._

---

## 5. Post-verification checklist

- [ ] `ctest` 100 % green on the Pi.
- [ ] `python -m unittest` 100 % green.
- [ ] Every DATA frame field populated with plausible simulated data in bench mode.
- [ ] Every command returns `ACK`.
- [ ] HEATER_INHIBITED interlock demonstrably zeros duty during a pull.
- [ ] EVT,PULL emitted and captured in `<log>_pulls.csv` on the GS.
- [ ] `Tmc2240Driver` exists, used, and SPI-configured on non-simulated build.
- [ ] Agent findings folded back into `rev-b-integration`, pushed to origin.
- [ ] `rev-b-integration` merged (or tagged) as the pre-flight candidate.
