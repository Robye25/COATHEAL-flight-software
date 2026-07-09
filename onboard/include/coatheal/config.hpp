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
  // Final BOM: 5 W polyimide film heaters; at most 4 energized at once,
  // yielding the default 20 W combined thermal draw ceiling.
  std::size_t max_active_heaters = 4;
  double max_thermal_w = 20.0;
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
  bool daq132m_enabled = true;
  bool dps310_auto_discover = true;
  bool ads1115_auto_discover = true;
  bool daq132m_auto_discover = true;
  int dps310_poll_ms = 1000;
  int ads1115_poll_ms = 1000;
  int daq132m_poll_ms = 1000;
  int stale_after_ms = 3000;
  std::string sample_temperature_source = "daq132m_modbus";
  std::string daq132m_device = "/dev/ttyUSB0";
  int daq132m_baud = 9600;
  std::string daq132m_parity = "N";
  int daq132m_data_bits = 8;
  int daq132m_stop_bits = 1;
  int daq132m_slave_id = 1;
  int daq132m_function_code = 3;
  int daq132m_register_base = 0;
  int daq132m_register_count = 8;
  double daq132m_c_per_count = 0.1;
  double daq132m_c_offset = 0.0;
  std::vector<std::size_t> daq132m_enabled_channels;

  bool rtd_click_enabled = false;
  std::string rtd_click_spi_device = "/dev/spidev0.0";
  std::size_t rtd_click_cs_line = 7;
  std::size_t rtd_click_drdy_line = 25;
  int rtd_click_wires = 3;

  std::string pressure_source = "dps310";
  int dps310_i2c_addr = 0x77;

  std::string uv_source = "guva_s12sd_ads1115";
  int ads1115_i2c_addr = 0x48;
  int uv_ads1115_channel = 0;
  double uv_full_scale_v = 4.096;

  std::string resistance_source = "disabled";
};

struct HeaterOutputConfig {
  std::vector<std::size_t> output_lines;
  std::vector<std::size_t> temperature_channels;
  double pwm_frequency_hz = 10.0;
  bool active_high = true;
};

struct StepperConfig {
  // Shared motion defaults. Per-motor TMC5160 SPI and GPIO wiring live in
  // MotorConfig and are loaded from motor0.* / motor1.* keys.
  int steps_per_rev = 200;           // NEMA 17 full-step default
  int microstep = 4;
  double default_step_hz = 100.0;
  double max_step_hz = 100.0;
  std::int64_t max_position_steps = 200000;  // absolute travel limit
  std::size_t step_line = 19;        // Motor 0 BCM lines; see docs/hardware.md
  std::size_t dir_line = 26;
  std::size_t enable_line = 12;
  bool invert_direction = false;
  bool enable_active_low = true;     // most step/dir drivers have active-low /EN
  bool enable_on_boot = false;       // stay de-energised until commanded
};

struct PullConfig {
  double max_step_hz = 100.0;
  double accel_steps_per_s2 = 200.0;
  int microstep = 4;
  int travel_full_steps = 200;
  double hold_s = 5.0;
};

struct MotorConfig {
  std::string driver = "tmc5160";
  std::string spi_device = "/dev/spidev0.0";
  std::size_t cs_line = 22;
  std::size_t step_line = 19;
  std::size_t dir_line = 26;
  std::size_t enable_line = 12;
  bool invert_direction = false;
  bool enable_active_low = true;
  double run_current_a_rms = 0.8;
  double sense_resistor_ohm = 0.075;
  double hold_current_frac = 0.30;
  bool stealth_chop = true;
  std::uint32_t spi_speed_hz = 1000000;
  int pulse_high_us = 3;
  int retry_ms = 2000;
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
