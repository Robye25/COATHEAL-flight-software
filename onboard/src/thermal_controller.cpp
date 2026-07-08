#include "coatheal/thermal_controller.hpp"

#include <algorithm>

#include "coatheal/telemetry.hpp"  // full definition of SensorSnapshot

namespace coatheal {

ThermalController::ThermalController(const OnboardConfig& config)
    : config_(config),
      channel_latched_(config.hardware.heater_count, false),
      sample_heating_(config.hardware.heater_count, false) {
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
  overtemp_latched_ = false;
  uniformity_ok_ = true;
}

void ThermalController::UpdatePid(PidGains gains) {
  for (auto& pid : sample_pids_) {
    pid.SetGains(gains);
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
  std::vector<double> duty(heater_count, 0.0);

  // Per-channel over-temp latch. Rev C fallback is a floor
  // controller but we still cut off any heated channel that pegs the RTD).
  for (std::size_t i = 0; i < heater_count; ++i) {
    if (i < sensors.sample_temps_c.size() &&
        sensors.sample_temps_c[i] > config_.heater_safety.max_sample_temp_c) {
      channel_latched_[i] = true;
    }
  }
  overtemp_latched_ = std::any_of(channel_latched_.begin(), channel_latched_.end(),
                                  [](bool v) { return v; });

  // Uniformity monitor: flag spread > tolerance during any heating phase.
  // Only the 6 heated samples (index < heater_count) participate.
  uniformity_ok_ = true;
  if (ShouldHeatPhase(phase) && !sensors.sample_temps_c.empty() && heater_count > 0) {
    const std::size_t end = std::min(heater_count, sensors.sample_temps_c.size());
    if (end > 0) {
      const auto begin_it = sensors.sample_temps_c.begin();
      const auto end_it = begin_it + static_cast<std::ptrdiff_t>(end);
      const auto [lo, hi] = std::minmax_element(begin_it, end_it);
      if ((*hi - *lo) > config_.phase.uniformity_tolerance_c) {
        uniformity_ok_ = false;
      }
    }
  }

  const bool has_direct_override =
      overrides.all_heaters_override.has_value() ||
      overrides.single_heater_override.has_value();

  if (overrides.heaters_off) {
    std::fill(sample_heating_.begin(), sample_heating_.end(), false);
    return duty;
  }

  if (overrides.floor_control_enabled && ShouldHeatPhase(phase)) {
    const double floor_c = config_.phase.sample_floor_c;
    const double on_threshold = floor_c - kFloorHysteresisC;  // below -> heat
    const double off_threshold = floor_c;                      // at/above -> off

    // Per-sample floor PID with hysteresis. The PID output is only applied
    // (and the integrator only accumulates) while the sample is below the
    // on_threshold; once it reaches off_threshold we freeze the controller.
    // heater[i] drives sample[i]; there is no box heater slot.
    for (std::size_t i = 0; i < sample_pids_.size() && i < heater_count; ++i) {
      const double measured = (i < sensors.sample_temps_c.size())
                                  ? sensors.sample_temps_c[i]
                                  : floor_c;  // no data -> assume at floor

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

      if (sample_heating_[i]) {
        duty[i] = sample_pids_[i].Update(floor_c, measured, dt_seconds);
      } else {
        duty[i] = 0.0;
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

  if (overrides.pid_override.has_value()) {
    for (auto& pid : sample_pids_) {
      pid.SetGains(overrides.pid_override.value());
    }
  }

  // Enforce per-channel latch last so no override re-arms a tripped channel
  // without RESET_CONTROL.
  for (std::size_t i = 0; i < heater_count; ++i) {
    if (channel_latched_[i]) {
      duty[i] = 0.0;
    }
  }

  return duty;
}

}  // namespace coatheal
