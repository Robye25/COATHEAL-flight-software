# Rev C Manual Operations

Run commands from `ground-station/`:

```powershell
python main.py command --cmd "<COMMAND>"
```

The default host resolution is discovery, cached discovery, then
`169.254.10.10`. Add `--host <ip>` when needed.

## Start and Preflight

```powershell
python main.py command --cmd PING
python main.py command --cmd STATUS
python main.py command --cmd CHECK
python main.py command --cmd ARM
```

`STATUS` reports cached live state and does not disturb hardware. `CHECK`
actively opens/flushes both log paths, reads DPS310, ADS1115, the Sequent
RTD HAT, and the two MAX31865 sample-resistance clicks, checks the PWM
backend, and re-runs the TMC5160 SPI setup (SPI-only, no STEP/DIR GPIO). Run
motor-focused `CHECK` only while both motors are idle.

## Thermal Control

Set and inspect closed-loop targets:

```powershell
python main.py command --cmd "SET_PID ALL 0.20 0.02 0.03"
python main.py command --cmd "SET_PID 2 0.25 0.03 0.02"
python main.py command --cmd "SET_TEMP_TARGET 2 25.0"
python main.py command --cmd "SET_ALL_TEMP_TARGETS 20.0"
python main.py command --cmd GET_THERMAL
python main.py command --cmd "CLEAR_TEMP_TARGET 2"
python main.py command --cmd CLEAR_TEMP_TARGETS
```

Open-loop duty control:

```powershell
python main.py command --cmd "SET_HEATER_DUTY 2 0.20"
python main.py command --cmd "SET_ALL_DUTY 0.10"
python main.py command --cmd HEATERS_OFF --yes
```

Rules:

- Targets must be within `heater.target_min_c..heater.target_max_c` (flight
  `0..75` °C). The ground station asks for confirmation before any target
  above 40 °C.
- Setting a duty clears that channel's target.
- Setting a target clears that channel's duty override.
- `HEATERS_OFF` clears every duty and target.
- Invalid PT100 data forces the matching heater off, including open-loop mode.
- A heater whose sample reads above `heater.max_sample_temp_c` (80 °C) is
  latched off until `RESET_CTRL`.
- The Pi does not persist targets or tuned PID gains across restart. Save and
  reapply them through a ground-station thermal profile.

## Motor Preparation

There are no limit switches. After every onboard restart:

1. Move the mechanism to a known physical reference using safe relative jogs.
2. Set that position as software zero.
3. Use absolute moves or sequences only after zeroing.

```powershell
python main.py command --cmd "STEPPER_ENABLE 0"
python main.py command --cmd "STEPPER_SET_SPEED 0 50"
python main.py command --cmd "STEPPER_MOVE 0 -100"
python main.py command --cmd "SET_POSITION_ZERO 0"
python main.py command --cmd "STEPPER_MOVETO 0 800"
python main.py command --cmd "STEPPER_HOME 0"
python main.py command --cmd "STEPPER_STOP 0"
python main.py command --cmd "STEPPER_DISABLE 0"
```

Repeat with motor id `1` for the second actuator. `MotionLock` allows only one
motor to move at a time and immediately forces all heater outputs to zero.

## Is the motor really moving?

The position in telemetry is the firmware's own counter and advances even
when the motor does not (a module strapped for STEP/DIR, or a power stage
that is off). The chip cannot lie: `MOTOR_DEBUG <id>` reads its registers
live. `mscnt` (0..1023, the microstep sine-table index) advances only when
the sequencer really steps -- 256 counts per full step, so it wraps every
four full steps; `xactual`/`vactual` are the ramp generator's
position and velocity; `stst=1` means standstill; `drv_enn=1` or `toff=0`
means the power stage is off; `ola`/`olb` are open-load flags (only valid
at standstill), `s2ga`/`s2gb` short-to-ground. The console's **Debug tab**
polls this twice a second and turns the deltas into full-steps/s, rev/s and
mm/s (using the ball-screw lead you enter), with a verdict line. Expected:
one revolution = 200 full steps ≈ 1–2 mm of pull; at the 0.5 mm/s ceiling (50 full-steps/s) a
BEND of 800 µsteps (µ4) takes 4 s and moves about 1.5 mm -- easy to miss on
a camera, obvious in `mscnt`.

```powershell
python main.py command --cmd "MOTOR_DEBUG 1"
```

## Bend Sequences

A step is:

```text
<absolute_target_microsteps>:<hold_seconds>[:<speed_full_step_hz>]
```

Example:

```powershell
python main.py command --cmd "STEPPER_ENABLE 1"
python main.py command --cmd "SET_POSITION_ZERO 1"
python main.py command --cmd "BENDSEQ_LOAD 1 flex 800:2:50 1600:3:75 0:1:50"
python main.py command --cmd "BENDSEQ_RUN 1 flex"
python main.py command --cmd "BENDSEQ_STATUS 1"
python main.py command --cmd "BENDSEQ_PAUSE 1"
python main.py command --cmd "BENDSEQ_RESUME 1"
python main.py command --cmd "BENDSEQ_STOP 1"
python main.py command --cmd "BENDSEQ_CLEAR 1 flex"
python main.py command --cmd "BENDSEQ_CLEAR 1"
```

Only one sequence runs per motor and `MotionLock` still prevents simultaneous
motion. A motor/backend/overtemperature fault pauses the sequence and reports
`SEQ_PAUSED` plus fault detail in `BENDSEQ_STATUS`.

## Radio Silence

`RADIO_SILENCE` stops every transmission the onboard originates until the
operator lifts it with `RADIO_RESUME`:

- the telemetry TCP client closes and makes no reconnect attempts;
- the UDP `ONBOARD_BEACON` broadcast stops;
- `GS_HELLO` datagrams are not answered (the sender is still remembered so
  `RADIO_RESUME` can dial it);
- every command except `RADIO_RESUME`, `RADIO_SILENCE`, `STATUS` and `PING`
  is refused with `NACK,<COMMAND>,radio silence active` before it has any
  effect, so a mis-sent command cannot start anything while silent.

The command server keeps listening — that is the only way to resume — and
the reply to one of the four allowed commands is the only packet the onboard
will send. `STATUS` reports `silence=1` while silent. The state is persisted
in `<storage.queue_dir>/radio_silence`, so an onboard restart during a
mandated silence comes back silent; `RADIO_RESUME` removes the file.

Telemetry frames produced during silence stay in the durable queue and are
delivered, in order, after `RADIO_RESUME` (the ground station shows the
backlog draining through the `CTRL` `queue` field).

```powershell
python main.py command --cmd RADIO_SILENCE --yes
python main.py command --cmd STATUS            # ...;silence=1;...
python main.py command --cmd RADIO_RESUME
```

The ground station pauses its own discovery beacons and command probes while
silent and sends nothing but `RADIO_RESUME` (and, from its console, `STATUS`
or `PING`).

## Link-Loss Fallback

Fallback begins only after a link has been established and then remains lost
for `manual.link_loss_fallback_s`.

On fallback entry:

- The currently active bend sequence may continue.
- No new sequence may start.
- Non-sequence manual motion stops and new manual motion is rejected.
- Existing PID targets continue.
- Untargeted channels use `phase.sample_floor_c`.
- No sequence starts automatically; the only autonomous motion is the
  operator-armed failsafe plan below.

Inspect fallback state with:

```powershell
python main.py command --cmd STATUS
```

## Link-Loss Failsafe Plan

If the link is lost right when the samples should be bent (ascent, just
before float), the onboard can bend them on its own — but only with a plan
the operator loaded and armed beforehand. Load one bend per motor (absolute
microsteps, hold seconds, optional full-steps/s — the console's Advanced tab
takes mm and mm/s and converts), then arm:

```powershell
python main.py command --cmd "FALLBACK_PLAN 0 800 5 50"
python main.py command --cmd "FALLBACK_PLAN 1 800 5 50"
python main.py command --cmd FALLBACK_ARM
python main.py command --cmd FALLBACK_STATUS
```

`FALLBACK_STATUS` answers
`state=armed;armed=1;deadline_s=1800;deadline_started=0;m0=800/5/50/pending;m1=800/5/50/pending`.
The same state is in every telemetry frame (`CTRL` `plan`) and in `STATUS`
(`plan=`). Arming works in any mode; the plan only ever runs during
link-loss fallback, which requires RUN. Do it after the motors are enabled
and zeroed — a motor that is not enabled, zeroed and healthy when its turn
comes is skipped once the deadline passes.

What the onboard does, and only while fallback is active at `PRE_FLOAT` or
`FLOAT`: M0 bends first, then M1, one at a time, each when its sample
group's mean valid temperature is inside `fallback.bend_min_c..bend_max_c`
(default −40…+40 °C) or, unconditionally, once `fallback.bend_deadline_s`
(default 30 min) have passed since fallback first held there. Each bend is a
normal absolute move with hold and produces an `EVT,PULL`. UV and specimen
resistance are logged for the post-flight analysis but never gate the plan.
A completed or failed plan never re-runs (restarts included); if the link
returns while a bend is in motion the bend finishes.

Disarm at any time (the loaded targets stay, so `FALLBACK_ARM` re-arms them):

```powershell
python main.py command --cmd FALLBACK_DISARM
python main.py command --cmd "STEPPER_STOP 0"      # disarming never stops motion
```

At `LANDED` in fallback (`fallback.landed_safe=true`) the onboard turns the
heaters off and disables both motors once. Keys: `docs/configuration.md`
(*Link-Loss Failsafe Plan*).

## Stop and Safe State

```powershell
python main.py command --cmd HEATERS_OFF --yes
python main.py command --cmd "BENDSEQ_STOP 0"
python main.py command --cmd "BENDSEQ_STOP 1"
python main.py command --cmd DISARM
python main.py command --cmd ENTER_SAFE --yes
python main.py command --cmd SHUTDOWN_SAFE --yes
```
