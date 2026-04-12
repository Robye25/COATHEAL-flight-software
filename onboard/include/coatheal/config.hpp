#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace coatheal {

struct RuntimeConfig {
  double tick_hz = 1.0;
  bool bench_mode = false;
  std::string debug_arm_code = "COATHEAL_DEBUG";
  bool use_simulated_pwm = false;
  std::string gpio_chip = "/dev/gpiochip0";
};

struct CommsConfig {
  std::string telemetry_host = "127.0.0.1";
  std::string static_ground_ip = "192.168.50.1";
  std::string static_pi_ip = "192.168.50.2";
  int telemetry_port = 4000;
  int command_port = 5000;
  int reconnect_ms = 2000;
  bool discovery_enabled = true;
  int discovery_port = 4100;
};

struct StorageConfig {
  std::string primary_log_path = "logs/onboard_primary.csv";
  std::string secondary_log_path = "logs/onboard_usb_mirror.csv";
  std::string queue_dir = "logs/telemetry-queue";
  double queue_retention_hours = 72.0;
  std::uint64_t queue_max_bytes = 8589934592ULL;  // 8 GiB
};

struct PhaseConfig {
  double ascent_target_c = -30.0;
  double activation_target_c = 70.0;
  double float_target_c = 70.0;
  double descent_floor_c = -20.0;
  double box_target_c = 0.0;
  double activation_ramp_c_per_s = 0.85;
  double float_hold_minutes = 90.0;
  double uniformity_tolerance_c = 2.0;
};

struct TransitionConfig {
  double ascent_to_activation_mbar = 100.0;
  double float_to_descent_mbar = 300.0;
};

struct HeaterSafetyConfig {
  double max_sample_temp_c = 85.0;
  double max_box_temp_c = 60.0;
};

struct SensorRangeConfig {
  double ambient_temp_min_c = -90.0;
  double ambient_temp_max_c = 50.0;
  double ambient_pressure_min_mbar = 5.0;
  double ambient_pressure_max_mbar = 1050.0;
};

struct PowerConfig {
  std::size_t max_active_heaters = 4;
  double max_thermal_w = 40.0;
  double max_system_w = 48.23;
  double heater_nominal_w = 10.0;
  // BEXUS User Manual §5.2: each team is allocated 150 Wh for the full flight.
  // Pi 4 + sensors consume ~5–10 W continuously, so the heater share is lower.
  // 0 disables enforcement (back-compat).
  double energy_budget_wh = 0.0;
};

struct PidConfig {
  double kp = 0.20;
  double ki = 0.02;
  double kd = 0.03;
  double box_kp = 0.15;
  double box_ki = 0.01;
  double box_kd = 0.02;
};

struct HardwareConfig {
  std::size_t heater_count = 10;
  std::size_t electronics_heater_index = 9;
};

struct StepperConfig {
  // Driver is unspecified at SED v2.0. These defaults match a generic STEP/
  // DIR/EN driver (A4988, DRV8825, TMC2209 legacy mode) and are fully
  // overridable from onboard.ini once the hardware is chosen.
  int steps_per_rev = 200;           // NEMA 17 full-step default
  int microstep = 16;                // common default for bending actuator
  double default_step_hz = 400.0;    // pulse rate used by Tick()
  double max_step_hz = 4000.0;       // hard ceiling (SetSpeed clamp)
  std::int64_t max_position_steps = 200000;  // absolute travel limit
  std::size_t step_line = 5;         // GPIO lines (BCM) — see docs/hardware.md
  std::size_t dir_line = 6;
  std::size_t enable_line = 13;
  bool invert_direction = false;
  bool enable_active_low = true;     // most step/dir drivers have active-low /EN
  bool enable_on_boot = false;       // stay de-energised until commanded
};

struct BendScheduleConfig {
  // Per mission-phase bend setpoint + hold duration. Applied on phase entry.
  // Units: steps (signed, driver-side; sign governed by stepper.invert_direction).
  std::int64_t ascent_steps = 0;
  double ascent_hold_s = 0.0;
  std::int64_t activation_steps = 0;
  double activation_hold_s = 0.0;
  std::int64_t float_steps = 0;
  double float_hold_s = 0.0;
  std::int64_t descent_steps = 0;
  double descent_hold_s = 0.0;
};

struct HalConfig {
  // GPIO line numbers for the two visual status LEDs on the Pi 40-pin header.
  // Defaults (BCM 17 / BCM 27) match the wiring documented in docs/hardware.md.
  std::size_t status_led_line = 17;  // heartbeat
  std::size_t mode_led_line = 27;    // system-mode indicator
};

struct OnboardConfig {
  RuntimeConfig runtime;
  CommsConfig comms;
  StorageConfig storage;
  PhaseConfig phase;
  TransitionConfig transition;
  PowerConfig power;
  PidConfig pid;
  HardwareConfig hardware;
  HeaterSafetyConfig heater_safety;
  SensorRangeConfig sensor_range;
  HalConfig hal;
  StepperConfig stepper;
  BendScheduleConfig bend;
};

bool LoadConfigFromIni(const std::string& path, OnboardConfig* config, std::string* error);

}  // namespace coatheal