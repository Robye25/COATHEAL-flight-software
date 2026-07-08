#include "coatheal/config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

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
  if constexpr (std::is_integral_v<T>) {
    iss >> std::setbase(0) >> *out;
  } else {
    iss >> *out;
  }
  return iss && iss.eof();
}

bool ParseSizeList(const std::string& value, std::vector<std::size_t>* out) {
  out->clear();
  std::istringstream iss(value);
  std::string item;
  while (std::getline(iss, item, ',')) {
    const std::string trimmed = Trim(item);
    if (trimmed.empty()) {
      return false;
    }
    std::size_t parsed = 0;
    if (!ParseNumber(trimmed, &parsed)) {
      return false;
    }
    out->push_back(parsed);
  }
  return true;
}

}  // namespace

OnboardConfig::OnboardConfig() {
  heaters.output_lines = {17, 18, 27, 5, 6, 13};
  stepper.step_line = 19;
  stepper.dir_line = 26;
  stepper.enable_line = 12;

  motors[0].driver = "tmc5160";
  motors[0].spi_device = "/dev/spidev0.0";
  motors[0].cs_line = 22;
  motors[0].step_line = 19;
  motors[0].dir_line = 26;
  motors[0].enable_line = 12;
  motors[0].invert_direction = stepper.invert_direction;
  motors[0].enable_active_low = stepper.enable_active_low;
  motors[0].samples = {0, 1, 2, 3};

  motors[1].driver = "tmc5160";
  motors[1].spi_device = "/dev/spidev0.0";
  motors[1].cs_line = 23;
  motors[1].step_line = 16;
  motors[1].dir_line = 20;
  motors[1].enable_line = 21;
  motors[1].invert_direction = stepper.invert_direction;
  motors[1].enable_active_low = stepper.enable_active_low;
  motors[1].samples = {4, 5, 6, 7};
}

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
    } else if (key == "runtime.use_simulated_sensors") {
      if (!parse_bool(key, value, &config->runtime.use_simulated_sensors, line_no)) return false;
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
    } else if (key == "heater.target_min_c") {
      if (!parse_double(key, value, &config->heater_safety.target_min_c, line_no)) return false;
    } else if (key == "heater.target_max_c") {
      if (!parse_double(key, value, &config->heater_safety.target_max_c, line_no)) return false;

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
    } else if (key == "power.logic_regulator_v") {
      if (!parse_double(key, value, &config->power.logic_regulator_v, line_no)) return false;
    } else if (key == "power.stepper_regulator_v") {
      if (!parse_double(key, value, &config->power.stepper_regulator_v, line_no)) return false;

    } else if (key == "pid.kp") {
      if (!parse_double(key, value, &config->pid.kp, line_no)) return false;
    } else if (key == "pid.ki") {
      if (!parse_double(key, value, &config->pid.ki, line_no)) return false;
    } else if (key == "pid.kd") {
      if (!parse_double(key, value, &config->pid.kd, line_no)) return false;

    } else if (key == "hardware.sample_count") {
      if (!parse_size_t(key, value, &config->hardware.sample_count, line_no)) return false;
    } else if (key == "hardware.heater_count") {
      if (!parse_size_t(key, value, &config->hardware.heater_count, line_no)) return false;
    } else if (key == "hardware.electronics_heater_index") {
      if (!parse_size_t(key, value, &config->hardware.electronics_heater_index, line_no)) return false;

    } else if (key == "sensor.sample_temperature_source") {
      config->sensors.sample_temperature_source = value;
    } else if (key == "sensor.daq132m_device") {
      config->sensors.daq132m_device = value;
    } else if (key == "sensor.daq132m_baud") {
      if (!parse_int(key, value, &config->sensors.daq132m_baud, line_no)) return false;
    } else if (key == "sensor.daq132m_parity") {
      config->sensors.daq132m_parity = value;
    } else if (key == "sensor.daq132m_data_bits") {
      if (!parse_int(key, value, &config->sensors.daq132m_data_bits, line_no)) return false;
    } else if (key == "sensor.daq132m_stop_bits") {
      if (!parse_int(key, value, &config->sensors.daq132m_stop_bits, line_no)) return false;
    } else if (key == "sensor.daq132m_slave_id") {
      if (!parse_int(key, value, &config->sensors.daq132m_slave_id, line_no)) return false;
    } else if (key == "sensor.daq132m_function_code") {
      if (!parse_int(key, value, &config->sensors.daq132m_function_code, line_no)) return false;
    } else if (key == "sensor.daq132m_register_base") {
      if (!parse_int(key, value, &config->sensors.daq132m_register_base, line_no)) return false;
    } else if (key == "sensor.daq132m_register_count") {
      if (!parse_int(key, value, &config->sensors.daq132m_register_count, line_no)) return false;
    } else if (key == "sensor.daq132m_c_per_count") {
      if (!parse_double(key, value, &config->sensors.daq132m_c_per_count, line_no)) return false;
    } else if (key == "sensor.daq132m_c_offset") {
      if (!parse_double(key, value, &config->sensors.daq132m_c_offset, line_no)) return false;
    } else if (key == "sensor.rtd_click_enabled") {
      if (!parse_bool(key, value, &config->sensors.rtd_click_enabled, line_no)) return false;
    } else if (key == "sensor.rtd_click_spi_device") {
      config->sensors.rtd_click_spi_device = value;
    } else if (key == "sensor.rtd_click_cs_line") {
      if (!parse_size_t(key, value, &config->sensors.rtd_click_cs_line, line_no)) return false;
    } else if (key == "sensor.rtd_click_drdy_line") {
      if (!parse_size_t(key, value, &config->sensors.rtd_click_drdy_line, line_no)) return false;
    } else if (key == "sensor.rtd_click_wires") {
      if (!parse_int(key, value, &config->sensors.rtd_click_wires, line_no)) return false;
    } else if (key == "sensor.pressure_source") {
      config->sensors.pressure_source = value;
    } else if (key == "sensor.dps310_i2c_addr") {
      if (!parse_int(key, value, &config->sensors.dps310_i2c_addr, line_no)) return false;
    } else if (key == "sensor.uv_source") {
      config->sensors.uv_source = value;
    } else if (key == "sensor.ads1115_i2c_addr") {
      if (!parse_int(key, value, &config->sensors.ads1115_i2c_addr, line_no)) return false;
    } else if (key == "sensor.uv_ads1115_channel") {
      if (!parse_int(key, value, &config->sensors.uv_ads1115_channel, line_no)) return false;
    } else if (key == "sensor.uv_full_scale_v") {
      if (!parse_double(key, value, &config->sensors.uv_full_scale_v, line_no)) return false;
    } else if (key == "sensor.resistance_source") {
      config->sensors.resistance_source = value;

    } else if (key == "heater.output_lines") {
      if (!ParseSizeList(value, &config->heaters.output_lines)) {
        if (error != nullptr) {
          *error = "invalid GPIO list for heater.output_lines at line " +
                   std::to_string(line_no);
        }
        return false;
      }
    } else if (key == "heater.pwm_frequency_hz") {
      if (!parse_double(key, value, &config->heaters.pwm_frequency_hz, line_no)) return false;
    } else if (key == "heater.active_high") {
      if (!parse_bool(key, value, &config->heaters.active_high, line_no)) return false;

    } else if (key == "hal.status_led_enabled") {
      if (!parse_bool(key, value, &config->hal.status_led_enabled, line_no)) return false;
    } else if (key == "hal.mode_led_enabled") {
      if (!parse_bool(key, value, &config->hal.mode_led_enabled, line_no)) return false;
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

    } else if (key == "pull.max_step_hz") {
      if (!parse_double(key, value, &config->pull.max_step_hz, line_no)) return false;
    } else if (key == "pull.accel_steps_per_s2") {
      if (!parse_double(key, value, &config->pull.accel_steps_per_s2, line_no)) return false;
    } else if (key == "pull.microstep") {
      if (!parse_int(key, value, &config->pull.microstep, line_no)) return false;
    } else if (key == "pull.travel_full_steps") {
      if (!parse_int(key, value, &config->pull.travel_full_steps, line_no)) return false;
    } else if (key == "pull.hold_s") {
      if (!parse_double(key, value, &config->pull.hold_s, line_no)) return false;

    } else if (key.rfind("motor0.", 0) == 0 || key.rfind("motor1.", 0) == 0) {
      const std::size_t motor_index = key[5] == '0' ? 0U : 1U;
      const std::string suffix = key.substr(7);
      MotorConfig& motor = config->motors[motor_index];
      if (suffix == "driver") {
        motor.driver = value;
      } else if (suffix == "spi_device") {
        motor.spi_device = value;
      } else if (suffix == "cs_line") {
        if (!parse_size_t(key, value, &motor.cs_line, line_no)) return false;
      } else if (suffix == "step_line") {
        if (!parse_size_t(key, value, &motor.step_line, line_no)) return false;
      } else if (suffix == "dir_line") {
        if (!parse_size_t(key, value, &motor.dir_line, line_no)) return false;
      } else if (suffix == "enable_line") {
        if (!parse_size_t(key, value, &motor.enable_line, line_no)) return false;
      } else if (suffix == "invert_direction") {
        if (!parse_bool(key, value, &motor.invert_direction, line_no)) return false;
      } else if (suffix == "enable_active_low") {
        if (!parse_bool(key, value, &motor.enable_active_low, line_no)) return false;
      } else if (suffix == "run_current_a_rms") {
        if (!parse_double(key, value, &motor.run_current_a_rms, line_no)) return false;
      } else if (suffix == "hold_current_frac") {
        if (!parse_double(key, value, &motor.hold_current_frac, line_no)) return false;
      } else if (suffix == "stealth_chop") {
        if (!parse_bool(key, value, &motor.stealth_chop, line_no)) return false;
      } else if (suffix == "spi_speed_hz") {
        int speed = 0;
        if (!parse_int(key, value, &speed, line_no)) return false;
        motor.spi_speed_hz = static_cast<std::uint32_t>(speed);
      } else if (suffix == "samples") {
        if (!ParseSizeList(value, &motor.samples)) {
          if (error != nullptr) {
            *error = "invalid sample list for " + key + " at line " +
                     std::to_string(line_no);
          }
          return false;
        }
      } else {
        if (error != nullptr) {
          *error = "unknown motor config key at line " + std::to_string(line_no) +
                   ": " + key;
        }
        return false;
      }

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
    } else {
      // Reject unknown keys so final-BOM configuration drift fails at startup.
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

  if (config->hardware.sample_count == 0U) {
    if (error != nullptr) {
      *error = "hardware.sample_count must be > 0";
    }
    return false;
  }

  if (config->hardware.heater_count > config->hardware.sample_count) {
    if (error != nullptr) {
      *error = "hardware.heater_count must be <= hardware.sample_count";
    }
    return false;
  }

  if (!config->heaters.output_lines.empty() &&
      config->heaters.output_lines.size() != config->hardware.heater_count) {
    if (error != nullptr) {
      *error = "heater.output_lines count must equal hardware.heater_count";
    }
    return false;
  }

  if (config->heaters.pwm_frequency_hz <= 0.0) {
    if (error != nullptr) {
      *error = "heater.pwm_frequency_hz must be > 0";
    }
    return false;
  }

  if (config->heater_safety.target_min_c >= config->heater_safety.target_max_c ||
      config->heater_safety.target_max_c >= config->heater_safety.max_sample_temp_c) {
    if (error != nullptr) {
      *error = "heater target limits must satisfy min < max < max_sample_temp_c";
    }
    return false;
  }

  // SIZE_MAX is the sentinel for "no electronics-box heater". Any other value
  // must still be a valid channel index.
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

  if (config->sensors.daq132m_baud <= 0 ||
      config->sensors.daq132m_data_bits <= 0 ||
      config->sensors.daq132m_stop_bits <= 0 ||
      config->sensors.daq132m_slave_id <= 0 ||
      (config->sensors.daq132m_function_code != 3 &&
       config->sensors.daq132m_function_code != 4) ||
      config->sensors.daq132m_register_count <
          static_cast<int>(config->hardware.sample_count) ||
      config->sensors.daq132m_c_per_count == 0.0) {
    if (error != nullptr) {
      *error = "invalid DAQ132M Modbus configuration";
    }
    return false;
  }

  if (config->sensors.sample_temperature_source != "daq132m_modbus" ||
      config->sensors.pressure_source != "dps310" ||
      config->sensors.uv_source != "guva_s12sd_ads1115") {
    if (error != nullptr) {
      *error = "sensor sources must match the Rev C final BOM";
    }
    return false;
  }

  if (config->sensors.resistance_source != "disabled" &&
      config->sensors.resistance_source != "simulated") {
    if (error != nullptr) {
      *error = "sensor.resistance_source must be disabled or simulated";
    }
    return false;
  }

  if (config->sensors.dps310_i2c_addr < 0 ||
      config->sensors.dps310_i2c_addr > 0x7F ||
      config->sensors.ads1115_i2c_addr < 0 ||
      config->sensors.ads1115_i2c_addr > 0x7F ||
      config->sensors.uv_ads1115_channel < 0 ||
      config->sensors.uv_ads1115_channel > 3 ||
      config->sensors.uv_full_scale_v <= 0.0) {
    if (error != nullptr) {
      *error = "invalid I2C sensor configuration";
    }
    return false;
  }

  if (config->sensors.rtd_click_wires < 2 || config->sensors.rtd_click_wires > 4) {
    if (error != nullptr) {
      *error = "sensor.rtd_click_wires must be 2, 3, or 4";
    }
    return false;
  }

  if (config->stepper.steps_per_rev <= 0 || config->stepper.microstep <= 0 ||
      config->stepper.default_step_hz <= 0.0 || config->stepper.max_step_hz <= 0.0 ||
      config->stepper.max_position_steps <= 0) {
    if (error != nullptr) {
      *error = "invalid stepper configuration";
    }
    return false;
  }

  if (config->pull.max_step_hz <= 0.0 || config->pull.accel_steps_per_s2 <= 0.0 ||
      config->pull.microstep <= 0 || config->pull.travel_full_steps <= 0 ||
      config->pull.hold_s < 0.0) {
    if (error != nullptr) {
      *error = "invalid pull configuration";
    }
    return false;
  }

  for (std::size_t i = 0; i < config->motors.size(); ++i) {
    const MotorConfig& motor = config->motors[i];
    if (motor.driver != "tmc5160") {
      if (error != nullptr) {
        *error = "motor" + std::to_string(i) + ".driver must be tmc5160";
      }
      return false;
    }
    if (motor.spi_device.empty() || motor.run_current_a_rms <= 0.0 ||
        motor.hold_current_frac < 0.0 || motor.hold_current_frac > 1.0 ||
        motor.spi_speed_hz == 0U || motor.samples.empty()) {
      if (error != nullptr) {
        *error = "invalid motor" + std::to_string(i) + " configuration";
      }
      return false;
    }
    for (const std::size_t sample : motor.samples) {
      if (sample >= config->hardware.sample_count) {
        if (error != nullptr) {
          *error = "motor" + std::to_string(i) + ".samples contains out-of-range sample";
        }
        return false;
      }
    }
  }

  std::map<std::size_t, std::string> gpio_owners;
  auto claim_gpio = [&](std::size_t line, const std::string& owner) -> bool {
    const auto [it, inserted] = gpio_owners.emplace(line, owner);
    if (!inserted) {
      if (error != nullptr) {
        *error = "BCM GPIO " + std::to_string(line) + " assigned to both " +
                 it->second + " and " + owner;
      }
      return false;
    }
    return true;
  };

  for (std::size_t i = 0; i < config->heaters.output_lines.size(); ++i) {
    if (!claim_gpio(config->heaters.output_lines[i],
                    "heater.output_lines[" + std::to_string(i) + "]")) {
      return false;
    }
  }
  for (std::size_t i = 0; i < config->motors.size(); ++i) {
    const std::string prefix = "motor" + std::to_string(i);
    if (!claim_gpio(config->motors[i].cs_line, prefix + ".cs_line") ||
        !claim_gpio(config->motors[i].step_line, prefix + ".step_line") ||
        !claim_gpio(config->motors[i].dir_line, prefix + ".dir_line") ||
        !claim_gpio(config->motors[i].enable_line, prefix + ".enable_line")) {
      return false;
    }
  }
  if (config->sensors.rtd_click_enabled &&
      (!claim_gpio(config->sensors.rtd_click_cs_line,
                   "sensor.rtd_click_cs_line") ||
       !claim_gpio(config->sensors.rtd_click_drdy_line,
                   "sensor.rtd_click_drdy_line"))) {
    return false;
  }
  if (config->hal.status_led_enabled &&
      !claim_gpio(config->hal.status_led_line, "hal.status_led_line")) {
    return false;
  }
  if (config->hal.mode_led_enabled &&
      !claim_gpio(config->hal.mode_led_line, "hal.mode_led_line")) {
    return false;
  }

  return true;
}

}  // namespace coatheal
