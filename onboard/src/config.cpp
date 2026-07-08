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

  auto parse_i64 = [&](const std::string& key,
                       const std::string& value,
                       std::int64_t* out,
                       int line_no) -> bool {
    if (ParseNumber(value, out)) {
      return true;
    }
    if (error != nullptr) {
      *error = "invalid int64 value for " + key + " at line " + std::to_string(line_no);
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

    } else if (key == "manual.manual_first") {
      if (!parse_bool(key, value, &config->manual.manual_first, line_no)) return false;
    } else if (key == "manual.link_loss_fallback_enabled") {
      if (!parse_bool(key, value, &config->manual.link_loss_fallback_enabled, line_no)) return false;
    } else if (key == "manual.link_loss_fallback_s") {
      if (!parse_double(key, value, &config->manual.link_loss_fallback_s, line_no)) return false;

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
    } else if (key == "comms.discovery_period_ms") {
      if (!parse_int(key, value, &config->comms.discovery_period_ms, line_no)) return false;
    } else if (key == "comms.rediscover_period_s") {
      if (!parse_int(key, value, &config->comms.rediscover_period_s, line_no)) return false;
    } else if (key == "comms.failover_grace_s") {
      if (!parse_int(key, value, &config->comms.failover_grace_s, line_no)) return false;
    } else if (key == "comms.priority") {
      if (!parse_int(key, value, &config->comms.priority, line_no)) return false;

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

    } else if (key == "phase.sample_floor_c") {
      if (!parse_double(key, value, &config->phase.sample_floor_c, line_no)) return false;
    } else if (key == "phase.uniformity_tolerance_c") {
      if (!parse_double(key, value, &config->phase.uniformity_tolerance_c, line_no)) return false;

    } else if (key == "heater.max_sample_temp_c") {
      if (!parse_double(key, value, &config->heater_safety.max_sample_temp_c, line_no)) return false;

    } else if (key == "sensor.ambient_temp_min_c") {
      if (!parse_double(key, value, &config->sensor_range.ambient_temp_min_c, line_no)) return false;
    } else if (key == "sensor.ambient_temp_max_c") {
      if (!parse_double(key, value, &config->sensor_range.ambient_temp_max_c, line_no)) return false;
    } else if (key == "sensor.ambient_pressure_min_mbar") {
      if (!parse_double(key, value, &config->sensor_range.ambient_pressure_min_mbar, line_no)) return false;
    } else if (key == "sensor.ambient_pressure_max_mbar") {
      if (!parse_double(key, value, &config->sensor_range.ambient_pressure_max_mbar, line_no)) return false;

    } else if (key == "transition.pre_float_mbar") {
      if (!parse_double(key, value, &config->transition.pre_float_mbar, line_no)) return false;
    } else if (key == "transition.ascent_to_float_mbar") {
      if (!parse_double(key, value, &config->transition.ascent_to_float_mbar, line_no)) return false;
    } else if (key == "transition.float_to_descent_mbar") {
      if (!parse_double(key, value, &config->transition.float_to_descent_mbar, line_no)) return false;
    } else if (key == "transition.descent_to_landed_mbar") {
      if (!parse_double(key, value, &config->transition.descent_to_landed_mbar, line_no)) return false;
    } else if (key == "transition.debounce_samples") {
      if (!parse_int(key, value, &config->transition.debounce_samples, line_no)) return false;

    } else if (key == "fatigue.fatigue_cycles") {
      if (!parse_int(key, value, &config->fatigue.fatigue_cycles, line_no)) return false;
    } else if (key == "fatigue.fatigue_travel_full_steps") {
      if (!parse_int(key, value, &config->fatigue.fatigue_travel_full_steps, line_no)) return false;
    } else if (key == "fatigue.fatigue_pull_hold_s") {
      if (!parse_double(key, value, &config->fatigue.fatigue_pull_hold_s, line_no)) return false;
    } else if (key == "fatigue.soak_hold_s") {
      if (!parse_double(key, value, &config->fatigue.soak_hold_s, line_no)) return false;
    } else if (key == "fatigue.soak_travel_full_steps") {
      if (!parse_int(key, value, &config->fatigue.soak_travel_full_steps, line_no)) return false;

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

    } else if (key == "hardware.heater_count") {
      if (!parse_size_t(key, value, &config->hardware.heater_count, line_no)) return false;
    } else if (key == "hardware.electronics_heater_index") {
      if (!parse_size_t(key, value, &config->hardware.electronics_heater_index, line_no)) return false;

    } else if (key == "hal.status_led_line") {
      if (!parse_size_t(key, value, &config->hal.status_led_line, line_no)) return false;
    } else if (key == "hal.mode_led_line") {
      if (!parse_size_t(key, value, &config->hal.mode_led_line, line_no)) return false;

    } else if (key == "stepper.steps_per_rev") {
      if (!parse_int(key, value, &config->stepper.steps_per_rev, line_no)) return false;
    } else if (key == "stepper.microstep") {
      if (!parse_int(key, value, &config->stepper.microstep, line_no)) return false;
    } else if (key == "stepper.default_step_hz") {
      if (!parse_double(key, value, &config->stepper.default_step_hz, line_no)) return false;
    } else if (key == "stepper.max_step_hz") {
      if (!parse_double(key, value, &config->stepper.max_step_hz, line_no)) return false;
    } else if (key == "stepper.max_position_steps") {
      if (!parse_i64(key, value, &config->stepper.max_position_steps, line_no)) return false;
    } else if (key == "stepper.step_line") {
      if (!parse_size_t(key, value, &config->stepper.step_line, line_no)) return false;
    } else if (key == "stepper.dir_line") {
      if (!parse_size_t(key, value, &config->stepper.dir_line, line_no)) return false;
    } else if (key == "stepper.enable_line") {
      if (!parse_size_t(key, value, &config->stepper.enable_line, line_no)) return false;
    } else if (key == "stepper.invert_direction") {
      if (!parse_bool(key, value, &config->stepper.invert_direction, line_no)) return false;
    } else if (key == "stepper.enable_active_low") {
      if (!parse_bool(key, value, &config->stepper.enable_active_low, line_no)) return false;
    } else if (key == "stepper.enable_on_boot") {
      if (!parse_bool(key, value, &config->stepper.enable_on_boot, line_no)) return false;

    } else if (key == "bend.ascent_steps") {
      if (!parse_i64(key, value, &config->bend.ascent_steps, line_no)) return false;
    } else if (key == "bend.ascent_hold_s") {
      if (!parse_double(key, value, &config->bend.ascent_hold_s, line_no)) return false;
    } else if (key == "bend.activation_steps") {
      if (!parse_i64(key, value, &config->bend.activation_steps, line_no)) return false;
    } else if (key == "bend.activation_hold_s") {
      if (!parse_double(key, value, &config->bend.activation_hold_s, line_no)) return false;
    } else if (key == "bend.float_steps") {
      if (!parse_i64(key, value, &config->bend.float_steps, line_no)) return false;
    } else if (key == "bend.float_hold_s") {
      if (!parse_double(key, value, &config->bend.float_hold_s, line_no)) return false;
    } else if (key == "bend.descent_steps") {
      if (!parse_i64(key, value, &config->bend.descent_steps, line_no)) return false;
    } else if (key == "bend.descent_hold_s") {
      if (!parse_double(key, value, &config->bend.descent_hold_s, line_no)) return false;
    } else if (key.rfind("pull.", 0) == 0 ||
               key.rfind("motor0.", 0) == 0 ||
               key.rfind("motor1.", 0) == 0) {
      // Rev B dual-stepper / pull-envelope keys. Accepted but not yet wired
      // into StepperChannelConfig — system_controller.cpp uses hard-coded
      // Rev B defaults for now. Parsing will be added when the multi-channel
      // config path is fully plumbed.
      (void)value;
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

  if (config->manual.link_loss_fallback_s < 0.0) {
    if (error != nullptr) {
      *error = "manual.link_loss_fallback_s must be >= 0";
    }
    return false;
  }

  if (config->hardware.heater_count == 0U) {
    if (error != nullptr) {
      *error = "hardware.heater_count must be > 0";
    }
    return false;
  }

  // Rev B.1: SIZE_MAX is the sentinel for "no electronics-box heater". Any
  // other value must still be a valid channel index.
  if (config->hardware.electronics_heater_index != static_cast<std::size_t>(-1) &&
      config->hardware.electronics_heater_index >= config->hardware.heater_count) {
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

  if (config->comms.priority < 0 || config->comms.priority > 999) {
    if (error != nullptr) {
      *error = "comms.priority must be in [0, 999]";
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
