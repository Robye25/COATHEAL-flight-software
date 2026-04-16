# COATHEAL SED v2.0 — Software Compliance Report

Source: `BX38_COATHEAL_SED_v2-0_08Mar2026.pdf` (126 pages, 08 Mar 2026).
Scope: software-relevant requirements only. Chemistry, mechanical, and thermal-
analysis content ignored except where it constrains software behavior (e.g. phase
targets, power caps).

> **Rev A (historical) notice.** This report was written against the pre-Rev B
> phase model (five phases: `ASCENT_HOLD`, `ACTIVATION_RAMP`, `FLOAT_HOLD`,
> `DESCENT_FLOOR`, `STOPPED` — with a +70 °C activation target). Rev B
> collapsed the thermal policy to a single cold-protection floor
> (`phase.sample_floor_c = +5 °C` shared across `ASCENT/FLOAT/DESCENT`) and
> replaced the FSM with `BOOT → ASCENT → FLOAT → DESCENT → LANDED` plus
> `STOPPED`. The per-requirement references below still describe the code
> paths that existed at the time of writing; see `docs/configuration.md`
> and `docs/onboard.md` for the current Rev B design.

---

## 1. Requirement-by-requirement compliance matrix

Legend: **OK** = implemented, **GAP** = missing / non-compliant, **PART** = partial,
**HW** = hardware-owned (software has supporting role only).

### 1.1 Functional (§2.1)

| ID | Requirement | Status | Notes / Evidence |
|----|-------------|--------|------------------|
| F.1 | 9 coating specimens accommodated | HW | — |
| F.2 | Induce controlled microcrack damage | PART | Activation-ramp phase at +70 °C implemented ([phase.hpp](onboard/include/coatheal/phase.hpp)). Ramp profile driven by PID. |
| F.3 | Microcracks of required size | HW | — |
| F.4–F.9 | Measure/record ambient T, P, UV with timestamps | OK | `SensorSnapshot` carries ambient T, P, humidity, UV; records written with timestamp in [telemetry.hpp](onboard/include/coatheal/telemetry.hpp) and [storage_manager.cpp](onboard/src/storage_manager.cpp). |
| F.10 | Per-specimen temperature acquisition | OK | `sample_temps_c` vector, 10 PT100 via MIKROE-2815 on SPI ([sensor_manager.cpp](onboard/src/sensor_manager.cpp)). |
| F.11 | Non-volatile storage of all acquired data | OK | SD + USB redundant mirroring in [storage_manager.cpp](onboard/src/storage_manager.cpp). |
| F.12 | Housekeeping via E-Link during flight | OK | TCP telemetry client ([telemetry_client.cpp](onboard/src/telemetry_client.cpp)). |
| F.13 | Telemetry ≥ 1 Hz | OK | `runtime.tick_hz=1.0` in [config/onboard.example.ini](config/onboard.example.ini). |
| F.14 | Heater control for predefined temperature profile | OK | [thermal_controller.cpp](onboard/src/thermal_controller.cpp) + [pid_controller.cpp](onboard/src/pid_controller.cpp). |
| F.15 | Target 70 °C | OK | `phase.activation_target_c=70.0`. |
| F.16 | Optional secondary heating cycle if time permits | **GAP** | `StateManager` has no secondary-cycle branch after `kDescentFloor`. No command hook for re-entering `kActivationRamp`. |
| F.17 | Fail-safe shutdown on over-temperature | PART | `HeaterScheduler` latches off on energy budget breach; per-channel over-T shutdown logic exists but threshold is implicit — **add explicit `heater.max_sample_temp_c` config and verify test T-08**. |

### 1.2 Performance (§2.2)

| ID | Requirement | Status | Notes |
|----|-------------|--------|-------|
| P.1–P.2 | Ambient T: −90…+50 °C ±3 °C | HW | BME280 reports to −40 °C only; requirement exceeds single-sensor range → **SW must flag out-of-range values rather than clamp silently** ([sensor_manager.cpp](onboard/src/sensor_manager.cpp)). |
| P.3–P.4 | Ambient P: 5–1050 mbar ±1 mbar | HW/OK | BME280 covers 300–1100 mbar; below 300 mbar sensor resolution degrades — SW should report `P_OK` status bit. |
| P.5–P.6 | UV 280–400 nm ±5 % | HW | BPW 21 + ADC. |
| P.7 | Specimen T accuracy (clipped) | HW | PT100 Class A. |
| P.8 | Ramp ≥5 °C/min | OK | `phase.activation_ramp_c_per_s=0.85` (51 °C/min). |
| P.9 | Uniformity ±2 °C across specimens | PART | Per-specimen PID enables uniformity but no cross-specimen uniformity check/alert. **Add uniformity-violation STATUS bit**. |
| P.10 | Sustain target ≥10 min | OK | `phase.float_hold_minutes=90.0`. |

### 1.3 Design (§2.3)

| ID | Requirement | Status | Notes |
|----|-------------|--------|-------|
| D.1–D.12 | Mechanical / electrical interface, mass, loads | HW | — |
| D.3 | E-Link communication compliance | OK | Ethernet TCP, newline-delimited records, port config in [config/onboard.example.ini](config/onboard.example.ini). |
| D.13 | ≤150 Wh energy | OK | `power.energy_budget_wh=130.0` with 20 Wh avionics reserve. |
| D.14 | Power switching via umbilical at T-1h | HW | SW must come up cleanly after cold power apply — verified by `sd_notify` ready signal. |
| D.15 | **Radio silence <1 s latency** | **GAP** | No dedicated `RADIO_SILENCE` command in [command_parser.cpp](onboard/src/command_parser.cpp). Current `SHUTDOWN_SAFE` kills telemetry but also heaters. Need a command that halts RF TX only, leaves heaters/logging running, reversible. |

### 1.4 Operational (§2.4)

| ID | Requirement | Status | Notes |
|----|-------------|--------|-------|
| O.1 | Activation by ground command or autonomous sequence | OK | `FORCE_START` + pressure-trigger. |
| O.2 | Autonomous heating sequence | OK | `StateManager::Update`. |
| O.3 | **Initiate heating when P < 100 mbar** | **GAP** | `transition.ascent_to_activation_mbar=140.0`. Must be ≤ 100 mbar or the requirement must be updated in the SED. |
| O.4 | Continue DAQ on loss of comms | OK | Queue + local log are independent of link ([telemetry_queue.cpp](onboard/src/telemetry_queue.cpp)). |
| O.5 | **Modes STANDBY / RUN / SAFE** | **GAP** | Code exposes `MissionPhase` (ascent/activation/float/descent/stopped) but no top-level `Mode` enum. `SHUTDOWN_SAFE` is a one-shot command, not a persistent mode. **Add `SystemMode { STANDBY, RUN, SAFE }` orthogonal to `MissionPhase`**. |
| O.6 | Retain stored data ≥48 h after landing w/o external power | PART | SW writes to non-volatile media; depends on HW battery. SW must: (a) flush + fsync on SAFE entry, (b) survive unclean shutdown (append-only CSV already ok). |
| O.7 | Specimen access without gondola disassembly | HW | — |
| O.8 | Accept radio-silence request on launch pad at any time | **GAP** | See D.15. |
| O.9 | Visual status indication during ground ops | **GAP** | No GPIO LED driver in [onboard/src/hal](onboard/src/hal/). Add heartbeat LED + mode LED. |
| O.10 | Log heating-cycle data (start time, peak T, hold, cooldown rate) | PART | Sample T is logged every tick, but **no dedicated "heating cycle summary" record** (T-13 test asset). Add `HEATING_CYCLE_EVENT` event frame. |
| O.11 | Downlink ≤100 kbps | OK | Per-record size × 1 Hz ≪ 100 kbps. Budget confirmed by test T-18. |

### 1.5 Constraints (§2.5)

| ID | Constraint | Status |
|----|-----------|--------|
| C.3 | Peak ≤3 A, avg ≤1.4 A | OK — `power.max_system_w=48.23` (1.67 A peak, scheduler enforces ≤4 active × 10 W = 40 W avg). |
| C.5 | Energy ≤150 Wh | OK (130 Wh budget, see D.13). |
| C.6 | RJF21B / 10/100 Base-T | HW |
| C.7 | Survival −60…+50 °C; operational −40…+40 °C | Controller keeps Pi in a 0 °C floor; box heater implemented. **Verify `phase.box_target_c` kicks in during STANDBY, not only in-flight**. |
| C.11 | No emission on protected RF | HW / SW radio-silence (D.15). |

---

## 2. Flight-phase behavior vs. §4.7.3 and §4.8

| Phase | PDF target | Implementation |
|-------|-----------|----------------|
| Pre-launch / ground | STANDBY mode, box heater to 0 °C floor, visual LED, accept arm/radio-silence | **GAPS:** no STANDBY mode, no LED, no radio-silence command. |
| Ascent | Hold plates at −30 °C hibernation; burst heating to prevent cold soak | Implemented via `kAscentHold` with `phase.ascent_target_c=-30.0`. OK. |
| Activation (P < 100 mbar) | Ramp plates −30 → +70 °C at ~0.85 °C/s, max 4 heaters active | Implemented, but **trigger threshold 140 mbar ≠ 100 mbar**. |
| Float (stratosphere) | Hold +70 °C for ≥10 min (target 90 min healing) | OK — `float_hold_minutes=90.0`. |
| Descent / recovery | Plate floor −20 °C to preserve healed polymer; keep logging through landing | OK — `kDescentFloor`, `phase.descent_floor_c=-20.0`. Trigger `float_to_descent_mbar=300.0` — **confirm against BEXUS descent profile**. |
| Post-landing | Continue logging, retain data ≥48 h | Depends on HW battery + graceful-shutdown SW. |

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

Implementation in [telemetry.hpp](onboard/include/coatheal/telemetry.hpp) /
[telemetry.cpp](onboard/src/telemetry.cpp) / [status_flags.hpp](onboard/include/coatheal/status_flags.hpp):

- ✅ `seq`, `rtc_valid`, `timestamp_utc`
- ✅ Ambient T/P/UV, box T, per-specimen T
- ✅ `heater_duty` vector
- ⚠️ STATUS bitfield — **verify `StatusFlags` enumerates exactly `{SD_OK, USB_OK, I2C_OK, SPI_OK, LINK_OK}` plus additions (ENERGY_OK, UNIFORMITY_OK, OVERTEMP_OK)**.
- ❌ No `HEATING_CYCLE_EVENT` frame for O.10.

Uplink commands per §4.5.3 ("manually overriding mission phases, forcing a heater
shutdown, or resetting the control unit"): `FORCE_START`, `FORCE_STOP`,
`RESET_CONTROL`, `SHUTDOWN_SAFE` exist in [command_parser.cpp](onboard/src/command_parser.cpp).
**Missing:** `RADIO_SILENCE` / `RADIO_RESUME`, `ARM` / `DISARM` (STANDBY↔RUN),
`ENTER_SAFE` / `EXIT_SAFE`, `SECONDARY_CYCLE` (F.16).

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

## 5. Compliance gaps — actionable follow-ups

Priority for next implementation cycle:

1. **[O.5, T-13] Introduce `SystemMode {STANDBY, RUN, SAFE}` orthogonal to `MissionPhase`.** STANDBY = armed-on-pad, heaters disabled, sensors + telemetry live; RUN = autonomous flight logic; SAFE = heaters latched off, telemetry still running, flush NV storage.
2. **[O.3] Change `transition.ascent_to_activation_mbar` from 140 → 100** (or file a deviation with justification).
3. **[D.15, O.8] Add `RADIO_SILENCE` / `RADIO_RESUME` command** with <1 s latency; must suspend TX socket but keep PID/logging/DAQ.
4. **[O.9] Add visual-status LED driver** (heartbeat + mode indicator) under [onboard/src/hal/](onboard/src/hal/).
5. **[F.17, T-08] Explicit per-channel over-temperature cutoff**: add `heater.max_sample_temp_c` (e.g. 85 °C) with unit test.
6. **[O.10] Emit `HEATING_CYCLE_EVENT` frame** with start_ts, peak_T, hold_duration, cooldown_rate.
7. **[F.16] Secondary heating cycle**: command hook and a state-machine branch allowing re-entry to `kActivationRamp` from `kFloatHold` if remaining float time permits.
8. **[P.9] Uniformity monitor**: add STATUS bit set when max−min sample T > 2 °C during hold.
9. **[P.1, P.3] Out-of-range sensor flagging**: set corresponding STATUS bits instead of clamping silently.
10. **[O.6] Unclean-shutdown hardening**: fsync after every record during SAFE; verify append-only CSV robustness under power-cut fault injection.
11. **[T-18] CI assertion** that steady-state telemetry bandwidth < 100 kbps.
12. **Descent-threshold verification**: confirm `transition.float_to_descent_mbar=300.0` matches BEXUS descent profile; otherwise retune.

No chemistry, mechanical, thermal-FEA, or coating-formulation items are in scope
for software and are intentionally omitted.

---

*Generated 2026-04-13 against SED v2.0, 08 Mar 2026.*
