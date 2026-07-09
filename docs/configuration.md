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
| `runtime.bench_mode` | `false` | Enables bench-only commands after `ARM_DEBUG`. |
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
| `hardware.sample_count` | `8` | Software sample channels. Current bench uses RTD Click on `S1` for heater 1; DAQ132M can later fill all eight. |
| `hardware.heater_count` | `6` | Polyimide heater channels, samples 0-5. |
| `hardware.electronics_heater_index` | `SIZE_MAX` | Optional box heater; omitted for final BOM. |

## Sensors

| Key | Default | Description |
|---|---:|---|
| `sensor.sample_temperature_source` | `rtd_click_max31865` | Current PT100 acquisition path. `daq132m_modbus` remains available later. |
| `sensor.dps310_enabled`, `ads1115_enabled`, `daq132m_enabled` | `true`, `true`, `false` | Enable each independent polling worker. |
| `sensor.dps310_auto_discover`, `ads1115_auto_discover`, `daq132m_auto_discover` | `true` | Try only the safe address/path alternatives documented in the bring-up guide. |
| `sensor.dps310_poll_ms`, `ads1115_poll_ms`, `daq132m_poll_ms` | `1000` | Independent worker polling intervals. |
| `sensor.stale_after_ms` | `3000` | Age after which a last-good failed reading is `STALE`. |
| `sensor.daq132m_device` | `/dev/ttyUSB0` | USB-RS485 converter path. |
| `sensor.daq132m_baud` | `9600` | Modbus RTU baud rate. |
| `sensor.daq132m_parity` | `N` | Modbus parity. |
| `sensor.daq132m_data_bits` / `stop_bits` | `8` / `1` | Serial framing. |
| `sensor.daq132m_slave_id` | `1` | DAQ132M Modbus slave ID. |
| `sensor.daq132m_function_code` | `3` | Modbus register-read function, `3` or `4`. |
| `sensor.daq132m_register_base` | `0` | First temperature register. |
| `sensor.daq132m_register_count` | `8` | Number of PT100 registers. Must cover `sample_count`. |
| `sensor.daq132m_c_per_count` | `0.1` | Temperature scale; verify against DAQ132M manual. |
| `sensor.daq132m_c_offset` | `0.0` | Temperature offset applied after scaling. |
| `sensor.daq132m_enabled_channels` | `0..7` | Zero-based channels expected to be connected. Physical channel 2 is software `S1`. |
| `sensor.rtd_click_enabled` | `true` | Current MIKROE-2815/MAX31865 bench path. |
| `sensor.rtd_click_spi_device` | `/dev/spidev0.0` | RTD Click SPI path if enabled. |
| `sensor.rtd_click_cs_line` / `drdy_line` | `16` / `25` | RTD Click CS and DRDY GPIO. |
| `sensor.rtd_click_wires` | `3` | PT100 wire mode; must be 2, 3, or 4. |
| `sensor.rtd_click_sample_channel` | `1` | Software sample channel populated by RTD Click. |
| `sensor.rtd_click_reference_ohm` | `400.0` | MAX31865 reference resistor value used for PT100 conversion. |
| `sensor.rtd_click_filter_hz` | `50` | MAX31865 mains filter, `50` or `60`. |
| `sensor.rtd_click_spi_speed_hz` | `500000` | RTD Click SPI speed. |
| `sensor.pressure_source` | `dps310` | Final pressure/ambient-T source. |
| `sensor.dps310_i2c_addr` | `0x77` | DPS310 I2C address. |
| `sensor.uv_source` | `guva_s12sd_ads1115` | Final UV path. |
| `sensor.ads1115_i2c_addr` | `0x48` | ADS1115 I2C address. |
| `sensor.uv_ads1115_channel` | `0` | ADS1115 channel for GUVA-S12SD output. |
| `sensor.uv_full_scale_v` | `4.096` | ADC full-scale used for normalization. |
| `sensor.resistance_source` | `disabled` | Final BOM has no resistance instrument; telemetry emits `-`. |

## Heater Control

| Key | Default | Description |
|---|---:|---|
| `heater.max_sample_temp_c` | `85.0` | Per-sample overtemperature latch. |
| `heater.target_min_c` | `0.0` | Lowest accepted manual PID target. |
| `heater.target_max_c` | `80.0` | Highest accepted manual PID target; must stay below the overtemperature latch. |
| `heater.output_lines` | `17,18,27,5,6,13` | BCM GPIO lines for HEAT_EN1..6. |
| `heater.temperature_channels` | `0,1,2,3,4,5` | DAQ sample supplying feedback for H0..H5. |
| `heater.pwm_frequency_hz` | `10.0` | Requested heater PWM frequency. |
| `heater.active_high` | `true` | MOSFET input polarity. |
| `heater.debug_max_duty` | `0.25` | Bench-only maximum `HEATER_TEST` duty. |
| `heater.debug_max_seconds` | `10.0` | Bench-only maximum `HEATER_TEST` duration. |

## Power

| Key | Default | Description |
|---|---:|---|
| `power.max_active_heaters` | `4` | Scheduler limit. |
| `power.max_thermal_w` | `20.0` | Thermal power cap. |
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
| `stepper.default_step_hz` | `100.0` | Default jog rate. |
| `stepper.max_position_steps` | `200000` | Absolute software travel limit. |
| `stepper.enable_on_boot` | `false` | Keep drivers de-energized until commanded. |
| `pull.max_step_hz` | `100.0` | Pull cycle max rate. |
| `pull.accel_steps_per_s2` | `200.0` | Pull acceleration/deceleration. |
| `pull.microstep` | `4` | Microstep divisor programmed into each TMC2240. |
| `pull.travel_full_steps` | `200` | Pull travel in full steps; calibrate to ball-screw lead. |
| `pull.hold_s` | `5.0` | Hold time at target. |

## Motor Channels

| Key | Motor 0 default | Motor 1 default | Description |
|---|---:|---:|---|
| `motor*.driver` | `tmc2240` | `tmc2240` | Required final driver type. |
| `motor*.gpio_chip` | `/dev/gpiochip0` | `/dev/gpiochip0` | GPIO chip containing CS, STEP, DIR, and EN lines. |
| `motor*.spi_device` | `/dev/spidev0.0` | `/dev/spidev0.0` | Shared SPI0 bus device; software drives each configured CS GPIO. |
| `motor*.cs_line` | `22` | `23` | Chip select GPIO. |
| `motor*.step_line` | `19` | `16` | STEP GPIO. |
| `motor*.dir_line` | `26` | `20` | DIR GPIO. |
| `motor*.enable_line` | `12` | `21` | EN GPIO. |
| `motor*.run_current_a_rms` | `0.8` | `0.8` | Conservative commissioning current; increase only after thermal validation. |
| `motor*.current_range_a_peak` | `0` | `0` | TMC2240 peak-current range: `0` auto, or exactly `1`, `2`, or `3` A. |
| `motor*.hold_current_frac` | `0.30` | `0.30` | Hold current fraction. |
| `motor*.stealth_chop` | `true` | `true` | StealthChop request. |
| `motor*.spi_speed_hz` | `1000000` | `1000000` | SPI speed. |
| `motor*.pulse_high_us` | `3` | `3` | STEP high time. |
| `motor*.retry_ms` | `2000` | `2000` | Idle driver re-probe interval after a fault. |
| `motor*.samples` | `0,1,2,3` | `4,5,6,7` | Sample indices pulled by the motor. |

The TMC2240 backend uses SPI mode 3, opens SPI with kernel chip-select
disabled, and drives
`motor0.cs_line` and `motor1.cs_line` through libgpiod. Do not install the old
`spi0-2cs,cs0_pin=22,cs1_pin=23` overlay because it would reserve those GPIO
lines in the kernel and conflict with the software-controlled chip selects.
`run_current_a_rms` is converted to the integrated-sense `CURRENT_RANGE` and
`GLOBALSCALER` settings; external phase-sense-resistor keys are invalid.

## HAL

| Key | Default | Description |
|---|---:|---|
| `hal.status_led_enabled` | `false` | No status LED is present in the final pinout. |
| `hal.mode_led_enabled` | `false` | No mode LED is present in the final pinout. |
| `hal.status_led_line` | `17` | Heartbeat LED GPIO. |
| `hal.mode_led_line` | `27` | Mode LED GPIO. |

Disabled LED line values are not claimed. Configuration validation rejects any
duplicate active BCM assignment across heaters, motors, RTD Click, and LEDs.

Rev C requires `manual.manual_first=true`. Legacy `fatigue.*` and `bend.*`
configuration keys are rejected; runtime `BENDSEQ_*` commands are the only
sequence definition.

See [Component Configuration and Bring-Up](component-configuration-and-bring-up.md)
for wiring, discovery, scan, validation, and commissioning commands.
