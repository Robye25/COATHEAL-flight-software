#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <string>
#include <vector>

namespace coatheal {

struct RuntimeConfig {
  double tick_hz = 1.0;
  bool bench_mode = false;
  std::string debug_arm_code = "COATHEAL_DEBUG";
  bool use_simulated_pwm = false;
  bool use_simulated_sensors = false;
  std::string gpio_chip = "/dev/gpiochip0";
};

struct ManualControlConfig {
  // Rev C manual-first policy:
  //   * while the ground link is healthy, ARM enables operator-directed
  //     heater and stepper commands; no phase entry starts motion;
  //   * after an established link is lost, the onboard may fall back to the
  //     cold-protection floor controller while active sequences continue.
  bool manual_first = true;
  bool link_loss_fallback_enabled = true;
  double link_loss_fallback_s = 10.0;
};

struct FallbackConfig {
  // Operator-armed bend plan executed only during link-loss fallback
  // (redesign spec §10, owner decisions D3–D6). A motor's bend starts when
  // its sample group is inside the temperature window, or unconditionally
  // once bend_deadline_s have passed since fallback first held at
  // PRE_FLOAT/FLOAT. landed_safe: heaters off + motors disabled the first
  // time fallback is active at LANDED.
  double bend_min_c = -40.0;
  double bend_max_c = 40.0;
  double bend_deadline_s = 1800.0;
  bool landed_safe = true;
};

struct CommsConfig {
  std::string telemetry_host;
  std::string static_ground_ip;
  std::string static_pi_ip = "169.254.10.10";
  int telemetry_port = 4000;
  int command_port = 5000;
  int reconnect_ms = 2000;
  bool discovery_enabled = true;
  int discovery_port = 4100;
  int discovery_period_ms = 2000;
  int rediscover_period_s = 30;
  int failover_grace_s = 5;
  int priority = 100;
};

struct StorageConfig {
  std::string primary_log_path = "logs/onboard_primary.csv";
  std::string secondary_log_path = "logs/onboard_usb_mirror.csv";
  std::string queue_dir = "logs/telemetry-queue";
  double queue_retention_hours = 72.0;
  std::uint64_t queue_max_bytes = 8589934592ULL;  // 8 GiB
};

struct PhaseConfig {
  // Rev C fallback floor shared across ASCENT/FLOAT/DESCENT. Connected
  // operation is manual-first; the floor controller runs only after link loss.
  double sample_floor_c = 5.0;
  double uniformity_tolerance_c = 2.0;
};

struct TransitionConfig {
  // Pressure thresholds used only for link-loss fallback phase tracking.
  double pre_float_mbar = 150.0;
  double ascent_to_float_mbar = 100.0;
  double float_to_descent_mbar = 300.0;
  double descent_to_landed_mbar = 800.0;
  // Number of consecutive pressure samples that must satisfy a threshold before
  // the state machine commits to the transition. Prevents single bad readings
  // from causing irreversible phase changes. Especially important near the
  // 300 mbar sensor accuracy boundary (±6 mbar).
  int debounce_samples = 5;
};

struct HeaterSafetyConfig {
  double max_sample_temp_c = 85.0;
  double target_min_c = 0.0;
  double target_max_c = 80.0;
};

struct SensorRangeConfig {
  double ambient_temp_min_c = -90.0;
  double ambient_temp_max_c = 50.0;
  double ambient_pressure_min_mbar = 5.0;
  double ambient_pressure_max_mbar = 1050.0;
};

struct PowerConfig {
  // Final BOM: 5 W polyimide film heaters. Owner power-budget rule: never
  // more than 3 energized at once, yielding the default 15 W combined
  // thermal draw ceiling.
  std::size_t max_active_heaters = 3;
  double max_thermal_w = 15.0;
  double max_system_w = 48.23;
  double heater_nominal_w = 5.0;
  // BEXUS User Manual §5.2: each team is allocated 150 Wh for the full flight.
  // Pi 4 + sensors consume ~5–10 W continuously, so the heater share is lower.
  // 0 disables enforcement (back-compat).
  double energy_budget_wh = 0.0;
  double logic_regulator_v = 5.0;
  double stepper_regulator_v = 12.0;
};

struct PidConfig {
  double kp = 0.20;
  double ki = 0.02;
  double kd = 0.03;
};

struct HardwareConfig {
  // Final BOM: 8 PT100 sample channels and 6 heated samples. Samples 6 and 7
  // are pulled but unheated. No electronics-box heater; SIZE_MAX means absent.
  std::size_t sample_count = 8;
  std::size_t heater_count = 6;
  std::size_t electronics_heater_index = static_cast<std::size_t>(-1);
};

struct SensorHardwareConfig {
  bool dps310_enabled = true;
  bool ads1115_enabled = true;
  bool dps310_auto_discover = true;
  bool ads1115_auto_discover = true;
  int dps310_poll_ms = 1000;
  int ads1115_poll_ms = 1000;
  int stale_after_ms = 3000;

  // Sequent Microsystems 8-channel RTD HAT. Sole sample-temperature source.
  int sequent_rtd_stack = 0;                     // 0..7 -> I2C 0x40..0x47
  std::vector<std::size_t> sequent_rtd_channels; // card channel per sample
  int sequent_rtd_poll_ms = 1000;
  std::string sequent_rtd_expect_sensor_type = "pt100";
  double sequent_rtd_resistance_min_ohm = 60.0;
  double sequent_rtd_resistance_max_ohm = 390.0;
  double sequent_rtd_crosscheck_tol_c = 2.0;

  std::string pressure_source = "dps310";
  int dps310_i2c_addr = 0x77;

  std::string uv_source = "guva_s12sd_ads1115";
  int ads1115_i2c_addr = 0x48;
  int uv_ads1115_channel = 0;
  double uv_full_scale_v = 4.096;

  // v3 default: the MAX31865 dual-click sample-resistance instrument.
  // "sequent_rtd", "disabled" and "simulated" remain accepted (see
  // config.cpp's validation) for back-compat with fielded/legacy configs.
  std::string resistance_source = "max31865_click";

  // MAX31865 dual-click sample-resistance instrument (schematic v3): two
  // clicks, each wired 4-wire Kelvin to one coating specimen. Device paths
  // are fixed by hardware (SensorManager owns the constants: CE1/GP07 =
  // click 0 = SAMPLE1 = /dev/spidev0.1; CE0/GP08 = click 1 = SAMPLE2 =
  // /dev/spidev0.0) and are therefore not configurable -- these three keys
  // are the only tunables.
  double max31865_reference_ohm = 470.0;
  int max31865_poll_ms = 1000;
  // Which two of hardware.sample_count indices the two clicks feed,
  // click-index-ordered (entry 0 -> click 0/SAMPLE1, entry 1 -> click
  // 1/SAMPLE2). OWNER-FLAGGED PLACEHOLDER: {0, 4} is the first specimen of
  // each motor group (motor0.samples starts at 0, motor1.samples starts at
  // 4) -- config-only to change once the real commissioning mapping from
  // the coating bench is known.
  std::vector<std::size_t> max31865_sample_indices{0, 4};
};

struct HeaterOutputConfig {
  std::vector<std::size_t> output_lines;
  std::vector<std::size_t> temperature_channels;
  // v3: film heaters have high thermal inertia, so a 1 Hz software PWM loop
  // (no hardware PWM channel is wired) is the owner-confirmed rate.
  double pwm_frequency_hz = 1.0;
  bool active_high = true;
  double debug_max_duty = 0.25;
  double debug_max_seconds = 10.0;
};

struct StepperConfig {
  // Shared motion defaults. All electrical wiring and polarity are per motor.
  int steps_per_rev = 200;           // NEMA 17 full-step default
  // Legacy single-channel constructor fields; production INI uses pull.*.
  int microstep = 4;
  double default_step_hz = 100.0;
  double max_step_hz = 100.0;
  std::int64_t max_position_steps = 200000;  // absolute travel limit
  bool enable_on_boot = false;       // stay de-energised until commanded
  // Ball-screw lead: linear travel per motor revolution. The mm command
  // surface (STEPPER_MOVE_MM / STEPPER_MOVETO_MM, mm telemetry keys)
  // converts through this single value; the microstep commands stay raw.
  double lead_mm_per_rev = 2.0;
  // Ceiling for STEPPER_SET_ACCEL and motorN.accel_steps_per_s2, in
  // full-steps/s². Bounds runtime experimentation the same way
  // pull.max_step_hz bounds STEPPER_SET_SPEED.
  double max_accel_steps_per_s2 = 5000.0;
};

struct PullConfig {
  double max_step_hz = 100.0;
  double accel_steps_per_s2 = 200.0;
  int microstep = 4;
  int travel_full_steps = 200;
  double hold_s = 5.0;
};

struct MotorConfig {
  // v3 schematic: TMC5160 SPI-only motion (position dribble via XTARGET).
  // No STEP/DIR lines exist. CS is a software chip-select GPIO because the
  // SPI0 native chip-selects (CE0/CE1) are wired to the MAX31865
  // sample-resistance clicks instead.
  std::string driver = "tmc5160";
  std::string gpio_chip = "/dev/gpiochip0";
  std::string spi_device = "/dev/spidev0.0";
  std::size_t cs_line = 22;
  std::size_t enable_line = 20;
  bool invert_direction = false;
  bool enable_active_low = true;
  // Validated at config load against both an absolute ceiling and the
  // sense resistor's physical current limit -- see config.cpp's per-motor
  // validation block.
  double run_current_a_rms = 0.8;
  double hold_current_frac = 0.30;
  bool stealth_chop = true;
  std::uint32_t spi_speed_hz = 1000000;
  // TMC5160 current-sense resistor value (ohms); feeds the
  // GLOBALSCALER/IHOLD_IRUN current calculation.
  double sense_resistor_ohm = 0.075;
  int retry_ms = 2000;
  // Per-motor trapezoidal acceleration in full-steps/s². 0 (the default)
  // inherits pull.accel_steps_per_s2; a positive value overrides it for
  // this motor only. Runtime-adjustable via STEPPER_SET_ACCEL, bounded by
  // stepper.max_accel_steps_per_s2 either way.
  double accel_steps_per_s2 = 0.0;
  std::vector<std::size_t> samples;
};

struct HalConfig {
  // The final pinout has no status LEDs. Keep both disabled so their old
  // defaults, BCM 17 and BCM 27, remain available for heater channels.
  bool status_led_enabled = false;
  bool mode_led_enabled = false;
  std::size_t status_led_line = 17;  // heartbeat
  std::size_t mode_led_line = 27;    // system-mode indicator
};

struct OnboardConfig {
  RuntimeConfig runtime;
  ManualControlConfig manual;
  FallbackConfig fallback;
  CommsConfig comms;
  StorageConfig storage;
  PhaseConfig phase;
  TransitionConfig transition;
  PowerConfig power;
  PidConfig pid;
  HardwareConfig hardware;
  SensorHardwareConfig sensors;
  HeaterOutputConfig heaters;
  HeaterSafetyConfig heater_safety;
  SensorRangeConfig sensor_range;
  HalConfig hal;
  StepperConfig stepper;
  PullConfig pull;
  std::array<MotorConfig, 2> motors;

  OnboardConfig();
};

bool LoadConfigFromIni(const std::string& path, OnboardConfig* config, std::string* error);

}  // namespace coatheal
