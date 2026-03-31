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
};

struct TransitionConfig {
  double ascent_to_activation_mbar = 140.0;
  double float_to_descent_mbar = 300.0;
};

struct PowerConfig {
  std::size_t max_active_heaters = 4;
  double max_thermal_w = 40.0;
  double max_system_w = 48.23;
  double heater_nominal_w = 10.0;
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

struct OnboardConfig {
  RuntimeConfig runtime;
  CommsConfig comms;
  StorageConfig storage;
  PhaseConfig phase;
  TransitionConfig transition;
  PowerConfig power;
  PidConfig pid;
  HardwareConfig hardware;
};

bool LoadConfigFromIni(const std::string& path, OnboardConfig* config, std::string* error);

}  // namespace coatheal