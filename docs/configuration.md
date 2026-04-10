# Configuration Reference

All onboard configuration is loaded from an INI file at startup via `--config <path>`. See `config/onboard.example.ini` for a working example.

## Format

Keys use the form `<section>.<key>=<value>`. Boolean values are `true`/`false`. Strings are unquoted.

```ini
runtime.tick_hz=1.0
comms.telemetry_host=192.168.50.1
```

---

## `[runtime]`

General process behavior.

| Key | Type | Default | Description |
|---|---|---|---|
| `runtime.tick_hz` | float | `1.0` | Main loop frequency in Hz. 1 Hz = one telemetry frame per second. |
| `runtime.bench_mode` | bool | `false` | Enable bench/simulation mode. Required to use extended debug commands. |
| `runtime.debug_arm_code` | string | `COATHEAL_DEBUG` | Token required by `ARM_DEBUG <token>` to unlock extended commands. Change before flight. |
| `runtime.use_simulated_pwm` | bool | `false` | Use in-memory `SimulatedPwmController` instead of real GPIO PWM. Automatically true in bench mode if no GPIO hardware is present. |
| `runtime.gpio_chip` | string | `/dev/gpiochip0` | `libgpiod` GPIO chip device path. Only used when `use_simulated_pwm=false`. |

---

## `[comms]`

Network connectivity for telemetry and command links.

| Key | Type | Default | Description |
|---|---|---|---|
| `comms.telemetry_host` | string | `127.0.0.1` | IP address of the ground station telemetry server. The Pi **connects** to this host on `telemetry_port`. |
| `comms.static_ground_ip` | string | `192.168.50.1` | Static fallback ground station IP when UDP discovery fails. |
| `comms.static_pi_ip` | string | `192.168.50.2` | Static onboard IP (used for reference; not bound directly). |
| `comms.telemetry_port` | int | `4000` | TCP port the Pi connects to for telemetry. |
| `comms.command_port` | int | `5000` | TCP port the Pi **listens on** for incoming commands. |
| `comms.reconnect_ms` | int | `2000` | ACK timeout in milliseconds. If no ACK is received within this window, the Pi closes the TCP connection and retries. Must be longer than the ground station's `accept()` cycle time. |
| `comms.discovery_enabled` | bool | `true` | Enable UDP discovery. The Pi listens on `discovery_port` for `GS_HELLO` broadcasts. |
| `comms.discovery_port` | int | `4100` | UDP port for discovery beacons. Must match the ground station's `--discovery-port`. |

---

## `[storage]`

Persistent data logging.

| Key | Type | Default | Description |
|---|---|---|---|
| `storage.primary_log_path` | string | `logs/onboard_primary.csv` | Primary CSV telemetry log path (SD card). Relative to the working directory, or absolute. |
| `storage.secondary_log_path` | string | `logs/onboard_usb_mirror.csv` | Secondary CSV mirror path (USB drive). Writes continue if primary fails. |
| `storage.queue_dir` | string | `logs/telemetry-queue` | Directory for the durable disk-backed telemetry queue. Survives process restarts. |
| `storage.queue_retention_hours` | float | `72.0` | Maximum age of queued frames in hours. Older frames are pruned on startup and periodically. |
| `storage.queue_max_bytes` | uint64 | `8589934592` | Maximum total queue size in bytes (default 8 GiB). Excess frames are pruned oldest-first. |

---

## `[phase]`

Thermal setpoints and ramp parameters for each mission phase.

| Key | Type | Default | Description |
|---|---|---|---|
| `phase.ascent_target_c` | float | `-30.0` | Sample temperature setpoint during `ASCENT_HOLD` (°C). |
| `phase.activation_target_c` | float | `70.0` | Final target temperature for `ACTIVATION_RAMP` (°C). Ramp ends when this is reached. |
| `phase.float_target_c` | float | `70.0` | Sample temperature setpoint during `FLOAT_HOLD` (°C). |
| `phase.descent_floor_c` | float | `-20.0` | Sample temperature floor during `DESCENT_FLOOR` (°C). Heaters activate only if samples drop below this. |
| `phase.box_target_c` | float | `0.0` | Electronics box temperature setpoint for the box PID (°C). Set to 0 to disable box heating. |
| `phase.activation_ramp_c_per_s` | float | `0.85` | Ramp rate during `ACTIVATION_RAMP` in °C per second. Controls how quickly the setpoint increases toward `activation_target_c`. |
| `phase.float_hold_minutes` | float | `90.0` | Duration of `FLOAT_HOLD` before descending to `DESCENT_FLOOR` (minutes). Also the timeout fallback if pressure fails to indicate descent. |

---

## `[transition]`

Pressure thresholds that trigger automatic phase transitions.

| Key | Type | Default | Description |
|---|---|---|---|
| `transition.ascent_to_activation_mbar` | float | `140.0` | Ambient pressure below which `ASCENT_HOLD` → `ACTIVATION_RAMP` (mbar). Corresponds to ~14 km altitude. |
| `transition.float_to_descent_mbar` | float | `300.0` | Ambient pressure above which `FLOAT_HOLD` → `DESCENT_FLOOR` (mbar). Corresponds to balloon descending through ~9 km. |

---

## `[power]`

Heater power budget enforced by `HeaterScheduler`.

| Key | Type | Default | Description |
|---|---|---|---|
| `power.max_active_heaters` | size_t | `4` | Maximum number of simultaneously active heaters. Heaters beyond this limit are zeroed regardless of requested duty. |
| `power.max_thermal_w` | float | `40.0` | Maximum combined thermal power draw in watts. Duty cycles are clamped proportionally if this limit is approached. |
| `power.max_system_w` | float | `48.23` | Total system power budget in watts (informational; used for documentation and thermal modelling). Not enforced directly in software. |
| `power.heater_nominal_w` | float | `10.0` | Nominal power per heater at 100% duty cycle (watts). Used to convert duty cycles to estimated wattage. |
| `power.energy_budget_wh` | float | `130.0` | Cumulative heater-energy budget in Wh enforced by `HeaterScheduler` (BEXUS User Manual §5.2 — 150 Wh per-team allocation, minus ~20 Wh reserved for the Pi 4 + sensors). Once consumed, all heaters are latched off for the remainder of the mission. Set to `0` to disable enforcement. |

---

## `[pid]`

PID controller gains for sample and box temperature control.

| Key | Type | Default | Description |
|---|---|---|---|
| `pid.kp` | float | `0.20` | Proportional gain for the sample PID controller. |
| `pid.ki` | float | `0.02` | Integral gain for the sample PID controller. |
| `pid.kd` | float | `0.03` | Derivative gain for the sample PID controller. |
| `pid.box_kp` | float | `0.15` | Proportional gain for the box (electronics) PID controller. |
| `pid.box_ki` | float | `0.01` | Integral gain for the box PID controller. |
| `pid.box_kd` | float | `0.02` | Derivative gain for the box PID controller. |

All PID controllers use an integral anti-windup clamp of `[-10, 10]` and output clamp of `[0.0, 1.0]`. Gains can be overridden in flight via the `SET_PID` command (requires ARM_DEBUG).

---

## `[hardware]`

Heater channel mapping.

| Key | Type | Default | Description |
|---|---|---|---|
| `hardware.heater_count` | size_t | `10` | Total number of heater channels (sample heaters + electronics heater). |
| `hardware.electronics_heater_index` | size_t | `9` | Index of the electronics box heater within the duty array. This heater is de-prioritized by the scheduler and driven by the box PID, not the sample PID. |

---

## Notes

- All paths are resolved relative to the **working directory** at launch, unless they are absolute. The systemd service sets `WorkingDirectory=/bexus/code/coatheal`.
- `bench_mode=true` disables hardware I/O (sensors return simulated data) and enables extended debug commands after `ARM_DEBUG`.
- The `debug_arm_code` token is transmitted in plaintext over the command TCP connection. Change it from the default before flight. For additional security, the command server only accepts one client at a time and closes the connection immediately after responding.
- `runtime.tick_hz` is the **boot-time** rate. During flight the operator can change it live with the `SET_TICK_HZ <hz>` command (no restart required, no debug arm required) — the new rate is reported back via `STATUS`. The valid range is `0.1`–`5.0` Hz.
- The systemd unit (`deploy/coatheal-onboard.service`) uses `Type=notify` and `WatchdogSec=10`. The main loop pings the systemd watchdog every tick via `sd_notify(WATCHDOG=1)`; if the loop hangs for >10 s, systemd kills and restarts the process. No external libsystemd dependency is required — the notification protocol is implemented in `onboard/src/sd_notify.cpp`.
