# Ground Station Redesign Implementation Plan

> **For agentic workers:** work task-by-task; every task ends with both test suites green and a commit. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the dock-based GUI with the fixed mission console specified in
`docs/superpowers/specs/2026-08-28-ground-station-redesign-design.md`, fix the
backend defects found in the sweep (radio silence that is not silent, split CSV
schemas, unpersisted operator logs, stale limits), extend telemetry with the
state the GUI cannot otherwise know, and add the operator-armed link-loss
failsafe.

**Architecture:** `ground-station/app/gui/` is rebuilt around one `MainWindow`
with fixed regions and four left tabs; every panel consumes `TelemetryPacket`
through a single `on_packet` fan-out and sends through one `CommandDispatcher`
that also feeds the console and the command log. Pure, Qt-free modules hold
the logic that must be unit-tested without a display: `alarms.py`,
`reply_format.py`, `bend_tracker.py`, `telemetry_log.py`, `thermal_presets.py`,
`session_dir.py`. Onboard changes are limited to: telemetry additions
(`CTRL=`, `zeroed/seq/seqst`), radio-silence gating, and the fallback plan.

**Tech stack:** Python 3.11+ / PyQt6 6.5+ / pyqtgraph 0.13+ / numpy (unchanged
`requirements.txt`); C++17 / CMake for the onboard.

**Spec:** `docs/superpowers/specs/2026-08-28-ground-station-redesign-design.md`.

## Global constraints

- Branch: `feature/gs-redesign` (created from `main` at `0230cfa`, PR #13 merged).
- Build/test recipe (Linux dev host):
  ```bash
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build --parallel && ctest --test-dir build --output-on-failure
  cd ground-station && QT_QPA_PLATFORM=offscreen .venv/bin/python -m unittest discover -s tests -p "test_*.py"
  ```
- Baseline at branch: Python 147 tests OK; C++ suite green in CI for `0230cfa`. After Phase A/B (2026-08-28): Python 215, C++ 17/17.
- **Test-quality bar** (same as the previous plans): every new test is proven load-bearing — temporarily mutate the behaviour it covers, observe the assertion message, revert in the same step, and record the message in the task notes. Assert both directions of every policy. GUI tests run headless (`QT_QPA_PLATFORM=offscreen`) and capture `CommandDispatcher.send` instead of hitting sockets.
- Wire protocol: existing DATA tokens, `STATUS=` flags and `COMPONENT_STATE` keys are never changed; additions only (`CTRL=`, three new stepper keys, `STATUS` reply `silence=`), each documented in `docs/protocol.md` in the same task that emits it.
- Never leave the tree red between tasks; commit as soon as green.
- Names on screen: H0–H5, M0/M1, S0–S7. No emoji. Glyphs ✔ ✖ ⚠ ▶ ■ ● allowed.
- Do not touch: `hardware_setup.py`, launchers (`.bat`/`.sh`), firewall module, discovery wire format.

---

## Phase A — Foundation (backend sanity)

### Task A1: One telemetry/operator log module and per-session directories

**Files:** create `ground-station/app/telemetry_log.py`, `ground-station/app/session_dir.py`; modify `app/gui/dispatch.py` (receiver + dispatcher), `app/telemetry_server.py`; tests `tests/test_telemetry_log.py`, `tests/test_session_dir.py`; update `tests/test_telemetry_server_csv.py`.

- [x] `telemetry_log.py`: `TELEMETRY_CSV_FIELDS` (schema v6 = CLI v5 columns + `stepperN_zeroed`, `stepperN_seq`, `stepperN_seqstate`, `fallback`, `link_loss_s`, `energy_wh`, `budget_wh`, `budget_exhausted`, `heaters_active`, `queue`), `packet_to_row(pkt)`, `TelemetryCsvWriter`, `PullCsvWriter`, `CommandLog` (`ts_utc, command, ok, latency_ms, body, raw`), `EventLog` (`ts_utc level message`), `SessionMeta` (`session.json`).
- [x] `session_dir.py`: `SessionDirectory.for_session(root, onboard_session_id, now)` → `logs/sessions/<YYYYMMDD-HHMMSS>_<id>/`; reuses an existing directory for the same id; writes `logs/latest_session.txt`.
- [x] Receiver: open writers lazily on the first frame of a session; switch directory when `session_id` changes; keep `ground_ack_cursor.json` at `logs/`; throttle cursor persistence to ≤1 write/s.
- [x] CLI server: same writers (delete its inline schema); `--log` now means the sessions root.
- [x] Dispatcher: every response appended to `CommandLog`; GUI event log appends to `EventLog`.
- [x] Tests: identical row for identical packet from GUI and CLI paths; directory naming/reuse; command/event files written; cursor throttle. Mutation: drop a v6 column → header assertion names it.

### Task A2: Telemetry additions (onboard + parser)

**Files:** `onboard/include/coatheal/telemetry.hpp`, `onboard/src/telemetry.cpp`, `onboard/src/system_controller.cpp` (populate record), `onboard/include/coatheal/stepper_channel.hpp` (zeroed comes from `SystemController::motor_zeroed_`, not the channel), `docs/protocol.md`; GS `app/protocol.py`, `tests/test_protocol.py`; C++ `tests/unit/test_telemetry_rev_c.cpp`.

- [x] `StepperStatus` gains `zeroed`, `seq_name`, `seq_state`; segment emits `|zeroed:<0|1>|seq:<name|->|seqst:<idle|run|pause>`.
- [x] New `CtrlStatus` in `TelemetryRecord` → `,CTRL=fallback:…|link_loss_s:…|energy_wh:…|budget_wh:…|budget_exhausted:…|heaters_active:…|queue:…|plan:none` emitted after `COMPONENT_STATE`, before `STEPPER0`.
- [x] `parse_telemetry_csv`: `steppers[i]` gains `zeroed: Optional[bool]`, `seq_name`, `seq_state`; `TelemetryPacket.ctrl: Dict[str, str]` (raw) + typed accessors (`fallback_active`, `energy_wh`, …) returning `None` when absent.
- [x] Tests: old frames (no `CTRL=`, no `zeroed`) still parse with `None`s; new frames populate; C++ serializer test pins the exact token order. Mutation: remove `zeroed` emission → Python test naming the key fails.

### Task A3: Radio silence that is silent

**Files:** `onboard/src/telemetry_client.cpp` (+ `.hpp`), `onboard/src/system_controller.cpp` (command whitelist, flag file, `STATUS` `silence=`), `onboard/src/storage_manager.*` or `telemetry_queue.*` (flag file location `<queue_dir>/radio_silence`), `tests/unit/test_suite.cpp`, `tests/unit/test_telemetry_rev_c.cpp`; GS `app/gui/discovery.py` (`GsBeacon.set_quiet`, `CommandProbe.set_quiet`), `app/gui/dispatch.py` (`CommandDispatcher.silence_mode` gate raising a local NACK for anything but the whitelist), `tests/test_discovery.py`, `tests/test_command_timeouts.py`; `docs/protocol.md`, `docs/manual-operations.md`.

- [x] Onboard: `BeaconSenderLoop` and `SendOnboardHelloReply` no-op while `!transmit_enabled_`; the listener still records `latest_gs_`.
- [x] Onboard: while silent, `HandleCommandLine` accepts only `RADIO_RESUME`, `RADIO_SILENCE`, `STATUS`, `PING`; everything else → `NACK,<cmd>,radio silence active` before any side effect.
- [x] Onboard: flag file written on `RADIO_SILENCE`, removed on `RADIO_RESUME`; on start, if present → begin silent (decision D2). `STATUS` reply gains `;silence=<0|1>`.
- [x] GS: `silence_mode` on the dispatcher blocks sends except the whitelist (local `CommandResponse` with error `blocked: radio silence`); beacon/probe threads pause; unknown-at-start handling per spec §9.4.
- [x] Tests (C++): with transmit disabled, the beacon loop's send counter stays 0 over N iterations and a `GS_HELLO` produces no reply; a `STEPPER_MOVE` during silence returns the NACK and leaves `moving=false`. Tests (Python): dispatcher blocks/allows per whitelist; beacon thread sends nothing while quiet. Mutation: re-enable beacon send → counter test fails.

### Task A4: Stale limits and dead code

**Files:** `app/protocol.py` (`validate_speed_hz` max 100, `validate_microstep` unchanged), `app/gui/theme.py` (drop Rev-A phase entries), `app/thermal_profiles.py` → replaced in B4, `tests/test_protocol.py`.

- [x] `validate_speed_hz(default max 100.0)`; test both sides of the bound.
- [x] Remove `PHASE_COLORS` Rev-A keys; `phase_color` exact-match on Rev C names.

---

## Phase B — GUI

### Task B1: Shell, layout, scaling, persistence

**Files:** rewrite `app/gui/main_window.py`; create `app/gui/layout.py` (region container with named splitters), `app/gui/scale.py` (font scale persisted in `QSettings`); delete dock code; tests `tests/test_gui_layout.py`.

- [x] Fixed regions per spec §4; all persistent widgets have `objectName`; `saveState/restoreState` round-trips (test: restore after resize changes splitter sizes).
- [x] Minimum-size test: at 1366×768 and 1920×1080 every region's `minimumSizeHint()` fits its allotted rectangle and no child reports a width larger than its parent (walk the tree). Mutation: give one label a 600 px fixed width → test names it.
- [x] UI scale actions (`Ctrl+=`, `Ctrl+-`, `Ctrl+0`) change `QApplication.font()` point size within 8–16 and persist.
- [x] Status bar shows session directory, frame count, parse errors, disk usage — never the stale "start telemetry" text (test).

### Task B2: Top strip and alarm strip

**Files:** create `app/gui/alarms.py` (pure: `AlarmModel.evaluate(pkt, link_age_s, silent) -> list[Alarm]`, ack state), `app/gui/panel_top.py`; tests `tests/test_alarms.py`, `tests/test_gui_top.py`.

- [x] Alarm table from spec §5.2; ack semantics (muted until cleared; re-raises when the condition returns after clearing).
- [x] Top strip fields incl. `T+` from first frame of the session and `RADIO` state; silence band.
- [x] Beep: `QApplication.beep()` on a *new* alarm only, gated by a persisted toggle (default off).
- [x] Mutation: remove the `LINK STALE` suppression during silence → test fails.

### Task B3: System tab

**Files:** create `app/gui/tab_system.py`; tests `tests/test_gui_system.py`.

- [x] Groups per spec §5.3; mode-derived enabling (ARM only in STANDBY, DISARM only in RUN, EXIT SAFE only in SAFE) with reason tooltips; confirm set per §6.1; `CHECK` component selector; last-response line.
- [x] Tests capture `send`; assert `SET_PHASE <combo>` and confirm-gating; assert disabled states per mode packet. Mutation: allow ARM in RUN → test fails.

### Task B4: Thermal tab and presets v2

**Files:** create `app/gui/tab_thermal.py`, `app/thermal_presets.py` (replaces `thermal_profiles.py`, migrates v1); tests `tests/test_thermal_presets.py`, `tests/test_gui_thermal.py`.

- [x] Header (energy bar from `CTRL`, active heaters, PID summary, inhibit); six rows; all-targets; presets with atomic write + `.bak`; `Capture from onboard` parses `GET_THERMAL`.
- [x] Row state word logic is pure (`heater_state(duty, target, valid, inhibited)`), tested in all five outcomes.
- [x] Target limits taken from the last `GET_THERMAL` (`target_min_c/target_max_c`) when available.

### Task B5: Motion tab

**Files:** create `app/gui/tab_motion.py`, `app/gui/bend_tracker.py` (pure: records R at bend start per motor from the monitored specimen(s), computes Δ%); tests `tests/test_bend_tracker.py`, `tests/test_gui_motion.py`.

- [x] Motor cards with `zeroed` from telemetry (grey when the key is absent); selector; actions; jog; speed 1–100 default 100; **BEND** → `STEPPER_MOVETO <id> <target> <hold>`; **STANDARD PULL** → `PULL_EXECUTE <id>`.
- [x] Gating with reasons (RUN, enabled, zeroed, no fallback, no silence). Mutation: drop the zeroed check → test "BEND enabled on unzeroed motor" fails.
- [x] Esc / STOP MOTORS keep sending both `STEPPER_STOP` ids (existing tests kept).

### Task B6: Advanced tab

**Files:** create `app/gui/tab_advanced.py` (bend sequences moved from `panels_control.py`, PID, open-loop duty, microstep, preset management, network fields, fallback plan placeholder until C2); tests `tests/test_gui_advanced.py`.

- [x] Sequence step speed validated ≤100 Hz before sending.

### Task B7: Plots

**Files:** rewrite `app/gui/plots.py`; create `app/gui/series_store.py` (pure numpy store with append/window/downsample); tests `tests/test_series_store.py`, `tests/test_gui_plots.py`.

- [x] Time axis (mission elapsed, UTC in crosshair), window selector, follow/pause, full-session retention, per-tab content per spec §5.6 (Ambient = three x-linked stacked plots), pull markers, targets overlay, export PNG/CSV.
- [x] Store test: 100k appends stay O(1) amortised (timing bound generous), window slicing exact at boundaries.

### Task B8: Right column — Health, Checkout, Values

**Files:** keep `panels_health.py` (single-column layout); create `app/gui/panel_checkout.py` (pure `checkout_items(pkt, link_ok, last_check)`), `app/gui/panel_values.py`; tests `tests/test_checkout.py`; existing `test_gui_health.py` stays green.

### Task B9: Console, events, pulls

**Files:** create `app/gui/panel_console.py`, `app/gui/reply_format.py` (pure pretty-printer for `key=value;…` and `{…}` bodies), `app/gui/panel_events.py`; tests `tests/test_reply_format.py`, `tests/test_gui_console.py`.

- [x] Console entry: completion over `KNOWN_COMMANDS`, history, `Send`, re-issue on row click; every panel's command appears (single dispatcher hook).
- [x] During silence the console shows the block reason inline.

### Task B10: Shortcuts, help, cleanup

**Files:** `main_window.py`, delete `widgets.Toast`, delete `panels_control.py` / `panels_info.py` remnants; tests `tests/test_gui_shortcuts.py`; update `test_gui_smoke.py`, `test_gui_uiux.py` to the new structure (keep their safety assertions).

- [x] Shortcut table per spec §6.5; `P` ignored in text fields (test); no unmodified letter sends a command (test walks all `QShortcut`s).

### Task B11: Docs

- [x] Rewrite `docs/ground-station.md` and the GUI section of `ground-station/README.md` to the new panels; README command summary; `docs/protocol.md` additions already done in A2/A3.

---

## Phase C — Link-loss failsafe (after owner decisions D3–D6)

### Task C1: Onboard fallback plan

**Files:** `onboard/include/coatheal/config.hpp` + `config.cpp` (`fallback.bend_min_c`, `bend_max_c`, `bend_deadline_s`, `landed_safe`), `command.hpp`/`command_parser.cpp` (`FALLBACK_PLAN`, `FALLBACK_ARM`, `FALLBACK_DISARM`, `FALLBACK_STATUS`), `system_controller.*` (plan store persisted under `storage.queue_dir`, executor in the tick, `plan:` in `CTRL`), tests `tests/unit/test_phase_rev_c.cpp` / new `test_fallback_plan.cpp`; `docs/protocol.md`, `docs/manual-operations.md`, `config/onboard.example.ini`.

- [x] Trigger and execution exactly per spec §10; `done` persisted; LANDED behaviour per D6. (merged 2026-08-28, `fb5d85f`; C++ 18/18)
- [x] Tests drive the pure `FallbackPlanner` with scripted timelines (10 cases + command/config tests); the tick wiring and LANDED safing are covered by reading and need one bench run (fake sensors) and assert: no motion before fallback; bend at PRE_FLOAT with window met; deadline behaviour; never twice; disarm stops it.

### Task C2: GUI fallback panel

**Files:** `app/gui/tab_advanced.py` (plan editor + ARM/DISARM + status), `alarms.py` (FALLBACK, PLAN RUNNING/DONE), tests.

---

## Phase D — Validation and hand-over

- [x] D1 Both suites green; mutation notes collected in the PR description.
- [x] D2 (read-only part) Bench: GUI against the Pi read-only (2026-08-28: STATUS/COMPONENTS/GET_THERMAL/CHECK/BENDSEQ_STATUS via the console; Pi connected telemetry and drained its backlog into a session directory; CLI telemetry-server wrote the same layout). Still open: the onboard-dependent checks after deploying the branch to the Pi (PING/STATUS/COMPONENTS/CHECK); with the owner: ARM, enable, zero, jog, bend on M1; radio silence with `tcpdump -i eth0 src <pi>` on the Pi showing no onboard-originated packets except replies to the operator's commands.
- [x] D3 Launchers: `COATHEAL-GroundStation.sh --check` and the Windows `.bat --check` unchanged and passing.
- [ ] D4 Pull request `feature/gs-redesign` → `main` with before/after screenshots (branch not pushed yet — owner's call).
