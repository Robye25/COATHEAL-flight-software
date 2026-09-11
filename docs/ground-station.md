# Ground Station Reference

The ground station is the operator console for Rev C manual-first flight
operations: a PyQt6 mission console (`gui_app.py`), a headless telemetry
receiver and a one-shot command client (`main.py`). This page describes the
console as built by the 2026-08-28 redesign
(`docs/superpowers/specs/2026-08-28-ground-station-redesign-design.md`).

```text
ground-station/
  gui_app.py                  console entry point
  main.py                     CLI entry point (telemetry-server, command)
  app/protocol.py             DATA / EVT,PULL / ACK / command parsing, validators
  app/reply_format.py         key=value; reply-body parsing and pretty-printing
  app/telemetry_log.py        schema-v6 CSV writer, session directories, command/event logs
  app/session_dir.py          per-session directory naming
  app/thermal_presets.py      thermal presets v2 (atomic file, .bak, v1 migration)
  app/telemetry_server.py     headless receiver (same writer as the GUI)
  app/command_client.py       one-shot command uplink
  app/gui/main_window.py      the console: regions, wiring, shortcuts, persistence
  app/gui/state.py            OnboardState snapshot per frame
  app/gui/gating.py           why a control cannot succeed (silence, mode, fallback, enable, zero, temperature)
  app/gui/alarms.py           alarm model with acknowledgement
  app/gui/dispatch.py         telemetry receiver thread, command dispatcher, radio-silence gate
  app/gui/discovery.py        UDP beacon / listener / command probe (quiet during silence)
  app/gui/panel_top.py        top strip (mode, phase, health, link, rate, target, T+, UTC, radio, panic buttons) and alarm strip
  app/gui/tab_system.py       System tab
  app/gui/tab_thermal.py      Thermal tab
  app/gui/tab_motion.py       Motion tab (bend, standard pull, resistance before/after)
  app/gui/tab_advanced.py     Advanced tab (sequences, PID, open-loop duty, microstep, presets, network, fallback plan)
  app/gui/tab_debug.py        Debug tab (MOTOR_DEBUG probe: live TMC5160 registers, rates, verdict)
  app/gui/motor_debug.py      MOTOR_DEBUG parsing + motion estimator (no Qt)
  app/gui/plots.py            time-axis plots over series_store.py
  app/gui/panels_health.py    Health tab and the aggregate health summary
  app/gui/panel_checkout.py   live go/no-go checklist + RUN CHECK
  app/gui/panel_values.py     latest value of every field
  app/gui/panel_console.py    command console (history, entry, completion)
  app/gui/panel_events.py     event log and EVT,PULL table
```

## Launch

```bash
cd ground-station
python gui_app.py [--host <onboard-ip>] [--tel-port 4000] [--cmd-port 5000] [--log logs] [--no-firewall-check]
```

With no `--host` the console is plug-and-play: it listens for telemetry on
TCP 4000, beacons on UDP 4100, probes `169.254.10.10:5000` with `PING`, and
takes the first onboard it hears (discovery, probe or telemetry peer) as the
command target. `--log` is the log root; every onboard session gets its own
directory under `<root>/sessions/`.

The console scales to any screen from 1366×768 up (`Ctrl+=` / `Ctrl+-` /
`Ctrl+0` change the UI size; the setting persists). The layout is fixed —
nothing floats — and the three column widths, the console height and the
window geometry are remembered between runs.

## Layout

```text
┌ File · View · Help ──────────────────────────────────────────────────────────────────┐
│ MODE · PHASE · HEALTH · LINK age · RX rate · TARGET · SESSION/seq · T+ · UTC · RADIO │ HEATERS OFF · STOP MOTORS · ENTER SAFE │
│ alarm strip (only while alarms are active)                                  [ACK ALL] │
├──────────────┬──────────────────────────────────────────────┬────────────────────────┤
│ System       │ Temperatures · Ambient · Heaters ·           │ Health · Checkout ·    │
│ Thermal      │ Resistance · Motors                          │ Values                 │
│ Motion       │ window 5 m · 30 m · 2 h · all · FOLLOW ·     │                        │
│ Advanced     │ PAUSE · EXPORT                               │                        │
├──────────────┴──────────────────────────────┬───────────────┴────────────────────────┤
│ Console (every command + reply, entry line) │ Events · Pulls                         │
├─────────────────────────────────────────────┴────────────────────────────────────────┤
│ session directory · frames · parse errors · disk · UI size · Esc = STOP MOTORS · F1  │
└──────────────────────────────────────────────────────────────────────────────────────┘
```

### Top strip
`MODE` (STANDBY / RUN / SAFE), `PHASE`, `HEALTH` (aggregate dot: green only
when all 14 OK/FAIL flags are OK and no component is DEGRADED/STALE/FAILED,
amber on simulated sensors, red on any failure, grey when unreported),
`LINK` (age of the last frame: green < 2 s, amber < 5 s, red beyond),
`RX` frames/s, `TARGET` host:port and how it was chosen, `SESSION`/seq,
`T+` mission elapsed since the first frame of the onboard session, `UTC`,
`RADIO` (TX, or `SILENT hh:mm:ss`). The panic group on the right sends
`HEATERS_OFF` and `STEPPER_STOP 0` + `STEPPER_STOP 1` without confirmation;
`ENTER SAFE` confirms first. During radio silence the whole strip turns
purple and the panic buttons are disabled (only `RADIO RESUME` is live).

### Alarm strip
Appears while alarms are active. Sources: `OVERTEMP`, `SAMPLE_TEMP` (which
heated channels are invalid), `FALLBACK` (link-loss fallback onboard),
`SEQ_PAUSED`, `ENERGY`, `MOTORn FAILED`, `HEATERS_INHIBITED` (amber, names
the moving motor), `SENSOR` (any bad component or bus flag), `RX_QUEUE`
(onboard backlog draining), `LINK STALE` (suppressed during silence).
Click a chip or `ACK ALL` to acknowledge; an acknowledged alarm stays listed
dimmed until its condition clears, and re-raises if the condition returns.
View → *Audible alarms* adds one beep per new alarm (off by default).

### System tab
Link (target, receiver state, rate, onboard queue depth, session; a
`RESTART RECEIVER` button appears only after a receiver failure) · Mode
(`ARM` confirmed, `DISARM`, `ENTER SAFE` confirmed, `EXIT SAFE` — each
enabled only in the mode where the onboard accepts it) · Phase (`SET PHASE`
confirmed; fallback indicator) · Radio (`RADIO SILENCE` confirmed, `RADIO
RESUME`) · Downlink rate (`SET_TICK_HZ`, 0.1–5 Hz) · Diagnostics (`PING`,
`STATUS`, `COMPONENTS`, `GET_THERMAL`, `CHECK <component>`, `RESET_CTRL`
confirmed) · Shutdown (`SHUTDOWN_SAFE` confirmed).

### Thermal tab
Energy used / budget, active heaters (of 3), motion inhibit, PID gains ·
six rows H0–H5: measured temperature of the mapped sample (S0–S5), target
spin box + `Set` / `Clr`, duty bar, state word (`OFF`, `PID`, `DUTY`,
`INHIB`, `NO-T` = the sample is invalid so the heater cannot run) · `Set
all` / `Clear all` / `Refresh` (`GET_THERMAL`, which also supplies the
target limits) · presets: `Apply` (re-sends `SET_PID ALL …` then a
`SET_TEMP_TARGET` / `CLEAR_TEMP_TARGET` per heater), `Save as…`, `Capture`
(reads the onboard's current targets into a preset).

Presets live in `profiles/thermal_presets.json` (v2). Every save is atomic
and keeps the previous file as `.bak`; a legacy `thermal_profiles.json` is
migrated on first load and never deleted.

### Motion tab
Two motor cards (M0 = S0–S3, M1 = S4–S7) with enabled / zeroed / moving /
holding / healthy dots, position and target, speed, last command, sequence
state, and the resistance readout: `R now`, `bend start` and `Δ %` for the
card's monitored specimen — the bend confirmation. The selector (M0 | M1)
drives the shared controls: `ENABLE`, `DISABLE`, `SET ZERO`, `HOME`, `STOP`
· jog ±0.1/±1/±5 mm (allowed before zeroing) · speed 1–50
full-step Hz (the 0.5 mm/s ceiling at the 2 mm lead) · **BEND**
(`STEPPER_MOVETO <id> <target> <hold>`) and
**STANDARD PULL** (`PULL_EXECUTE <id>`, the config-defined pull). BEND and
STANDARD PULL are disabled — with the reason shown — until the motor is
enabled and zeroed, the mode is RUN, and neither radio silence nor
link-loss fallback is active. Pressing them records the specimen
resistance at bend start; the idle→moving edge does the same automatically.

### Advanced tab
Bend sequences (`BENDSEQ_LOAD/RUN/PAUSE/RESUME/STOP/STATUS/CLEAR`; step
speed ≤ 50 Hz) · PID tuning · open-loop duty (`SET_HEATER_DUTY`,
`SET_ALL_DUTY`, `CLEAR_OVERRIDES`) · microstep · preset management ·
fallback plan (`FALLBACK_PLAN/ARM/DISARM/STATUS`, executed onboard only
during link-loss fallback — see the redesign spec §10) · network (beacon
priority, manual host override). Bench-only commands (`ARM_DEBUG`,
`HEATER_TEST`, `SET_BENCH_MODE`) are console-only.

### Debug tab

Answers "is the motor really moving?" when the camera cannot. The
telemetry position is the firmware's own counter and advances even when
nothing turns (a module strapped for STEP/DIR, a power stage that is off).
The tab polls `MOTOR_DEBUG <id>` (START PROBE, default every 500 ms; READ
ONCE for a single sample) through the dispatcher's quiet path -- the replies
never reach the console, the command history or `commands.csv` -- and
decodes the TMC5160's own registers:

- **MSCNT** is the chip's microstep sine-table index: 256 counts per full
  step whatever the microstep setting, so ΔMSCNT is the coil-driving truth.
  **XACTUAL / VACTUAL** are the ramp generator's position and velocity;
  **stst** standstill; **DRV_ENN=1** or **TOFF=0** means the power stage is
  off; **SD_MODE=1** is the STEP/DIR strap; **ola/olb/s2ga/s2gb/ot/otpw**
  are the driver's fault flags (open-load is only valid at standstill).
- Derived over a 3 s window: sequencer rate (full-steps/s from ΔMSCNT),
  ramp rate (from ΔXACTUAL), rev/s and rpm (200 full steps per revolution),
  mm/s using the *ball-screw lead* you enter (persisted; default 1.5 mm/rev
  -- the mechanism does ~1–2 mm per revolution), and travel since the probe
  started.
- A verdict line: MOVING; COMMANDED BUT NOT STEPPING (ramp moves, MSCNT
  frozen); firmware says moving but the chip is at standstill; power stage
  off; SD_MODE strap; DRIVER FAULT.
- A plot of MSCNT and XACTUAL against seconds since the probe started.

At the 50 Hz ceiling a BEND of 800 µsteps at µ4 is one revolution in 4 s,
about 1.5 mm -- invisible on a remote camera, unmistakable in MSCNT. The
probe stops itself when radio silence starts or the link is lost.

### Plots
x-axis is mission elapsed time (`T+hh:mm:ss`; the crosshair readout also
shows UTC). Window 5 m / 30 m / 2 h / all; `FOLLOW` re-engages live
scrolling after a manual pan; `PAUSE` freezes drawing (data keeps
accumulating); `EXPORT` writes a PNG of the current page or a CSV of the
visible window. Every series is kept for the whole session. Pages:
Temperatures (S0–S7, dashed target overlays, fallback floor and
over-temperature lines), Ambient (three x-linked plots: ambient T, pressure
with the pre-float line, UV), Heaters (duty %), Resistance (only channels
that have reported; pull markers), Motors (position/target per motor; pull
markers).

### Right column
Health (every OK/FAIL flag, tri-state flag and COMPONENT_STATE entry as a
dot) · Checkout (live go/no-go rows: link, RTC, DPS310/ADS1115, PT100
channels valid n/8, MAX31865 clicks reporting, PWM, M0/M1 healthy · enabled
· zeroed, storage, energy budget, mode RUN, unacknowledged alarms, last
CHECK; `RUN CHECK ALL`) · Values (every field, monospace).

### Console, events, pulls
The console lists every command from every panel with ✔/✖, latency and the
reply body; select a row to see the reply pretty-printed, double-click to
put the command back in the entry. The entry completes known commands with
Tab and keeps ↑/↓ history. Events shows the log with level colouring and a
filter; Pulls lists every `EVT,PULL`. All three are written to the session
directory automatically.

### Backlog replay

After a link outage the onboard replays its queued frames at ~10/s. With
current firmware the drain is **live-first**: each tick's frame is sent
before the backlog and every frame carries its age (`TX=`), so the panels,
gating and alarms stay on live frames throughout; the replayed frames only
fill the plots (inserted at their onboard time) and the session logs. The
top strip shows `REPLAY <n> queued · ETA`, and one amber `BACKLOG` alarm
replaces the queue-depth alarm until the queue is empty. The previous
session's backlog and the current session's live frames interleave; each
lands in its own `logs/sessions/` directory, and commands and events go to
the newest session.

With firmware that predates the stamp the drain is in order and the console
falls back to clock reasoning: a frame whose onboard time lags the best lag
seen this session by more than 30 s, or a stream advancing faster than 1.5×
wall time, is replay; the panels keep the last live frame and the top strip
shows how far behind the arriving frames are. In both cases an `ARM`,
`DISARM`, `EXIT_SAFE` or `STATUS` acknowledgement that carries `mode=` is
applied to the panels immediately, so an ARM during a replay is never shown
as ignored.

## Interaction rules

- **Confirmation dialogs**: `ARM`, `SET_PHASE`, `ENTER_SAFE`, `SHUTDOWN_SAFE`,
  `RADIO_SILENCE`, `RESET_CTRL`, `BENDSEQ_RUN`, `FALLBACK_ARM`. Everything
  else — including the panic pair — sends immediately.
- **Gating**: a control that would be refused by the onboard is disabled
  and its tooltip (and the note under its group) says why. Unknown facts
  never block; the onboard remains the authority.
- **Feedback**: a persistent response line under each control group, plus
  the console. No toasts.
- **Radio silence**: after `ACK,RADIO_SILENCE` (or a `STATUS` reply carrying
  `silence=1`) the ground station stops its beacon and probe and refuses to
  send anything except `RADIO_RESUME`, `RADIO_SILENCE`, `STATUS`, `PING`;
  refused commands appear in the console and `commands.csv` with the reason.

### Shortcuts

| Keys | Action |
|---|---|
| `Esc` | `STEPPER_STOP 0` + `STEPPER_STOP 1` (panic, no confirm) |
| `Ctrl+Shift+H` | `HEATERS_OFF` (panic, no confirm) |
| `Ctrl+L` | focus the console entry |
| `Ctrl+1 … Ctrl+5` | System / Thermal / Motion / Advanced / Debug |
| `Alt+1 … Alt+5` | plot pages |
| `P` | pause / resume plots (ignored while typing) |
| `F5` | send `STATUS` |
| `Ctrl+=` / `Ctrl+-` / `Ctrl+0` | UI size |
| `F1` | shortcut list |

No unmodified letter sends a command. While a confirmation dialog is open
every shortcut is blocked; `Esc` closes the dialog first.

## Logs

```text
logs/
  ground_ack_cursor.json                   per-session ACK cursor (dedupe across restarts)
  latest_session.txt                       path of the directory in use
  sessions/<YYYYMMDD-HHMMSS>_<session id>/
    telemetry.csv                          schema v6, one row per accepted DATA frame
    pulls.csv                              EVT,PULL
    commands.csv                           ts_utc, command, ok, latency_ms, body, raw
    events.log                             the event log
    session.json                           ground-station metadata and counters
```

The directory is named after the onboard session id (its boot time first,
so directories sort chronologically); a ground-station restart while the
onboard session is unchanged appends to the same directory. Commands and
events that happen before the first frame are buffered and written when
the directory opens. The headless `telemetry-server` writes exactly the
same files.

Schema v6 columns: `gs_rx_utc, session_id, seq, timestamp, rtc_valid,
ambient_temp_c, ambient_pressure_mbar, uv, sample_0..7, h0..5, r0..7, phase,
mode, status, sensor_valid, sensor_age_ms, component_state,
stepper{0,1}_{position,target,hz,microstep,enabled,ok,moving,holding,hold_s,pulses,missed,source,zeroed,seq,seqstate},
fallback, link_loss_s, energy_wh, budget_wh, budget_exhausted,
heaters_active, queue, plan`. Numbers are written losslessly; unmeasured
values are empty cells.

## CLI

```bash
python main.py telemetry-server [--bind 0.0.0.0] [--port 4000] [--log logs] [--plot] [--alert-temp-c 80] [--timeout-s 10]
python main.py command --cmd "<COMMAND>" [--host <ip>] [--port 5000] [--timeout <s>] [--yes]
```

`command` resolves the host from `--host`, then `logs/discovered_onboard.json`,
then UDP discovery, then `169.254.10.10`. Dangerous commands ask for a
typed `YES` unless `--yes` is given. Per-verb timeouts come from
`protocol.COMMAND_TIMEOUTS` (`CHECK` 15 s, everything else 3 s).
