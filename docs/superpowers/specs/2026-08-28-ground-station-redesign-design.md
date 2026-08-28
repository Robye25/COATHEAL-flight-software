# Ground Station Redesign — Design

**Date:** 2026-08-28
**Status:** Draft for owner approval (layout mockup published alongside this spec)
**Authority:** Owner Q&A of 2026-08-28 (§1); onboard source at `main` after PR #13
(`0230cfa`); BEXUS User Manual v8-2 §4.1 (E-Link bandwidth is shared with flight
systems, "downlink coordination is essential"), §5.4 (ground station shall be
robust to connection loss, restart automatically, and let the experimenters tune
the downlink data rate), §5.10.5 (reconnect handling must be tested).
**Supersedes:** the GUI panel model described in `docs/ground-station.md` and
`ground-station/README.md` (both rewritten by the plan).

---

## 1. Owner decisions (2026-08-28)

| # | Decision |
|---|---|
| 1 | Laptop/OS/screen not chosen — the GUI must scale to any screen size and OS. |
| 2 | Timeline: bench + gondola checkout → ascent (monitor) → **bend the samples during ascent, right before float**, confirm via resistance → float (monitor) → descent (monitor, prepare landing). All operations via the ground station; an onboard failsafe takes decisions only as a last resort (pressure/altitude, UV, temperature, resistance). |
| 3 | Primary mechanical action: redesign as needed (§5.4 below). |
| 4 | Thermal method: whichever works best (§5.3: closed-loop targets primary, open-loop duty in Advanced). |
| 5 | PT100s are being mounted now; will be in place when the GUI is done. Pi will be online soon. |
| 6 | Names on screen: **H0–H5, M0/M1, S0–S7** (0-based wire indices). |
| 7 | Command set: owner asks for a recommendation (§5, §6.4). **Radio silence/resume is the most important control** — silence must cut off all communication on request. |
| 8 | Thermal profiles: redesign for efficiency and lossless retention (§5.3.4). |
| 9 | Bend-sequence editor: Advanced tab. |
| 10 | Keep glyphs (✔ ✖ ⚠ ▶ ■ ●); no emoji. |
| 11 | Redesign the shortcut system (§6.5). |
| 12 | Fixed layout, no floating docks, built for control efficiency and data monitoring. |
| 13 | Left column organised as tabs. |
| 14 | Time x-axis, window selector, full-flight retention. |
| 15 | Plot set: Temperatures, **Ambient = ambient T + pressure + UV**, Heaters, Resistance, Motors. |
| 16 | Confirmation policy as proposed (§6.1). |
| 17 | Persistent last-response line per control group + a real command console; toasts removed. |
| 18 | Alarm strip with acknowledge; beep off by default, toggleable. |
| 19 | Dark theme only. |
| 20 | Per-session log directories; commands and events persisted automatically. |
| 21 | One shared CSV writer for GUI and CLI. |
| 22 | Onboard telemetry additions allowed (Pi redeploy is feasible). |
| 23 | Motor speed control 1–100 full-step Hz, default 100. |
| 24 | Work on a new branch after merging the finished branch: PR #13 merged (`0230cfa`), branch `feature/gs-redesign` created off `main`. |
| 25 | Wireframe mockup for approval before coding. |
| 26 | Unit-test suite stays the gate (CI, Python 3.11). |

---

## 2. Operator workflow and what it demands of the GUI

| Phase | Operator activity | GUI must make this fast / visible |
|---|---|---|
| Bench / gondola checkout | Confirm every subsystem, ARM, enable + zero motors, set thermal targets, verify link and logging | Checkout list derived from live telemetry, one-click `CHECK`, motor cards with enabled/zeroed state, "logging to …" in the status bar |
| Ascent | Monitor; keep samples within thermal policy; **right before float, bend** | Motion tab as the primary control: a single BEND action per motor, live position, resistance readout with delta since bend start, pull events |
| Float | Monitor thermal self-healing; adjust targets; watch resistance recover | Thermal tab table (measured / target / duty / state per heater), Temperatures plot with target overlays, Resistance plot with pull markers |
| Descent / landing | Monitor; heaters off / SAFE before landing | System tab mode controls, panic buttons always visible, alarm strip |
| Link loss (any phase) | Nothing possible from the ground | Onboard failsafe (§10); GUI shows `FALLBACK` state and the armed plan when the link returns |
| Radio silence request | Silence everything, later resume | One control, unmistakable state, GS goes quiet too (§9) |

---

## 3. Findings from the sweep that this design corrects

1. Left/right docks clip their second column at 1600×900 (min widths do not match content; horizontal scroll disabled).
2. Motor speed UI is Rev-A stale: slider 10–4000 Hz, validator ≤5000; the onboard clamps to `pull.max_step_hz` = 100 full-step Hz and NACKs `BENDSEQ_LOAD` speeds above it. Microstep default 16 vs onboard 4.
3. `PULL_EXECUTE` (the config-defined standard pull) has no control; `PULL_ARM` and `PULL_EXECUTE` are the same onboard code path (both `ArmPull`).
4. NACK reasons vanish after a 2.5 s toast.
5. Zeroed state, fallback state, energy budget, active sequence are only in `STATUS`/`BENDSEQ_STATUS` replies, never in telemetry.
6. Command history and event log are not persisted; one telemetry CSV grows forever across sessions; GUI and CLI write different CSV schemas to the same default path.
7. Dock layout restore never works (`QDockWidget` objectName unset).
8. Stale text: status bar "start telemetry to begin" after auto-start; docs describe a UV plot, `CommandSender`, status-bar readouts that do not exist; Rev-A phase colours "for replay"; preflight "6 heater duties reporting" is vacuous; 8 resistance traces for 2 measured channels.
9. `SET PHASE` confirms but `FORCE START` (same effect) does not.
10. Plot x-axis is packet sequence; 1200-point window.
11. **Radio silence is not silent**: `RADIO_SILENCE` only closes the telemetry TCP client. `BeaconSenderLoop` broadcasts `ONBOARD_BEACON` every 2 s *whenever it is not connected* — i.e. continuously during silence — and `DiscoveryListenerLoop` answers every `GS_HELLO` with `ONBOARD_HELLO`. The GS itself keeps beaconing and PING-probing (each probe makes the Pi reply).

---

## 4. Layout

Fixed regions (no floating/movable docks; View menu can hide the right column
and the bottom region; splitters allow resizing within limits; everything is
persisted with named widgets so restore works).

```text
┌ Menu: File · View · Help ─────────────────────────────────────────────────────────────┐
│ TOP STRIP  MODE · PHASE · LINK age · RX rate · target · session/seq · T+ · UTC · RADIO │ HEATERS OFF · STOP MOTORS · ENTER SAFE │
│ ALARM STRIP (only while alarms are active)  [chip] [chip] …                    [ACK ALL] │
├──────────────┬──────────────────────────────────────────────┬────────────────────────────┤
│ LEFT column  │ CENTER plots                                 │ RIGHT column               │
│ tabs:        │ tabs: Temperatures · Ambient · Heaters ·     │ tabs: Health · Checkout ·  │
│ System       │       Resistance · Motors                    │       Values               │
│ Thermal      │ toolbar: window 5m·30m·2h·all · pause ·      │                            │
│ Motion       │          export                              │                            │
│ Advanced     │                                              │                            │
├──────────────┴──────────────────────────────┬───────────────┴────────────────────────────┤
│ BOTTOM  Console (history table + entry)     │ Events · Pulls                             │
├─────────────────────────────────────────────┴────────────────────────────────────────────┤
│ STATUS BAR  session dir · frames · parse errors · disk · UI scale                        │
└──────────────────────────────────────────────────────────────────────────────────────────┘
```

Scaling rules (decision 1):

- Reference sizes: 1920×1080 (comfortable) and 1366×768 (minimum supported; everything remains reachable, nothing clipped).
- Column widths are proportional (`QSplitter` stretch 3 : 6 : 3) with content-derived minimums; the left column's tab pages are vertically scrollable as a last resort, never horizontally.
- Base font size follows the platform default; `Ctrl+=` / `Ctrl+-` / `Ctrl+0` scale the whole UI (persisted). Qt 6 high-DPI scaling stays on.
- No fixed pixel widths on labels; monospace is used for every numeric readout so columns stay aligned.
- Panic buttons keep a minimum 34 px height at every scale.

Theme (decision 19): the existing dark tokens in `app/gui/theme.py` are kept
(`#1c1c1c` window, `#141414` strips, `#111` inputs, `#2a2a2a` borders,
`#dddddd` text, `#888` secondary, accent `#2980b9`, success `#27ae60`/`#2ecc71`,
danger `#c0392b`/`#e74c3c`, amber `#f39c12`, panic red `#8e1d1d`). Semantic
button classes stay: primary / success / danger / neutral / panic.

---

## 5. Panels

### 5.1 Top strip
`MODE` (STANDBY/RUN/SAFE, coloured tile) · `PHASE` · `LINK` (age of last frame:
green <2 s, amber <5 s, red ≥5 s, grey before first frame) · `RX` frames/s ·
command target `host:port` (auto/discovered/manual) · `SESS`/`seq` · `T+`
mission elapsed (since first frame of the onboard session) · `UTC` · `RADIO`
(TX / **SILENT hh:mm:ss**). Right end: panic group **HEATERS OFF**, **STOP
MOTORS** (both unconfirmed), **ENTER SAFE** (confirmed).

### 5.2 Alarm strip
Appears only while at least one alarm is active. Each alarm is a chip with
source and detail; `ACK ALL` (and per-chip click) mutes the chip (it stays
listed, dimmed, until the condition clears). Optional single beep on a *new*
alarm (off by default; View → Audible alarms). Alarm sources:

| Alarm | Condition |
|---|---|
| OVERTEMP | `OVERTEMP_FAIL` |
| SAMPLE_TEMP | `SAMPLE_TEMP_FAIL` (heated channel invalid) |
| LINK STALE | no frame for ≥5 s while not in radio silence |
| FALLBACK | `CTRL fallback:1` |
| SEQ PAUSED | `SEQ_PAUSED` |
| ENERGY | `ENERGY_FAIL` / `budget_exhausted:1` |
| MOTOR n FAILED | `COMPONENT_STATE MOTORn:FAILED` |
| HEATERS INHIBITED | `HEATER_INHIBITED` (informational, amber) |
| SENSOR | any of DPS310/ADS1115/SEQUENT_RTD/PWM not OK, `RESISTANCE_FAIL`, `SPI_FAIL`, `I2C_FAIL`, `SD_FAIL`, `USB_FAIL` |
| RX QUEUE | `CTRL queue:` above 100 frames (backlog draining after link recovery) |

### 5.3 Left column — System tab
- **Link**: target host:port and how it was chosen (discovery / probe / telemetry peer / manual), receiver state, frames/s, last-frame age, onboard queue depth, session id. A `Restart receiver` button appears only after a receiver failure.
- **Mode**: current mode tile; `ARM` (confirm; enabled only in STANDBY), `DISARM` (enabled only in RUN), `ENTER SAFE` (confirm), `EXIT SAFE` (enabled only in SAFE).
- **Phase**: current phase; combo + `SET PHASE` (confirm). Shows `FALLBACK ACTIVE` when reported.
- **Radio**: `RADIO SILENCE` (confirm) / `RADIO RESUME`; silence timer (§9).
- **Downlink rate**: tick Hz spin (0.1–5.0) + `Set` (BEXUS §5.4).
- **Diagnostics**: `PING`, `STATUS`, `COMPONENTS`, `CHECK` with a component selector (ALL, DPS310, ADS1115, SEQUENT_RTD, MAX31865, PWM, MOTOR0, MOTOR1, STORAGE, COMMS), `GET_THERMAL`, `RESET_CTRL` (confirm — clears the over-temperature latch and PID integrators). Replies are pretty-printed in the console (`key=value;…` bodies become aligned rows).
- **Shutdown**: `SHUTDOWN SAFE` (confirm) — bench / post-landing only, styled danger, placed last.

### 5.3 Left column — Thermal tab
- Header: energy used / budget (Wh) bar, active heaters `n/3`, current PID gains, inhibit state (`heaters inhibited — Mx moving`).
- Six rows H0…H5: measured temperature from the mapped sample (S0…S5, coloured by validity/staleness) · target spin (limits from `GET_THERMAL` `target_min_c/target_max_c`, cached; 0–80 default) · `Set` / `Clear` · duty bar with % · state word: `OFF` / `PID` / `DUTY` / `INHIBITED` / `NO TEMP` (heater cannot run: mapped sample invalid).
- `All targets`: value + `Set all` / `Clear all`.
- `Presets` (decision 8): combo · `Apply` · `Save as…` · `Capture from onboard` (reads `GET_THERMAL` into a preset). Storage: `ground-station/profiles/thermal_presets.json`, versioned (`version: 2`), atomic write (temp file + rename), a `.bak` of the previous file kept on every save, and a record per preset of `created`, `last_applied`, targets, per-channel PID (+ `ALL` gains). Legacy v1 files are migrated on load, never deleted.
- Persistent last-response line.

### 5.4 Left column — Motion tab (the ascent bend)
- Two motor cards, always visible: **M0 · S0–S3** and **M1 · S4–S7**: dots `EN`, `ZERO` (from telemetry `zeroed:`), `MOV`, `HOLD`, `OK`; `pos / tgt` usteps; speed Hz; µstep; resistance readout for the card's monitored specimen(s) (`R now`, `R at bend start`, `Δ%`) — the bend confirmation the owner asked for.
- Motor selector (segmented M0 | M1) drives every control below.
- Actions: `ENABLE`, `DISABLE`, `SET ZERO`, `HOME`, `STOP` (selected motor).
- Jog: −1000 / −100 / −10 / +10 / +100 / +1000 usteps (relative, allowed before zeroing) · speed spin 1–100 full-step Hz, default 100 · `Set speed`.
- **Bend**: target usteps + hold s + `BEND` → `STEPPER_MOVETO <id> <target> <hold>`; **`STANDARD PULL`** → `PULL_EXECUTE <id>` (config travel 200 full steps, hold 5 s). Both are disabled with a visible reason until: mode RUN, motor enabled, motor zeroed, no radio silence, no fallback.
- Pull history: last three `EVT,PULL` rows (full table in the bottom Pulls tab).
- Persistent last-response line.
- Not in this tab (Advanced or console only): `STEPPER_ROTATE`, `STEPPER_SET_MICROSTEP`, `STEPPER_BEND` (alias), `PULL_ARM` (duplicate of `PULL_EXECUTE`).

### 5.5 Left column — Advanced tab (scrollable)
Bend sequences (editor: name, steps table target/hold/speed ≤100 Hz, `LOAD` `RUN` `PAUSE` `RESUME` `STOP` `STATUS` `CLEAR`) · PID tuning (channel/ALL, kp ki kd) · Open-loop duty (per heater slider + `Set`, `All` presets 0/25/50/100 %, `CLEAR_OVERRIDES`) · Microstep (current value, set) · Preset management (rename, delete, export/import JSON) · Network (bind IP, telemetry/command/discovery ports, beacon priority, manual host override) · Fallback plan (§10) · note that bench-only commands (`ARM_DEBUG`, `HEATER_TEST`, `SET_BENCH_MODE`) are console-only.

### 5.6 Center — plots
- x-axis: mission elapsed time `T+hh:mm:ss` (UTC shown in the crosshair readout). Window selector 5 min / 30 min / 2 h / all; the plot follows live data unless paused or panned.
- Full-session retention in memory (numpy arrays; 5 Hz × 6 h × ~30 series is trivial); pyqtgraph auto-downsampling + clip-to-view for drawing.
- Tabs: **Temperatures** (S0–S7; the active target of each heated sample as a dashed line in the same hue; floor / over-temperature reference lines) · **Ambient** (three stacked, x-linked plots: ambient T, pressure, UV) · **Heaters** (H0–H5 duty %) · **Resistance** (only channels that have ever reported; vertical markers at `EVT,PULL` times) · **Motors** (pos/tgt per motor, pull markers).
- Toolbar: window buttons, `Pause`, `Export…` (PNG of the current plot; CSV of the visible window).
- Legend rows show the current value of each series.

### 5.7 Right column
- **Health**: unchanged semantics (17 flags, 6 components), laid out as single-column lists that fit the column at any width.
- **Checkout** (replaces Preflight): live go/no-go rows — link healthy · RTC valid · DPS310/ADS1115 OK · 8/8 PT100 valid · MAX31865 clicks reporting · PWM OK · M0/M1 healthy, enabled, zeroed · storage OK (SD/USB) · energy budget OK · mode RUN · no active alarms; `Run CHECK` button and the last `CHECK` summary (`overall`, per-component, `*_error`).
- **Values**: compact monospace table (session, environment, S0–S7, R monitored, M0/M1, CTRL fields).

### 5.8 Bottom — console, events, pulls
- **Console**: table (time · command · ✔/✖ · ms · response body), newest at the bottom; click a row to re-issue; entry line with completion of `KNOWN_COMMANDS` (Tab), history (↑/↓), `Send`; `key=value;` bodies rendered as aligned rows. Every command from every panel appears here.
- **Events**: log with level colouring (INFO/WARN/ERROR) and a filter box; same content is written to `events.log`.
- **Pulls**: `EVT,PULL` table (time, motor, pull id, steps, hold, samples).

---

## 6. Interaction rules

### 6.1 Confirmation
Modal Yes/No (default No): `ARM`, `SET_PHASE`, `ENTER_SAFE`, `SHUTDOWN_SAFE`, `RADIO_SILENCE`, `RESET_CTRL`, `BENDSEQ_RUN`. Unconfirmed: everything else, including the panic pair (`HEATERS_OFF`, `STEPPER_STOP 0/1`), `DISARM`, `EXIT_SAFE`, `RADIO_RESUME`, motion commands.

### 6.2 Gating
A control that cannot succeed is disabled and its tooltip (and the panel's
reason line) says why, derived from telemetry: mode, enabled, zeroed,
fallback, radio silence, heater temperature validity. The onboard remains the
authority — gating never replaces its checks; it prevents predictable NACKs.

### 6.3 Feedback
No toasts. Each control group has a persistent last-response line (green ACK
with body, red NACK with reason, latency); the console holds the full record.

### 6.4 Command set exposed as controls
Kept: `PING STATUS COMPONENTS CHECK GET_THERMAL RESET_CTRL ARM DISARM
ENTER_SAFE EXIT_SAFE SET_PHASE RADIO_SILENCE RADIO_RESUME SET_TICK_HZ
HEATERS_OFF SHUTDOWN_SAFE SET_TEMP_TARGET SET_ALL_TEMP_TARGETS
CLEAR_TEMP_TARGET CLEAR_TEMP_TARGETS SET_PID SET_HEATER_DUTY SET_ALL_DUTY
CLEAR_OVERRIDES STEPPER_ENABLE STEPPER_DISABLE SET_POSITION_ZERO STEPPER_MOVE
STEPPER_MOVETO STEPPER_HOME STEPPER_STOP STEPPER_SET_SPEED
STEPPER_SET_MICROSTEP PULL_EXECUTE BENDSEQ_*`.
Console-only: `FORCE_START FORCE_STOP ON OFF RESET STEPPER_ROTATE STEPPER_BEND
PULL_ARM ARM_DEBUG DISARM_DEBUG HEATER_TEST SET_BENCH_MODE`.

### 6.5 Shortcuts (redesigned)
| Keys | Action | Notes |
|---|---|---|
| `Esc` | `STEPPER_STOP 0` + `STEPPER_STOP 1` | panic, no confirm, works with any focus except an open modal |
| `Ctrl+Shift+H` | `HEATERS_OFF` | panic, no confirm; modifier required to prevent accidental fire |
| `Ctrl+L` | focus the console entry | |
| `Ctrl+1…4` | left tabs System / Thermal / Motion / Advanced | |
| `Alt+1…5` | plot tabs | |
| `P` | pause/resume plots | ignored while a text field has focus |
| `F5` | send `STATUS` | |
| `Ctrl+=` / `Ctrl+-` / `Ctrl+0` | UI scale | persisted |
| `F1` | shortcut and glyph legend | |
No unmodified letter key sends a command.

---

## 7. Data retention (decisions 20, 21)

```text
ground-station/logs/
  ground_ack_cursor.json                 (unchanged: per-session ACK cursor)
  sessions/<YYYYMMDD-HHMMSS>_<onboard session id>/
    telemetry.csv                        (schema v6, one writer for GUI and CLI)
    pulls.csv                            (EVT,PULL)
    commands.csv                         (ts_utc, command, ok, latency_ms, body, raw)
    events.log                           (ts_utc level message)
    session.json                         (gs version, host, ports, first/last frame, counts)
  latest_session.txt                     (path of the directory in use)
```
A new directory starts when the onboard session id changes (Pi restart); a
GS restart while the onboard session is unchanged appends to the existing
directory. `app/telemetry_log.py` owns the v6 schema (`TELEMETRY_CSV_FIELDS`,
`packet_to_row`, `PullLog`, `CommandLog`, `EventLog`); the GUI receiver and the
CLI telemetry server both import it. v6 = v5 columns + `stepperN_zeroed`,
`stepperN_seq`, `stepperN_seqstate`, `fallback`, `link_loss_s`, `energy_wh`,
`budget_wh`, `budget_exhausted`, `heaters_active`, `queue`.

---

## 8. Wire-protocol additions (backward compatible; decision 22)

Documented in `docs/protocol.md`; the GS parser treats every new key as optional.

- `STEPPERn=` gains `|zeroed:<0|1>|seq:<name or ->|seqst:<idle|run|pause>`.
- New token `CTRL=fallback:<0|1>|link_loss_s:<x.x>|energy_wh:<x.x>|budget_wh:<x.x>|budget_exhausted:<0|1>|heaters_active:<n>|queue:<n>|plan:<none|armed|running|done|failed>` (`plan` is §10).
- `STATUS` reply unchanged (still carries everything above for CLI users).

---

## 9. Radio silence (decision 7 — the most important control)

Requirement (owner): on request, the experiment stops all communication until
the operator resumes it. Implementation, both sides:

Onboard (`RADIO_SILENCE`):
1. Telemetry TCP client closed and no reconnect attempts (already true).
2. `BeaconSenderLoop` sends nothing while `transmit_enabled_ == false` (fix).
3. `DiscoveryListenerLoop` does not answer `GS_HELLO` while silent (fix); it keeps listening.
4. The command server keeps accepting commands (the only way to resume) but replies only to `RADIO_RESUME`, `RADIO_SILENCE`, `STATUS` and `PING`; every other command is refused with `NACK,<cmd>,radio silence active` **without side effects**, so a mis-sent command cannot start anything while silent.
5. Silence is persisted (`<queue_dir>/radio_silence` flag file) so a process restart during silence comes back silent (**decision D2 below**).
6. The durable queue keeps every frame; `RADIO_RESUME` drains the backlog (already true; the GUI shows the `queue:` depth while it drains).

Ground station:
1. On `ACK,RADIO_SILENCE`: GS beacon and command probe stop; the listener stays receive-only; the top strip switches to the SILENT band with a timer; the LINK alarm is suppressed (expected).
2. The console and every panel refuse to send anything except `RADIO_RESUME` (and `STATUS`/`PING` only through the console, marked as "answers during silence").
3. On `ACK,RADIO_RESUME`: beacon and probe restart; normal state.
4. If the GS restarts while silent it does not know the state: it starts in "unknown" mode with beacons/probes **off** until the first telemetry frame or an explicit `STATUS` reply (`STATUS` reports `silence=…` — added to the reply) tells it otherwise.

Test (BEXUS §5.10.5 spirit): a bench packet capture during silence must show
zero packets originated by the Pi except replies to the operator's own
commands.

---

## 10. Link-loss failsafe (Phase C proposal — owner decisions D3–D6)

Existing onboard behaviour (unchanged): fallback starts after an established
link is lost for `manual.link_loss_fallback_s` (10 s) while in RUN: non-sequence
motion stops, an active bend sequence continues, PID targets continue, untargeted
heated samples get the +5 °C floor, phase tracks pressure (`ASCENT→PRE_FLOAT` at
150 mbar, `→FLOAT` at 100 mbar, `→DESCENT` at 300 mbar, `→LANDED` at 800 mbar,
5-sample debounce). Nothing starts a bend.

Proposed addition — an operator-armed **fallback plan** that the onboard executes
only while fallback is active:

- Commands: `FALLBACK_PLAN <motor> <target_usteps> <hold_s> [speed_hz]` (one per motor, persisted to disk), `FALLBACK_ARM`, `FALLBACK_DISARM`, `FALLBACK_STATUS`. Telemetry reports `plan:` in `CTRL=`.
- Trigger: fallback active ∧ phase ∈ {PRE_FLOAT, FLOAT} ∧ plan armed ∧ not executed ∧ motor enabled ∧ zeroed ∧ healthy ∧ (optional) the motor's sample group mean temperature within `[fallback.bend_min_c, fallback.bend_max_c]`.
- Execution: M0 then M1 (MotionLock serialises), each `MoveToSteps(target, hold)`; `EVT,PULL` emitted per motor; plan marked `done` (persisted) so a restart never repeats it.
- If the link returns mid-plan the plan finishes; the GUI shows it.
- Inputs the owner listed and how they are used: pressure → phase (decides *when*); temperature → optional window (decides *whether now*); resistance → logged before/after for verification, not a decision input; UV → logged, not a decision input (D5).
- Landing: on `LANDED` in fallback → heaters off and motors disabled (D6).

---

## 11. Non-goals
Light theme; movable docks; replay of old CSVs; changing the DATA frame's
existing tokens; a second ground-station failover UI (priority stays an
Advanced field); onboard autonomy beyond §10.

---

## 12. Open decisions for the owner

| ID | Question | Proposed default |
|---|---|---|
| D1 | Approve the mockup layout (Main, Compact, tab panels, silence state)? | — |
| D2 | Persist radio silence across an onboard restart? | Yes (a reboot during a mandated silence must not start transmitting) |
| D3 | Fallback plan: bend only inside a temperature window, or regardless of temperature? | Window, defaults −40…+40 °C, configurable; if the window is never met within `fallback.bend_deadline_s` (30 min after PRE_FLOAT), execute anyway |
| D4 | Fallback plan values: which target/hold per motor should be pre-loaded before launch? | Same as the operator's planned manual bend (entered in Advanced → Fallback plan) |
| D5 | UV and resistance stay logging-only inputs in the failsafe? | Yes |
| D6 | On LANDED during fallback: heaters off + motors disabled? | Yes |
| D7 | Beep on new alarm: off by default? | Yes |
