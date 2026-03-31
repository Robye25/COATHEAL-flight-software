#include "coatheal/thermal_controller.hpp"

#include <algorithm>
#include <numeric>

namespace coatheal {

ThermalController::ThermalController(const OnboardConfig& config)
    : config_(config),
      sample_pid_({config.pid.kp, config.pid.ki, config.pid.kd}, 0.0, 1.0, -10.0, 10.0),
      box_pid_({config.pid.box_kp, config.pid.box_ki, config.pid.box_kd}, 0.0, 1.0, -10.0, 10.0),
      activation_setpoint_c_(config.phase.ascent_target_c) {}

void ThermalController::Reset() {
  sample_pid_.Reset();
  box_pid_.Reset();
  activation_setpoint_c_ = config_.phase.ascent_target_c;
}

void ThermalController::UpdatePid(PidGains gains) {
  sample_pid_.SetGains(gains);
}

double ThermalController::ComputeSampleSetpoint(MissionPhase phase, double dt_seconds) {
  switch (phase) {
    case MissionPhase::kAscentHold:
      activation_setpoint_c_ = config_.phase.ascent_target_c;
      return config_.phase.ascent_target_c;
    case MissionPhase::kActivationRamp: {
      activation_setpoint_c_ += config_.phase.activation_ramp_c_per_s * dt_seconds;
      activation_setpoint_c_ = std::min(activation_setpoint_c_, config_.phase.activation_target_c);
      return activation_setpoint_c_;
    }
    case MissionPhase::kFloatHold:
      activation_setpoint_c_ = config_.phase.float_target_c;
      return config_.phase.float_target_c;
    case MissionPhase::kDescentFloor:
      activation_setpoint_c_ = config_.phase.descent_floor_c;
      return config_.phase.descent_floor_c;
    case MissionPhase::kStopped:
      return config_.phase.descent_floor_c;
  }
  return config_.phase.descent_floor_c;
}

std::vector<double> ThermalController::ComputeRequestedDuty(
    MissionPhase phase,
    const SensorSnapshot& sensors,
    double dt_seconds,
    const ControlOverrides& overrides) {
  const std::size_t heater_count = config_.hardware.heater_count;
  std::vector<double> duty(heater_count, 0.0);

  if (overrides.heaters_off || phase == MissionPhase::kStopped) {
    return duty;
  }

  const double sample_setpoint = ComputeSampleSetpoint(phase, dt_seconds);
  double avg_sample_temp = sensors.box_temp_c;
  if (!sensors.sample_temps_c.empty()) {
    const double sum = std::accumulate(sensors.sample_temps_c.begin(), sensors.sample_temps_c.end(), 0.0);
    avg_sample_temp = sum / static_cast<double>(sensors.sample_temps_c.size());
  }

  const double sample_duty = sample_pid_.Update(sample_setpoint, avg_sample_temp, dt_seconds);
  const double electronics_duty = box_pid_.Update(config_.phase.box_target_c, sensors.box_temp_c, dt_seconds);

  for (std::size_t i = 0; i < heater_count; ++i) {
    duty[i] = sample_duty;
  }

  if (config_.hardware.electronics_heater_index < duty.size()) {
    duty[config_.hardware.electronics_heater_index] = electronics_duty;
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
    sample_pid_.SetGains(overrides.pid_override.value());
  }

  return duty;
}

}  // namespace coatheal