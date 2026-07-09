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
actively opens/flushes both log paths, reads DPS310, ADS1115, and DAQ132M,
checks the PWM/stepper GPIO backends, and re-runs the TMC2240 SPI setup. Run
`CHECK` only while both motors are idle.

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

- Targets must be within `heater.target_min_c..heater.target_max_c`.
- Setting a duty clears that channel's target.
- Setting a target clears that channel's duty override.
- `HEATERS_OFF` clears every duty and target.
- Invalid PT100 data forces the matching heater off, including open-loop mode.
- Overtemperature is latched until `RESET_CTRL`.
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

## Link-Loss Fallback

Fallback begins only after a link has been established and then remains lost
for `manual.link_loss_fallback_s`.

On fallback entry:

- The currently active bend sequence may continue.
- No new sequence may start.
- Non-sequence manual motion stops and new manual motion is rejected.
- Existing PID targets continue.
- Untargeted channels use `phase.sample_floor_c`.
- No queued fatigue sequence or phase-bend action starts automatically.

Inspect fallback state with:

```powershell
python main.py command --cmd STATUS
```

## Stop and Safe State

```powershell
python main.py command --cmd HEATERS_OFF --yes
python main.py command --cmd "BENDSEQ_STOP 0"
python main.py command --cmd "BENDSEQ_STOP 1"
python main.py command --cmd DISARM
python main.py command --cmd ENTER_SAFE --yes
python main.py command --cmd SHUTDOWN_SAFE --yes
```
