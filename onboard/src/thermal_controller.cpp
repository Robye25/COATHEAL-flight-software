#include "coatheal/thermal_controller.hpp"

#include <algorithm>

#include "coatheal/telemetry.hpp"  // full definition of SensorSnapshot

namespace coatheal {

ThermalController::ThermalController(const OnboardConfig& config)
    : config_(config),
      channel_latched_(config.hardware.heater_count, false),
      sample_heating_(config.hardware.heater_count, false),
      active_targets_c_(config.hardware.heater_count) {
  sample_pids_.reserve(config_.hardware.heater_count);
  for (std::size_t i = 0; i < config_.hardware.heater_count; ++i) {
    sample_pids_.emplace_back(PidGains{config.pid.kp, config.pid.ki, config.pid.kd},
                              0.0, 1.0, -10.0, 10.0);
  }
}

void ThermalController::Reset() {
  for (auto& pid : sample_pids_) {
    pid.Reset();
  }
  std::fill(channel_latched_.begin(), channel_latched_.end(), false);
  std::fill(sample_heating_.begin(), sample_heating_.end(), false);
  std::fill(active_targets_c_.begin(), active_targets_c_.end(), std::nullopt);
  overtemp_latched_ = false;
  uniformity_ok_ = true;
}

void ThermalController::UpdatePid(PidGains gains) {
  for (auto& pid : sample_pids_) {
    pid.SetGains(gains);
  }
}

void ThermalController::UpdatePid(std::size_t channel, PidGains gains) {
  if (channel < sample_pids_.size()) {
    sample_pids_[channel].SetGains(gains);
  }
}

bool ThermalController::ShouldHeatPhase(MissionPhase phase) const {
  switch (phase) {
    case MissionPhase::kAscent:
    case MissionPhase::kPreFloat:
    case MissionPhase::kFloat:
    case MissionPhase::kDescent:
      return true;
    case MissionPhase::kBoot:
    case MissionPhase::kLanded:
    case MissionPhase::kStopped:
      return false;
  }
  return false;
}

std::vector<double> ThermalController::ComputeRequestedDuty(
    MissionPhase phase,
    const SensorSnapshot& sensors,
    double dt_seconds,
    const ControlOverrides& overrides) {
  const std::size_t heater_count = config_.hardware.heater_count;
  if (channel_latched_.size() != heater_count) {
    channel_latched_.assign(heater_count, false);
  }
  if (sample_heating_.size() != heater_count) {
    sample_heating_.assign(heater_count, false);
  }
  if (active_targets_c_.size() != heater_count) {
    active_targets_c_.assign(heater_count, std::nullopt);
  }
  std::vector<double> duty(heater_count, 0.0);
  const auto sensor_channel = [&](std::size_t heater) {
    return heater < config_.heaters.temperature_channels.size()
               ? config_.heaters.temperature_channels[heater]
               : heater;
  };
  const auto temp_valid_for = [&](std::size_t heater) {
    const std::size_t sample = sensor_channel(heater);
    return sample < sensors.sample_temps_c.size() &&
           (sensors.sample_temp_valid.empty() ||
            (sample < sensors.sample_temp_valid.size() &&
             sensors.sample_temp_valid[sample]));
  };

  // Per-channel over-temp latch. Rev C fallback is a floor
  // controller but we still cut off any heated channel that pegs the RTD).
  for (std::size_t i = 0; i < heater_count; ++i) {
    const std::size_t sample = sensor_channel(i);
    if (temp_valid_for(i) &&
        sensors.sample_temps_c[sample] >
            config_.heater_safety.max_sample_temp_c) {
      channel_latched_[i] = true;
    }
  }
  overtemp_latched_ = std::any_of(channel_latched_.begin(), channel_latched_.end(),
                                  [](bool v) { return v; });

  // Uniformity monitor: flag spread > tolerance during any heating phase.
  // Only the 6 heated samples (index < heater_count) participate.
  uniformity_ok_ = true;
  if (ShouldHeatPhase(phase) && !sensors.sample_temps_c.empty() && heater_count > 0) {
    std::vector<double> valid_temperatures;
    for (std::size_t i = 0; i < heater_count; ++i) {
      if (temp_valid_for(i)) {
        valid_temperatures.push_back(
            sensors.sample_temps_c[sensor_channel(i)]);
      }
    }
    if (valid_temperatures.size() > 1) {
      const auto [lo, hi] =
          std::minmax_element(valid_temperatures.begin(),
                              valid_temperatures.end());
      if ((*hi - *lo) > config_.phase.uniformity_tolerance_c) {
        uniformity_ok_ = false;
      }
    }
  }

  const bool has_direct_override =
      overrides.all_heaters_override.has_value() ||
      overrides.single_heater_override.has_value() ||
      std::any_of(overrides.heater_duty_overrides.begin(),
                  overrides.heater_duty_overrides.end(),
                  [](const std::optional<double>& v) { return v.has_value(); });
  const bool has_temp_targets =
      std::any_of(overrides.temp_targets_c.begin(), overrides.temp_targets_c.end(),
                  [](const std::optional<double>& v) { return v.has_value(); });

  if (overrides.heaters_off) {
    std::fill(sample_heating_.begin(), sample_heating_.end(), false);
    std::fill(active_targets_c_.begin(), active_targets_c_.end(), std::nullopt);
    return duty;
  }

  if (overrides.pid_override.has_value()) {
    for (auto& pid : sample_pids_) {
      pid.SetGains(overrides.pid_override.value());
    }
  }
  for (std::size_t i = 0;
       i < sample_pids_.size() && i < overrides.pid_overrides.size(); ++i) {
    if (overrides.pid_overrides[i].has_value()) {
      sample_pids_[i].SetGains(overrides.pid_overrides[i].value());
    }
  }

  active_targets_c_.assign(heater_count, std::nullopt);

  if (has_temp_targets ||
      (overrides.floor_control_enabled && ShouldHeatPhase(phase))) {
    for (std::size_t i = 0; i < sample_pids_.size() && i < heater_count; ++i) {
      const bool temp_valid = temp_valid_for(i);
      if (!temp_valid) {
        sample_heating_[i] = false;
        sample_pids_[i].Reset();
        duty[i] = 0.0;
        continue;
      }

      const bool explicit_target =
          i < overrides.temp_targets_c.size() &&
          overrides.temp_targets_c[i].has_value();
      if (!explicit_target &&
          (!overrides.floor_control_enabled || !ShouldHeatPhase(phase))) {
        sample_heating_[i] = false;
        continue;
      }

      const double target = explicit_target ? overrides.temp_targets_c[i].value()
                                            : config_.phase.sample_floor_c;
      const double measured =
          sensors.sample_temps_c[sensor_channel(i)];
      active_targets_c_[i] = target;

      if (explicit_target) {
        if (measured >= target) {
          sample_heating_[i] = false;
          sample_pids_[i].Reset();
          duty[i] = 0.0;
        } else {
          sample_heating_[i] = true;
          duty[i] = sample_pids_[i].Update(target, measured, dt_seconds);
        }
      } else {
        const double on_threshold = target - kFloorHysteresisC;
        const double off_threshold = target;
        if (sample_heating_[i]) {
          if (measured >= off_threshold) {
            sample_heating_[i] = false;
            sample_pids_[i].Reset();
            duty[i] = 0.0;
            continue;
          }
        } else {
          if (measured < on_threshold) {
            sample_heating_[i] = true;
          }
        }
        duty[i] = sample_heating_[i]
                      ? sample_pids_[i].Update(target, measured, dt_seconds)
                      : 0.0;
      }
    }
  } else {
    std::fill(sample_heating_.begin(), sample_heating_.end(), false);
    if (!has_direct_override) {
      return duty;
    }
  }

  if (overrides.all_heaters_override.has_value()) {
    const double v = std::clamp(overrides.all_heaters_override.value(), 0.0, 1.0);
    std::fill(duty.begin(), duty.end(), v);
  }

  if (overrides.single_heater_override.has_value()) {
    const auto [idx, value] = overrides.single_heater_override.value();
    if (idx < duty.size()) {
      duty[idx] = std::clamp(value, 0.0, 1.0);
    }
  }

  for (std::size_t i = 0; i < duty.size() && i < overrides.heater_duty_overrides.size(); ++i) {
    if (overrides.heater_duty_overrides[i].has_value()) {
      duty[i] = std::clamp(overrides.heater_duty_overrides[i].value(), 0.0, 1.0);
    }
  }

  // Enforce the per-channel latch last so no override can re-arm a tripped
  // channel without RESET_CONTROL. Normal control also requires valid sample
  // feedback; bench/debug open-loop duty overrides deliberately skip only
  // that feedback-validity clamp.
  for (std::size_t i = 0; i < heater_count; ++i) {
    const bool temp_valid = temp_valid_for(i);
    if ((!temp_valid && !overrides.bench_open_loop_heaters) ||
        channel_latched_[i]) {
      duty[i] = 0.0;
    }
  }

  return duty;
}

}  // namespace coatheal
