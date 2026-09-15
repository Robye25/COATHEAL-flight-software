# Configuration Reference (Rev C Final BOM)

All onboard configuration is loaded from an INI file at startup via
`--config <path>`. Use `config/onboard.example.ini` for the flight template and
`config/onboard.debug.ini` for bench/simulation.

Keys use `<section>.<key>=<value>`. Booleans accept `true/false`, `1/0`, and
`on/off`. Integers may be decimal or `0x` hexadecimal. Unknown keys are rejected
at load time.

## Runtime

| Key | Default | Description |
|---|---:|---|
| `runtime.tick_hz` | `1.0` | Main loop and telemetry rate. |
| `runtime.bench_mode` | `false` | Enables bench-only commands after `ARM_DEBUG`, including open-loop manual heater duty commands without temperature feedback or scheduler clamping. |
| `runtime.debug_arm_code` | `COATHEAL_DEBUG` | Debug token; change before serious testing. |
| `runtime.use_simulated_pwm` | `false` | Uses simulated heater/stepper/LED backends. |
| `runtime.use_simulated_sensors` | `false` | Explicitly enables synthetic sensor data. Real mode never silently falls back to simulation. |
| `runtime.gpio_chip` | `/dev/gpiochip0` | GPIO chip for heaters, LEDs, and sensor GPIO. Motors use `motor*.gpio_chip`. |

## Manual Control

| Key | Default | Description |
|---|---:|---|
| `manual.manual_first` | `true` | Connected operation is operator-directed. |
| `manual.link_loss_fallback_enabled` | `true` | Enables fallback after link loss. |
| `manual.link_loss_fallback_s` | `10.0` | Seconds before fallback activates. |

## Link-Loss Failsafe Plan

The operator-armed bend plan (`FALLBACK_PLAN` / `FALLBACK_ARM`, see
[protocol.md](protocol.md#link-loss-failsafe-plan)) runs only during link-loss
fallback at `PRE_FLOAT`/`FLOAT`.

| Key | Default | Description |
|---|---:|---|
| `fallback.bend_min_c` | `-40.0` | Lower bound of the sample-group temperature window that lets a motor's bend start. |
| `fallback.bend_max_c` | `40.0` | Upper bound of that window; must be greater than `fallback.bend_min_c`. |
| `fallback.bend_deadline_s` | `1800.0` | Seconds after fallback first holds at `PRE_FLOAT`/`FLOAT` beyond which the bend starts regardless of temperature (a motor still not enabled/zeroed/healthy is skipped instead). Must be `>= 0`; the clock restarts after an onboard restart. |
| `fallback.landed_safe` | `true` | Heaters off and both motors disabled the first time fallback is active at `LANDED`. |

## Communications

| Key | Default | Description |
|---|---:|---|
| `comms.telemetry_host` | empty | Leave empty for plug-and-play command-peer targeting. |
| `comms.static_ground_ip` | empty | Optional fixed laptop IP. |
| `comms.static_pi_ip` | `169.254.10.10` | Static onboard link-local IP. |
| `comms.telemetry_port` | `4000` | TCP port on ground station. |
| `comms.command_port` | `5000` | TCP command server port on Pi. |
| `comms.discovery_enabled` | `true` | UDP discovery enabled. |
| `comms.discovery_port` | `4100` | UDP discovery port. |
| `comms.reconnect_ms`, `discovery_period_ms`, `rediscover_period_s`, `failover_grace_s`, `priority` | see INI | Link retry/failover tuning. |

## Storage

| Key | Default | Description |
|---|---:|---|
| `storage.primary_log_path` | `logs/onboard_primary.csv` | Primary telemetry CSV. |
| `storage.secondary_log_path` | `logs/onboard_usb_mirror.csv` | USB mirror CSV. |
| `storage.queue_dir` | `logs/telemetry-queue` | Durable telemetry queue. |
| `storage.queue_retention_hours` | `72.0` | Queue retention. |
| `storage.queue_max_bytes` | `8589934592` | Queue size cap. |

## Hardware Counts

| Key | Default | Description |
|---|---:|---|
| `hardware.sample_count` | `8` | Software sample channels, one per Sequent RTD HAT card channel; must equal the number of specimens `motor0.specimens` and `motor1.specimens` list together. |
| `hardware.heater_count` | `6` | Polyimide heater channels; must equal the number of heated specimens in those lists. |
| `hardware.electronics_heater_index` | `SIZE_MAX` | Optional box heater; omitted for final BOM. |

## Sensors

| Key | Default | Description |
|---|---:|---|
| `sensor.dps310_enabled`, `ads1115_enabled` | `true`, `true` | Enable each independent polling worker. |
| `sensor.dps310_auto_discover`, `ads1115_auto_discover` | `true` | Try only the safe address/path alternatives documented in the bring-up guide. |
| `sensor.dps310_poll_ms`, `ads1115_poll_ms` | `1000` | Independent worker polling intervals. |
| `sensor.stale_after_ms` | `3000` | Age after which a last-good failed reading is `STALE`. |
| `sensor.sequent_rtd_stack` | `0` | Sequent RTD HAT DIP-switch stack level, `0..7` -> I2C `0x40..0x47`. |
| `sensor.sequent_rtd_poll_ms` | `1000` | RTD worker polling interval. |
| `sensor.sequent_rtd_expect_sensor_type` | `pt100` | Expected card-configured sensor type. **`pt100` is the only accepted value**; `Probe()` refuses on mismatch with the card. `pt1000` is recognised and rejected at config load: the card and `Probe()` handle it, but the card-vs-CVD cross-check hardcodes the PT100 Callendar-Van Dusen curve and the resistance window below is a PT100 window, so a `pt1000` config would load and then mark every channel invalid forever — every heater clamped, no diagnostic. |
| `sensor.sequent_rtd_resistance_min_ohm` | `60.0` | Lower plausibility bound for per-channel resistance; must be below `_max_ohm`. |
| `sensor.sequent_rtd_resistance_max_ohm` | `390.0` | Upper plausibility bound for per-channel resistance. |
| `sensor.sequent_rtd_crosscheck_tol_c` | `2.0` | Max allowed disagreement between the card's reported temperature and the temperature derived from its own resistance reading before a channel is marked invalid. |
| `sensor.pressure_source` | `dps310` | Final pressure/ambient-T source. |
| `sensor.dps310_i2c_addr` | `0x77` | DPS310 I2C address. |
| `sensor.uv_source` | `guva_s12sd_ads1115` | Final UV path. |
| `sensor.ads1115_i2c_addr` | `0x48` | ADS1115 I2C address. |
| `sensor.uv_ads1115_channel` | `0` | ADS1115 channel for GUVA-S12SD output. |
| `sensor.uv_full_scale_v` | `4.096` | ADC full-scale used for normalization. |
| `sensor.resistance_source` | `max31865_click` | Source for the compatibility `RESISTANCE=` field. `max31865_click` (v3-shipped default): coating-specimen resistance measured directly by the two MAX31865 clicks, in the two click slots only — the first specimen of each motor ([Motor groups](#motor-groups-motorspecimens)); other slots emit `-`. `sequent_rtd`: the RTD card's per-channel PT100 element resistance, all eight slots. `disabled`: `-` in every slot. `simulated`: the decaying bench model. |
| `sensor.max31865_reference_ohm` | `470.0` | MAX31865 reference resistor value (Ω), shared by both clicks. MikroE RTD Click nominal; **bench-confirm against the populated part** — the retired pre-migration code assumed `400`. See [Sequent RTD Bench Bring-Up §9](sequent-rtd-bring-up.md#9-max31865-sample-resistance-click-bring-up-blocking-gates), gate 4. |
| `sensor.max31865_poll_ms` | `1000` | MAX31865 click worker polling interval. |

Click 1 / SAMPLE1 (CE1, `/dev/spidev0.1`) reads the first specimen of
`motor0.specimens` and click 2 / SAMPLE2 (CE0, `/dev/spidev0.0`) the first of
`motor1.specimens` — owner decision (2026-08-29): resistance is measured on
exactly two specimens, one per motor group. The retired
`sensor.max31865_sample_indices` key is derived from the specimen lists; see
[Sequent RTD Bench Bring-Up §10](sequent-rtd-bring-up.md#10-sample-index-mapping-max31865_sample_indices).

The default `60.0 .. 390.0` Ω `sequent_rtd_resistance_*` window is a PT100
*sensor-range* sanity check, not a mission-envelope check: through the PT100
CVD curve it spans roughly −102 °C to +845 °C, far wider than anything this
payload should ever see. It catches an open, shorted, or miswired probe and
nothing subtler — the actual thermal guard is the `heater.max_sample_temp_c`
over-temp latch at 80 °C. The bench survey in section 8 of
[Sequent RTD Bench Bring-Up](sequent-rtd-bring-up.md) is expected to replace
these defaults with a narrower mission-envelope window. This window applies
only to the `sequent_rtd` resistance path — the MAX31865 click instrument
deliberately has no plausibility window of its own; an out-of-range coating
specimen is expected to saturate and be reported as such, not clamped to a
guessed range (section 9, gate 5 of the same bring-up doc).

## Heater Control

| Key | Default | Description |
|---|---:|---|
| `heater.max_sample_temp_c` | `80.0` | Per-channel overtemperature latch: a heater whose sample reads above it is held off until `RESET_CTRL`. Owner rule 2026-09-15 (was 85); `migrate-config` pins it. |
| `heater.target_min_c` | `0.0` | Lowest accepted manual PID target. |
| `heater.target_max_c` | `75.0` | Highest accepted manual PID target; must stay below the overtemperature latch — 5 °C under it, because a film heater overshoots its target. Owner rule 2026-09-15 (was 80); `migrate-config` pins it. The console asks for confirmation before any target above 40 °C. |
| `heater.pwm_frequency_hz` | `1.0` | Requested heater PWM frequency. v3: film heaters have high thermal inertia and no hardware PWM channel is wired, so 1 Hz software PWM is the owner-confirmed rate (was 10.0 pre-v3). The software PWM period is divided into 100 slices and the duty is re-read every slice, so a `SetDuty(0)` from the motion heater-inhibit reaches the GPIO within one slice — 1000/100 = **10 ms** at 1 Hz — not one whole period. |
| `heater.active_high` | `true` | MOSFET input polarity. |
| `heater.debug_max_duty` | `0.25` | Bench-only maximum `HEATER_TEST` duty. |
| `heater.debug_max_seconds` | `10.0` | Bench-only maximum `HEATER_TEST` duration. |
| `heater.max_duty` | `1.0` | Global heater power ceiling `(0, 1]`: caps the PID output and every duty override in `ThermalController`, and `SET_HEATER_DUTY` / `SET_ALL_DUTY` / `HEATER_TEST` above it are NACKed. Bound the heater film's surface temperature on the bench (~`0.25` for a 5 W film with a lagging PT100) instead of lowering setpoints; `1.0` keeps flight behavior unchanged. |

## Power

| Key | Default | Description |
|---|---:|---|
| `power.max_active_heaters` | `3` | Scheduler limit. Owner power-budget rule: never more than 3 of the six 5 W heaters energised simultaneously (was 4 pre-v3). |
| `power.max_thermal_w` | `15.0` | Thermal power cap: `3 * power.heater_nominal_w` (was 20.0 pre-v3). |
| `power.max_system_w` | `48.23` | Informational system budget. |
| `power.heater_nominal_w` | `5.0` | Per-heater nominal power. |
| `power.energy_budget_wh` | `130.0` | Heater energy latch threshold; `0` disables. |
| `power.logic_regulator_v` | `5.0` | Pololu D24V50F5 rail. |
| `power.stepper_regulator_v` | `12.0` | Pololu D42V110F12 rail. |

## Phase and PID

| Key | Default | Description |
|---|---:|---|
| `phase.sample_floor_c` | `5.0` | Link-loss fallback sample floor. |
| `phase.uniformity_tolerance_c` | `2.0` | Sample spread soft flag threshold. |
| `sensor.ambient_temp_min_c` / `_max_c` | `-90.0` / `50.0` | Ambient temperature range check. |
| `sensor.ambient_pressure_min_mbar` / `_max_mbar` | `5.0` / `1050.0` | Pressure range check. |
| `pid.kp`, `pid.ki`, `pid.kd` | `0.20`, `0.02`, `0.03` | Startup PID gains for manual targets and fallback floor control. |

## Fallback Transitions

| Key | Default | Description |
|---|---:|---|
| `transition.pre_float_mbar` | `150.0` | Fallback ASCENT -> PRE_FLOAT threshold. |
| `transition.ascent_to_float_mbar` | `100.0` | Legacy compatibility threshold. |
| `transition.float_to_descent_mbar` | `300.0` | Fallback FLOAT -> DESCENT threshold. |
| `transition.descent_to_landed_mbar` | `800.0` | Fallback DESCENT -> LANDED threshold. |
| `transition.debounce_samples` | `5` | Consecutive samples required. |

## Motion

| Key | Default | Description |
|---|---:|---|
| `stepper.steps_per_rev` | `200` | NEMA 17 full-step count. |
| `stepper.default_step_hz` | `50.0` | Default jog rate, full-steps/s (clamped to the speed ceiling). |
| `stepper.max_position_steps` | `200000` | Absolute software travel limit. |
| `stepper.enable_on_boot` | `false` | Keep drivers de-energized until commanded. |
| `stepper.lead_mm_per_rev` | `2.0` | Ball-screw lead: linear travel per motor revolution. The mm command surface (`STEPPER_MOVE_MM`, `STEPPER_MOVETO_MM`) and the `mm`/`mm_tgt` telemetry keys convert through this value. Validated `(0, 100]`. |
| `stepper.max_accel_steps_per_s2` | `5000.0` | Ceiling for `STEPPER_SET_ACCEL` and the per-motor accel overrides, full-steps/s². |
| `stepper.max_speed_mm_s` | `0.5` | Linear speed ceiling for every motion path (jog, bend, sequences, fallback plan, pulls), in mm/s of ball-screw travel. Converted through `stepper.lead_mm_per_rev` and `stepper.steps_per_rev` into full-steps/s (0.5 mm/s = 50 full-steps/s at the 2 mm lead); the effective ceiling is the lower of this and `pull.max_step_hz`. Validated `(0, 100]`. Owner rule 2026-09-11. |
| `stepper.max_direct_usteps` | `1000` | Longest raw-microstep command accepted: `\|steps\|` of `STEPPER_MOVE`, `\|target\|` of `STEPPER_MOVETO` / `STEPPER_BEND`, in microsteps at the live divisor (1000 = 1.25 rev = 2.5 mm at µ4 and the 2 mm lead). The mm commands, bend sequences and the fallback plan are bounded by `stepper.max_position_steps` instead. Validated `(0, stepper.max_position_steps]`. Owner rule 2026-09-11. |
| `pull.max_step_hz` | `50.0` | Pull cycle rate and legacy speed ceiling, full-steps/s. The effective ceiling for every motion path is the lower of this and `stepper.max_speed_mm_s` converted through the lead; a higher value here is clamped (the start-up journal line `[stepper] speed ceiling …` says which binds). |
| `pull.accel_steps_per_s2` | `200.0` | Trapezoidal acceleration/deceleration shared by all motion (not just pulls) unless a motor overrides it. Must be ≤ `stepper.max_accel_steps_per_s2`. |
| `pull.microstep` | `4` | Microstep divisor programmed into each TMC5160. |
| `pull.travel_full_steps` | `200` | Pull travel in full steps; calibrate to ball-screw lead. |
| `pull.hold_s` | `5.0` | Hold time at target. |

## Motor Channels

Schematic v3: TMC5160 SPI-only motion (position dribble via XTARGET writes —
see [TMC5160 Commissioning](tmc5160-commissioning.md)). **There is no
`step_line`, `dir_line`, or `pulse_high_us` key; those are rejected at config
load as unknown motor keys, not merely deprecated.**

| Key | Motor 0 default | Motor 1 default | Description |
|---|---:|---:|---|
| `motor*.driver` | `tmc5160` | `tmc5160` | Required driver type; only `tmc5160` and `simulated` are accepted. `tmc2240` is rejected at load, naming it retired. |
| `motor*.gpio_chip` | `/dev/gpiochip0` | `/dev/gpiochip0` | GPIO chip containing the CS and EN lines. |
| `motor*.spi_device` | `/dev/spidev0.0` | `/dev/spidev0.0` | Shared SPI0 bus device; software drives each configured CS GPIO with `SPI_NO_CS` (see below). |
| `motor*.cs_line` | `22` | `27` | Chip select GPIO (software CS). |
| `motor*.enable_line` | `20` | `21` | EN GPIO. |
| `motor*.invert_direction` | `true` (built-in `false`) | `false` | Flip the sign of every step so a positive command moves the mechanism the same physical way on both motors. Motor 0 turns opposite to motor 1 for the same command (owner 2026-09-15: its "+" was "−"), so `migrate-config` pins `motor0.invert_direction=true`. Fix direction here, never by rewiring the coils. |
| `motor*.run_current_a_rms` | `0.8` | `0.8` | Conservative commissioning current; increase only after thermal validation. Validated against both a flat `(0, 3.1]` A_rms ceiling and the sense resistor's physical current limit (below). `STEPPER_SET_CURRENT` changes it at runtime (same limits) until the service restarts. |
| `motor*.hold_current_frac` | `0.30` | `0.30` | Hold current fraction, relative to the chosen IRUN. |
| `motor*.stealth_chop` | `false` | `false` | Chopper mode (GCONF `en_pwm_mode`, bit 2). `false` — the flight setting since 2026-09-11 — writes `GCONF=0x00000000`: spreadCycle, full torque headroom for the ball-screw bend; `true` writes `GCONF=0x00000004`: StealthChop, the quiet low-speed chopper, bench opt-in only. The driver's GCONF readback verify confirms the mode during `Reinitialize()`, and `MOTOR_DEBUG` reports the live `stealth` flag. |
| `motor*.spi_speed_hz` | `1000000` | `1000000` | SPI speed. |
| `motor*.sense_resistor_ohm` | `0.075` | `0.075` | TMC5160 current-sense resistor value (Ω); feeds the GLOBALSCALER/IHOLD_IRUN current calculation. `0.075` is an assumed typical value for this board family — **read the actual value off the board at the bench** (see [TMC5160 Commissioning §6](tmc5160-commissioning.md#6-current-model-globalscaler--irun-two-regimes)). Validated `> 0.0 && < 1.0`. |
| `motor*.retry_ms` | `2000` | `2000` | Idle driver re-probe interval after a fault. |
| `motor*.accel_steps_per_s2` | `0` | `0` | Per-motor trapezoid slope, full-steps/s². `0` inherits `pull.accel_steps_per_s2`; a positive value (≤ `stepper.max_accel_steps_per_s2`) overrides it for this motor. `STEPPER_SET_ACCEL` adjusts it at runtime until restart. |
| `motor*.specimens` | `ch1:19,ch2:13,ch3:6,ch4:5` | `ch5:24,ch6:23,ch7,ch8` | The specimens the motor pulls, each with its PT100 terminal and heater line — see [Motor groups](#motor-groups-motorspecimens). |

### Motor groups (`motor*.specimens`)

Which specimens each motor pulls, which Sequent RTD card terminal each
specimen's PT100 is on and which GPIO line drives its heater are one fact of
the bench wiring, written once per motor:

```ini
motor0.specimens=ch8:19,ch2:13,ch3:6,ch4:5
motor1.specimens=ch5:24,ch7:23,ch1,ch6
```

Each entry is `ch<terminal>:<BCM line>` for a heated specimen or
`ch<terminal>` for an unheated one (`bcm19` is accepted for `19`). The
onboard numbers everything from these two lists:

- **Samples**: motor 0's specimens are S0, S1, … in the order listed; motor
  1's follow (S4… above).
- **Heaters**: the heated specimens are H0, H1, … in the same order (above:
  H0–H3 on motor 0, H4 = S4 and H5 = S5 on motor 1); heater `h` reads the
  PT100 of its own specimen.
- **Clicks**: MAX31865 click 1 reads the first specimen of motor 0, click 2
  the first of motor 1 — put the specimen whose resistance you monitor
  first.

The load is refused unless both lists are set, together they list exactly
`hardware.sample_count` specimens of which exactly `hardware.heater_count`
are heated, every terminal is `ch1..ch8` and appears once, and no BCM line
heats two specimens (heater lines also join the duplicate-GPIO check against
motors and LEDs). The boot-time `gpio=` block that holds heater lines low
from power-on is derived from the lists — redeploy and reboot after changing
a line.

`GET_LAYOUT` reports the derived groups, and the ground station arranges its
Thermal tab, motor cards, Values column, plots and `session.json` by them.
`scripts/associate_heaters.py` shows the lists, measures the heater/PT100
pairs (`auto`, `heat`), finds each specimen's motor and click by hand
(`touch`) and writes them (`auto`, `touch`, `assign`); `migrate-config` (every `coatheal-deploy`) keeps them
from the local config.

The index-based keys the lists replaced — `heater.output_lines`,
`heater.temperature_channels`, `sensor.sequent_rtd_channels`,
`sensor.max31865_sample_indices`, `motor0.samples`, `motor1.samples` — are
derived from them: an INI that sets both is refused (`<key> is derived from
motor0.specimens / motor1.specimens: remove it`). An INI with none of the
specimen keys still loads the old keys, and `migrate-config` converts it to
specimen lists (each motor's click sample first).

The TMC5160 backend uses SPI mode 3, opens SPI with the kernel chip-select
disabled (`SPI_NO_CS`), and drives `motor0.cs_line`/`motor1.cs_line` through
libgpiod as software chip-selects. This is mandatory, not an optimisation:
SPI0's native chip-selects (CE0/CE1) are wired to the MAX31865
sample-resistance clicks, not the motors — without `SPI_NO_CS` the kernel
would assert a click's CE line on every motor transfer. Do not install a
`spi0-2cs` overlay; it would reserve BCM 22/27 in the kernel and conflict
with the software-controlled chip selects.

`run_current_a_rms` and `sense_resistor_ohm` feed
`Tmc5160Driver::CalculateCurrent()`, which derives GLOBALSCALER (32..256) and
IRUN (0..31) directly — there is no TMC2240-style fixed peak-current range
selector, and `current_range_a_peak` no longer exists as a key. At the
assumed `sense_resistor_ohm=0.075`, the sense resistor can deliver at most
about 3.06 A_rms; very small requests are rejected rather than silently
overcurrenting the motor. That low-current floor is ~1.5 % of `I_peak_max` =
0.065 A **peak** (≈ 0.046 A_rms), and acceptance near it is ragged because
IRUN is a 5-bit ladder — measured, the first crossover is ≈ 0.032–0.033 A_rms
and everything above ≈ 0.055 A_rms is accepted. See
[TMC5160 Commissioning §6](tmc5160-commissioning.md#6-current-model-globalscaler--irun-two-regimes)
for the full derivation and the bench-confirmation blank for the real sense
resistor value.

## HAL

| Key | Default | Description |
|---|---:|---|
| `hal.status_led_enabled` | `false` | No status LED is present in the final pinout. |
| `hal.mode_led_enabled` | `false` | No mode LED is present in the final pinout. |
| `hal.status_led_line` | `17` | Heartbeat LED GPIO. |
| `hal.mode_led_line` | `27` | Mode LED GPIO. |

Disabled LED line values are not claimed. Configuration validation rejects any
duplicate active BCM assignment across heaters, motors, and LEDs. The Sequent
RTD HAT is I2C-only and claims no GPIO line.

Rev C requires `manual.manual_first=true`. Legacy `fatigue.*` and `bend.*`
configuration keys are rejected; runtime `BENDSEQ_*` commands are the only
sequence definition.

See [Component Configuration and Bring-Up](component-configuration-and-bring-up.md)
for wiring, discovery, scan, validation, and commissioning commands.
