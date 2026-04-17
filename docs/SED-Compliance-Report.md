# COATHEAL SED v2.0 — Software Compliance Report

Source: `BX38_COATHEAL_SED_v2-0_08Mar2026.pdf` (126 pages, 08 Mar 2026).
Scope: software-relevant requirements only. Chemistry, mechanical, and thermal-
analysis content ignored except where it constrains software behavior (e.g. phase
targets, power caps).

> **Rev B / Rev B.1 notice.** This report was originally written against the
> pre-Rev B phase model (five phases: `ASCENT_HOLD`, `ACTIVATION_RAMP`,
> `FLOAT_HOLD`, `DESCENT_FLOOR`, `STOPPED` — with a +70 °C activation target).
> Rev B collapsed the thermal policy to a single cold-protection floor
> (`phase.sample_floor_c = +5 °C` shared across `ASCENT/FLOAT/DESCENT`) and
> replaced the FSM with `BOOT → ASCENT → FLOAT → DESCENT → LANDED` plus
> `STOPPED`. **Rev B.1 (2026-04-17)** went further and retired the electronics
> box heater, the box PID, the humidity channel, and three of the nine sample
> heaters, cutting the heater count to six (5 W each) and reassigning the
> INA3221 chips as a **sample-resistance science instrument** used to detect
> microcrack formation during mechanical pulls. Several rows below are
> therefore **moot / SED-revision requests** rather than GAPs — see the
> per-row Rev B.1 notes. The per-requirement references still describe the
> code paths that existed at the time of writing; see
> [docs/configuration.md](configuration.md) and [docs/onboard.md](onboard.md)
> for the current design.

---

## 1. Requirement-by-requirement compliance matrix

Legend: **OK** = implemented, **GAP** = missing / non-compliant, **PART** = partial,
**HW** = hardware-owned (software has supporting role only).

### 1.1 Functional (§2.1)

| ID | Requirement | Status | Notes / Evidence |
|----|-------------|--------|------------------|
| F.1 | 9 coating specimens accommodated | **SED revision** | Rev B.1 carries **8 specimens** (6 heated + 2 pulled-unheated). SED §2.1 specimen count to be amended from 9 to 8. |
| F.2 | Induce controlled microcrack damage | **SED revision** | Rev B.1: microcrack formation is now driven by **mechanical pulls** (two OMC ball-screw stepper motors, 1 mm/rev) rather than a +70 °C activation ramp. The SED F.2 text "by thermal activation" should be updated to "by mechanical pull"; firmware path: `StepperChannel` + `MotionLock` + `EVT,PULL`. |
| F.3 | Microcracks of required size | HW | — |
| F.4–F.9 | Measure/record ambient T, P, UV with timestamps | OK — **but humidity dropped** | `SensorSnapshot` carries ambient T, P, UV at Rev B.1 (no humidity — MS5803-01BA replaces BME280). Records timestamped in [telemetry.hpp](../onboard/include/coatheal/telemetry.hpp) and [storage_manager.cpp](../onboard/src/storage_manager.cpp). If the SED F.4–F.9 list requires humidity, file a deviation. |
| F.10 | Per-specimen temperature acquisition | OK | `sample_temps_c` vector, **8 PT100 via 2 × 4-ch Modbus RTD collectors** on USB-RS485 ([sensor_manager.cpp](../onboard/src/sensor_manager.cpp)). |
| F.11 | Non-volatile storage of all acquired data | OK | SD + USB redundant mirroring in [storage_manager.cpp](../onboard/src/storage_manager.cpp). |
| F.12 | Housekeeping via E-Link during flight | OK | TCP telemetry client ([telemetry_client.cpp](../onboard/src/telemetry_client.cpp)). |
| F.13 | Telemetry ≥ 1 Hz | OK | `runtime.tick_hz=1.0` in [config/onboard.example.ini](../config/onboard.example.ini). |
| F.14 | Heater control for predefined temperature profile | OK (floor-only) | Rev B.1: 6 per-sample PIDs enforcing `phase.sample_floor_c = +5 °C` with 0.5 °C hysteresis ([thermal_controller.cpp](../onboard/src/thermal_controller.cpp) + [pid_controller.cpp](../onboard/src/pid_controller.cpp)). |
| F.15 | Target 70 °C | **Moot / SED revision** | Rev B.1 removed the +70 °C activation entirely — no `phase.activation_target_c`, no activation-ramp phase. Microcrack formation is now mechanical. SED §2.1 should drop the +70 °C target. |
| F.16 | Optional secondary heating cycle if time permits | **Moot / SED revision — retired** | Rev B.1 has **no primary heating cycle** (no activation ramp), so a "secondary heating cycle" has no parent. Row retired from firmware scope. |
| F.17 | Fail-safe shutdown on over-temperature | OK | `heater.max_sample_temp_c` explicit (default 85 °C) — `HeaterScheduler` + `ThermalController` latch per-channel on breach and latch all heaters on energy-budget exhaustion. |

### 1.2 Performance (§2.2)

| ID | Requirement | Status | Notes |
|----|-------------|--------|-------|
| P.1–P.2 | Ambient T: −90…+50 °C ±3 °C | HW / OK | Rev B.1: MS5803-01BA (−40 °C…+85 °C). `sensor.ambient_temp_min_c/max_c` defines the firmware-accepted window; out-of-range reads flip `T_AMBIENT_FAIL` without clamping ([sensor_manager.cpp](../onboard/src/sensor_manager.cpp)). |
| P.3–P.4 | Ambient P: 5–1050 mbar ±1 mbar | HW / OK | Rev B.1: MS5803-01BA covers 10–1300 mbar natively (better stratospheric coverage than the Rev A BME280). Out-of-range reads flip `P_AMBIENT_FAIL`. |
| P.5–P.6 | UV 280–400 nm ±5 % | HW | Rev B.1: GUVA-S12SD + ADS1015 (12-bit) on I2C. |
| P.7 | Specimen T accuracy (clipped) | HW | Labfacility XF-931 PT100 Class A through 4-ch Modbus collectors. |
| P.8 | Ramp ≥5 °C/min | **Moot** | Rev B.1 removed the activation ramp. Microcrack formation is mechanical (pull-driven), so the ≥5 °C/min requirement no longer maps onto firmware behaviour. |
| P.9 | Uniformity ±2 °C across specimens | PART | Per-sample PID enforces floor. Uniformity is checked across the **6 heated** samples (Rev B.1), not 9 — `phase.uniformity_tolerance_c=2.0` drives the `UNIFORMITY_FAIL` bit. SED §P.9 "9 specimens" should be amended to "6 heated specimens". |
| P.10 | Sustain target ≥10 min | **Moot** | No thermal target to sustain at Rev B.1. Covered by floor duration = full flight envelope (always-on floor) instead. |

### 1.3 Design (§2.3)

| ID | Requirement | Status | Notes |
|----|-------------|--------|-------|
| D.1–D.12 | Mechanical / electrical interface, mass, loads | HW | — |
| D.3 | E-Link communication compliance | OK | Ethernet TCP, newline-delimited records, port config in [config/onboard.example.ini](../config/onboard.example.ini). |
| D.13 | ≤150 Wh energy | OK | `power.energy_budget_wh=130.0` with ~20 Wh avionics reserve. Rev B.1 headroom is larger because per-heater power dropped from 10 W → 5 W and `max_thermal_w` from 40 → 20 W. |
| D.14 | Power switching via umbilical at T-1h | HW | SW must come up cleanly after cold power apply — verified by `sd_notify` ready signal. |
| D.15 | **Radio silence <1 s latency** | OK | `RADIO_SILENCE` / `RADIO_RESUME` commands added in [command_parser.cpp](../onboard/src/command_parser.cpp) and wired through `TelemetryClient`. TX socket closes < 1 s after the command; queue keeps filling. |

### 1.4 Operational (§2.4)

| ID | Requirement | Status | Notes |
|----|-------------|--------|-------|
| O.1 | Activation by ground command or autonomous sequence | OK | `FORCE_START` + pressure-trigger (`transition.ascent_to_float_mbar=100.0`). |
| O.2 | Autonomous heating sequence | OK | `StateManager::Update` — pressure-driven floor-only controller. |
| O.3 | **Initiate heating when P < 100 mbar** | OK | `transition.ascent_to_float_mbar=100.0`. Rev B.1 semantics: at P ≤ 100 mbar the FSM enters `FLOAT`, where the +5 °C floor and pull cycles apply. |
| O.4 | Continue DAQ on loss of comms | OK | Queue + local log are independent of link ([telemetry_queue.cpp](../onboard/src/telemetry_queue.cpp)). |
| O.5 | **Modes STANDBY / RUN / SAFE** | OK | `SystemMode` enum with `kStandby`, `kRun`, `kSafe` is orthogonal to `MissionPhase` and is emitted in the `MODE=` wire token. |
| O.6 | Retain stored data ≥48 h after landing w/o external power | PART | SW writes to non-volatile media; depends on HW battery. SW must: (a) flush + fsync on SAFE entry, (b) survive unclean shutdown (append-only CSV already ok). |
| O.7 | Specimen access without gondola disassembly | HW | — |
| O.8 | Accept radio-silence request on launch pad at any time | OK | `RADIO_SILENCE` reversible command. |
| O.9 | Visual status indication during ground ops | OK | `GpioStatusLed` drives heartbeat (BCM 17) + mode LED (BCM 27); see [hal/status_led.hpp](../onboard/include/coatheal/hal/status_led.hpp). |
| O.10 | Log heating-cycle data (start time, peak T, hold, cooldown rate) | **Moot** | Rev B.1 has no heating cycle. Replaced by per-pull `EVT,PULL,<session>,<pull_id>,<motor_id>,<start_ts>,<steps_moved>,<hold_s>,<samples>` event frame ([telemetry.cpp](../onboard/src/telemetry.cpp)). |
| O.11 | Downlink ≤100 kbps | OK | Per-record size × 1 Hz ≪ 100 kbps. Rev B.1 frame is slimmer (no humidity, no box_temp); bandwidth re-baselined in `tests/downlink_bandwidth_test.cpp`. |
| **O.12** | **Sample resistance measurement (Rev B.1, new)** | OK — instrument stubbed | Two INA3221 chips (I2C 0x40 / 0x41, 3 channels each) repurposed as a sample-resistance science instrument. `SensorSnapshot::sample_resistance_ohm` serialised in the `RESISTANCE=` wire segment; health bit `RESISTANCE_OK` derived from `Ina3221Adapter::healthy()`. Real I2C bring-up is deferred; the adapter currently returns zeros and the simulator decays resistance ~5 % per observed pull. |

### 1.5 Constraints (§2.5)

| ID | Constraint | Status |
|----|-----------|--------|
| C.3 | Peak ≤3 A, avg ≤1.4 A | OK | Rev B.1: scheduler enforces ≤4 active × 5 W = 20 W peak thermal (plus ~5–10 W avionics). Well under the 3 A / 28.8 V = 86 W ceiling. |
| C.5 | Energy ≤150 Wh | OK | 130 Wh budget, see D.13. Rev B.1 draws less than Rev B (5 W vs. 10 W per heater). |
| C.6 | RJF21B / 10/100 Base-T | HW | — |
| C.7 | Survival −60…+50 °C; operational −40…+40 °C | HW | No box heater at Rev B.1 — the electronics box relies on passive thermal design + self-heating from avionics. If this proves insufficient during environmental test, a box heater would need to be re-added to the BOM. |
| C.11 | No emission on protected RF | OK | `RADIO_SILENCE` / `RADIO_RESUME` (D.15). |

---

## 2. Flight-phase behaviour vs. §4.7.3 and §4.8 (Rev B.1)

| Phase | Rev B.1 implementation | Notes |
|---|---|---|
| Pre-launch / ground (`BOOT`, mode `STANDBY`) | No heaters, sensors + telemetry live, visual LEDs, `RADIO_SILENCE` accepted. | OK. |
| Ascent (`ASCENT`, mode `RUN`) | Floor-only PID: samples kept ≥ +5 °C. | Replaces Rev A "hold at −30 °C hibernation". |
| Activation (P ≤ 100 mbar) | **Removed.** Phase no longer exists in firmware. Microcrack formation happens mechanically at `FLOAT`. | SED §4.7.3 should be updated. |
| Float (`FLOAT`, stratosphere) | Floor +5 °C; ground may issue `PULL_ARM` / `PULL_EXECUTE` on either motor. Motor holds the `MotionLock`; heater duties are forced to zero for the pull duration (`HEATER_INHIBITED` bit). | OK. |
| Descent / recovery (`DESCENT`) | Floor +5 °C; logging continues. Trigger `transition.float_to_descent_mbar=300.0`. | Confirm against BEXUS descent profile. |
| Post-landing (`LANDED` → `STOPPED`) | Heaters off, logging continues, retain data ≥48 h. | Depends on HW battery + graceful-shutdown SW. |

---

## 3. Telemetry / command protocol (§4.8.d)

PDF prescribes newline-delimited records containing:

```
SEQ (monotonic counter)
TIMESTAMP (RTC time + "RTC valid" flag)
T_ambient, P_ambient, UV, T_box, ...
HEATER_DUTY
STATUS bitfield (SD_OK, USB_OK, I2C_OK, SPI_OK, LINK_OK)
```

Implementation in [telemetry.hpp](../onboard/include/coatheal/telemetry.hpp) /
[telemetry.cpp](../onboard/src/telemetry.cpp) /
[status_flags.hpp](../onboard/include/coatheal/status_flags.hpp) at Rev B.1:

- Present: `seq`, `rtc_valid`, `timestamp_utc`, ambient T/P/UV, 8 per-specimen T, 6-element `heater_duty`, 8-element `sample_resistance_ohm` (`RESISTANCE=` segment), two `STEPPER<n>=` segments.
- **Removed at Rev B.1:** humidity, box T. Neither appears in the Rev B.1 wire frame. SED §4.8.d reference to `T_box` should be struck.
- STATUS bitfield enumerates 13 tokens: `SD_OK`, `USB_OK`, `I2C_OK`, `SPI_OK`, `LINK_OK`, `T_AMBIENT_OK`, `P_AMBIENT_OK`, `UNIFORMITY_OK`, `OVERTEMP_OK`, `ENERGY_OK`, `RS485_OK`, `HEATER_ACTIVE`/`HEATER_INHIBITED`, `RESISTANCE_OK` (new Rev B.1).
- `HEATING_CYCLE_EVENT` retired (Rev B.1 — no heating cycle). Replaced by `EVT,PULL` per-pull event frames.

Uplink commands per §4.5.3 ("manually overriding mission phases, forcing a heater
shutdown, or resetting the control unit") at Rev B.1: `FORCE_START`, `FORCE_STOP`,
`RESET_CTRL`, `SHUTDOWN_SAFE`, `SET_TICK_HZ`, `RADIO_SILENCE`, `RADIO_RESUME`,
`HEATERS_OFF`, plus stepper / pull commands (`STEPPER_MOVE`, `STEPPER_BEND`,
`PULL_ARM`, `PULL_EXECUTE`, `STEPPER_STOP`, …). See
[command_parser.cpp](../onboard/src/command_parser.cpp).
**Retired:** `SECONDARY_CYCLE` (F.16 is moot at Rev B.1).

---

## 4. Test-procedure coverage (§5, referenced tables)

| Test | Subject | SW gap to close |
|------|---------|-----------------|
| T-03 | Temperature sensor + flight-condition compatibility | Out-of-range flagging in sensor manager |
| T-05 | Individual specimen heating + DAQ | Already covered |
| T-07 | E-Link comms + telemetry | Already covered; add packet-rate CI test |
| T-08 | **Fail-safe shutdown** | Explicit over-T threshold + unit test |
| T-13 | **Operational Mode Transitions** | Implement STANDBY/RUN/SAFE state machine + transition tests |
| T-18 | **Downlink Data Rate** | Add bandwidth assertion (<100 kbps) as CI test |

---

## 5. Compliance gaps — actionable follow-ups (Rev B.1)

Closed since the Rev A report was written: O.5 (`SystemMode`), O.3 (ascent threshold = 100 mbar), D.15 / O.8 (`RADIO_SILENCE`/`RADIO_RESUME`), O.9 (status LEDs), F.17 (`heater.max_sample_temp_c` default 85 °C), P.9 (`UNIFORMITY_FAIL` bit), P.1 / P.3 (range-flag bits).

Still open for flight:

1. **[O.6] Unclean-shutdown hardening**: fsync after every record during SAFE; verify append-only CSV robustness under power-cut fault injection.
2. **[T-18] CI assertion** that steady-state telemetry bandwidth < 100 kbps with the Rev B.1 slim frame.
3. **Descent-threshold verification**: confirm `transition.float_to_descent_mbar=300.0` matches the BEXUS descent profile.
4. **[O.12] INA3221 real I2C driver**: stub path today; needs bus-level read of bus+shunt V on 2 chips × 3 channels before flight.
5. **[P.10 / F.2 / F.15 / F.16 / O.10] SED revision request**: amend §2.1 specimen count (9 → 8), drop the +70 °C activation target, retire the secondary-cycle and heating-cycle-event rows, and record that microcrack formation is now mechanical (pulls). Coordinate with the SED author.
6. **Mechanical / pull verification**: no equivalent SED row yet for the new pull-based microcrack mechanism. Propose a new `M.x` (mechanical) family of requirements covering pull rate, linear travel (1 mm/rev), hold duration, sample coverage per pull cycle.

No chemistry, mechanical, thermal-FEA, or coating-formulation items are in scope
for software and are intentionally omitted.

---

*Originally generated 2026-04-13 against SED v2.0, 08 Mar 2026. Rev B / Rev B.1 deltas applied 2026-04-17.*
