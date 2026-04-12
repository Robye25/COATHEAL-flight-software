#include "coatheal/config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

namespace coatheal {
namespace {

std::string Trim(const std::string& in) {
  const auto begin = std::find_if_not(in.begin(), in.end(), [](unsigned char c) {
    return std::isspace(c) != 0;
  });
  const auto end = std::find_if_not(in.rbegin(), in.rend(), [](unsigned char c) {
    return std::isspace(c) != 0;
  }).base();
  if (begin >= end) {
    return {};
  }
  return std::string(begin, end);
}

bool ParseBool(const std::string& value, bool* out) {
  const std::string v = Trim(value);
  if (v == "1" || v == "true" || v == "TRUE" || v == "on" || v == "ON") {
    *out = true;
    return true;
  }
  if (v == "0" || v == "false" || v == "FALSE" || v == "off" || v == "OFF") {
    *out = false;
    return true;
  }
  return false;
}

template <typename T>
bool ParseNumber(const std::string& value, T* out) {
  std::istringstream iss(Trim(value));
  iss >> *out;
  return iss && iss.eof();
}

}  // namespace

bool LoadConfigFromIni(const std::string& path, OnboardConfig* config, std::string* error) {
  if (config == nullptr) {
    if (error != nullptr) {
      *error = "null config pointer";
    }
    return false;
  }

  std::ifstream in(path);
  if (!in.is_open()) {
    if (error != nullptr) {
      *error = "unable to open config file: " + path;
    }
    return false;
  }

  auto parse_bool = [&](const std::string& key,
                        const std::string& value,
                        bool* out,
                        int line_no) -> bool {
    if (ParseBool(value, out)) {
      return true;
    }
    if (error != nullptr) {
      *error = "invalid bool value for " + key + " at line " + std::to_string(line_no);
    }
    return false;
  };

  auto parse_int = [&](const std::string& key,
                       const std::string& value,
                       int* out,
                       int line_no) -> bool {
    if (ParseNumber(value, out)) {
      return true;
    }
    if (error != nullptr) {
      *error = "invalid integer value for " + key + " at line " + std::to_string(line_no);
    }
    return false;
  };

  auto parse_u64 = [&](const std::string& key,
                       const std::string& value,
                       std::uint64_t* out,
                       int line_no) -> bool {
    if (ParseNumber(value, out)) {
      return true;
    }
    if (error != nullptr) {
      *error = "invalid uint64 value for " + key + " at line " + std::to_string(line_no);
    }
    return false;
  };

  auto parse_double = [&](const std::string& key,
                          const std::string& value,
                          double* out,
                          int line_no) -> bool {
    if (ParseNumber(value, out)) {
      return true;
    }
    if (error != nullptr) {
      *error = "invalid numeric value for " + key + " at line " + std::to_string(line_no);
    }
    return false;
  };

  auto parse_size_t = [&](const std::string& key,
                          const std::string& value,
                          std::size_t* out,
                          int line_no) -> bool {
    if (ParseNumber(value, out)) {
      return true;
    }
    if (error != nullptr) {
      *error = "invalid size_t value for " + key + " at line " + std::to_string(line_no);
    }
    return false;
  };

  std::string line;
  int line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
      continue;
    }

    const std::size_t eq = trimmed.find('=');
    if (eq == std::string::npos) {
      if (error != nullptr) {
        *error = "invalid config line " + std::to_string(line_no) + ": missing '='";
      }
      return false;
    }

    const std::string key = Trim(trimmed.substr(0, eq));
    const std::string value = Trim(trimmed.substr(eq + 1));

    if (key == "runtime.tick_hz") {
      if (!parse_double(key, value, &config->runtime.tick_hz, line_no)) return false;
    } else if (key == "runtime.bench_mode") {
      if (!parse_bool(key, value, &config->runtime.bench_mode, line_no)) return false;
    } else if (key == "runtime.debug_arm_code") {
      config->runtime.debug_arm_code = value;
    } else if (key == "runtime.use_simulated_pwm") {
      if (!parse_bool(key, value, &config->runtime.use_simulated_pwm, line_no)) return false;
    } else if (key == "runtime.gpio_chip") {
      config->runtime.gpio_chip = value;

    } else if (key == "comms.telemetry_host") {
      config->comms.telemetry_host = value;
    } else if (key == "comms.static_ground_ip") {
      config->comms.static_ground_ip = value;
    } else if (key == "comms.static_pi_ip") {
      config->comms.static_pi_ip = value;
    } else if (key == "comms.telemetry_port") {
      if (!parse_int(key, value, &config->comms.telemetry_port, line_no)) return false;
    } else if (key == "comms.command_port") {
      if (!parse_int(key, value, &config->comms.command_port, line_no)) return false;
    } else if (key == "comms.reconnect_ms") {
      if (!parse_int(key, value, &config->comms.reconnect_ms, line_no)) return false;
    } else if (key == "comms.discovery_enabled") {
      if (!parse_bool(key, value, &config->comms.discovery_enabled, line_no)) return false;
    } else if (key == "comms.discovery_port") {
      if (!parse_int(key, value, &config->comms.discovery_port, line_no)) return false;

    } else if (key == "storage.primary_log_path") {
      config->storage.primary_log_path = value;
    } else if (key == "storage.secondary_log_path") {
      config->storage.secondary_log_path = value;
    } else if (key == "storage.queue_dir") {
      config->storage.queue_dir = value;
    } else if (key == "storage.queue_retention_hours") {
      if (!parse_double(key, value, &config->storage.queue_retention_hours, line_no)) return false;
    } else if (key == "storage.queue_max_bytes") {
      if (!parse_u64(key, value, &config->storage.queue_max_bytes, line_no)) return false;

    } else if (key == "phase.ascent_target_c") {
      if (!parse_double(key, value, &config->phase.ascent_target_c, line_no)) return false;
    } else if (key == "phase.activation_target_c") {
      if (!parse_double(key, value, &config->phase.activation_target_c, line_no)) return false;
    } else if (key == "phase.float_target_c") {
      if (!parse_double(key, value, &config->phase.float_target_c, line_no)) return false;
    } else if (key == "phase.descent_floor_c") {
      if (!parse_double(key, value, &config->phase.descent_floor_c, line_no)) return false;
    } else if (key == "phase.box_target_c") {
      if (!parse_double(key, value, &config->phase.box_target_c, line_no)) return false;
    } else if (key == "phase.activation_ramp_c_per_s") {
      if (!parse_double(key, value, &config->phase.activation_ramp_c_per_s, line_no)) return false;
    } else if (key == "phase.float_hold_minutes") {
      if (!parse_double(key, value, &config->phase.float_hold_minutes, line_no)) return false;
    } else if (key == "phase.uniformity_tolerance_c") {
      if (!parse_double(key, value, &config->phase.uniformity_tolerance_c, line_no)) return false;

    } else if (key == "heater.max_sample_temp_c") {
      if (!parse_double(key, value, &config->heater_safety.max_sample_temp_c, line_no)) return false;
    } else if (key == "heater.max_box_temp_c") {
      if (!parse_double(key, value, &config->heater_safety.max_box_temp_c, line_no)) return false;

    } else if (key == "sensor.ambient_temp_min_c") {
      if (!parse_double(key, value, &config->sensor_range.ambient_temp_min_c, line_no)) return false;
    } else if (key == "sensor.ambient_temp_max_c") {
      if (!parse_double(key, value, &config->sensor_range.ambient_temp_max_c, line_no)) return false;
    } else if (key == "sensor.ambient_pressure_min_mbar") {
      if (!parse_double(key, value, &config->sensor_range.ambient_pressure_min_mbar, line_no)) return false;
    } else if (key == "sensor.ambient_pressure_max_mbar") {
      if (!parse_double(key, value, &config->sensor_range.ambient_pressure_max_mbar, line_no)) return false;

    } else if (key == "transition.ascent_to_activation_mbar") {
      if (!parse_double(key, value, &config->transition.ascent_to_activation_mbar, line_no)) return false;
    } else if (key == "transition.float_to_descent_mbar") {
      if (!parse_double(key, value, &config->transition.float_to_descent_mbar, line_no)) return false;

    } else if (key == "power.max_active_heaters") {
      if (!parse_size_t(key, value, &config->power.max_active_heaters, line_no)) return false;
    } else if (key == "power.max_thermal_w") {
      if (!parse_double(key, value, &config->power.max_thermal_w, line_no)) return false;
    } else if (key == "power.max_system_w") {
      if (!parse_double(key, value, &config->power.max_system_w, line_no)) return false;
    } else if (key == "power.heater_nominal_w") {
      if (!parse_double(key, value, &config->power.heater_nominal_w, line_no)) return false;
    } else if (key == "power.energy_budget_wh") {
      if (!parse_double(key, value, &config->power.energy_budget_wh, line_no)) return false;

    } else if (key == "pid.kp") {
      if (!parse_double(key, value, &config->pid.kp, line_no)) return false;
    } else if (key == "pid.ki") {
      if (!parse_double(key, value, &config->pid.ki, line_no)) return false;
    } else if (key == "pid.kd") {
      if (!parse_double(key, value, &config->pid.kd, line_no)) return false;
    } else if (key == "pid.box_kp") {
      if (!parse_double(key, value, &config->pid.box_kp, line_no)) return false;
    } else if (key == "pid.box_ki") {
      if (!parse_double(key, value, &config->pid.box_ki, line_no)) return false;
    } else if (key == "pid.box_kd") {
      if (!parse_double(key, value, &config->pid.box_kd, line_no)) return false;

    } else if (key == "hardware.heater_count") {
      if (!parse_size_t(key, value, &config->hardware.heater_count, line_no)) return false;
    } else if (key == "hardware.electronics_heater_index") {
      if (!parse_size_t(key, value, &config->hardware.electronics_heater_index, line_no)) return false;

    } else if (key == "hal.status_led_line") {
      if (!parse_size_t(key, value, &config->hal.status_led_line, line_no)) return false;
    } else if (key == "hal.mode_led_line") {
      if (!parse_size_t(key, value, &config->hal.mode_led_line, line_no)) return false;
    } else {
      if (error != nullptr) {
        *error = "unknown config key at line " + std::to_string(line_no) + ": " + key;
      }
      return false;
    }
  }

  if (config->runtime.tick_hz <= 0.0) {
    if (error != nullptr) {
      *error = "runtime.tick_hz must be > 0";
    }
    return false;
  }

  if (config->hardware.heater_count == 0U) {
    if (error != nullptr) {
      *error = "hardware.heater_count must be > 0";
    }
    return false;
  }

  if (config->hardware.electronics_heater_index >= config->hardware.heater_count) {
    if (error != nullptr) {
      *error = "electronics heater index out of range";
    }
    return false;
  }

  if (config->comms.telemetry_port <= 0 || config->comms.command_port <= 0 ||
      config->comms.discovery_port <= 0) {
    if (error != nullptr) {
      *error = "all comms ports must be > 0";
    }
    return false;
  }

  if (config->storage.queue_retention_hours <= 0.0) {
    if (error != nullptr) {
      *error = "storage.queue_retention_hours must be > 0";
    }
    return false;
  }

  if (config->storage.queue_max_bytes == 0U) {
    if (error != nullptr) {
      *error = "storage.queue_max_bytes must be > 0";
    }
    return false;
  }

  return true;
}

}  // namespace coatheal
