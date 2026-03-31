#include "coatheal/state_manager.hpp"

#include <algorithm>
#include <numeric>

namespace coatheal {

StateManager::StateManager(const OnboardConfig& config) : config_(config) {}

void StateManager::Reset() {
  phase_ = MissionPhase::kAscentHold;
  float_hold_started_valid_ = false;
}

bool StateManager::IsAtTarget(const std::vector<double>& sample_temps_c, double target) const {
  if (sample_temps_c.empty()) {
    return false;
  }
  const double sum = std::accumulate(sample_temps_c.begin(), sample_temps_c.end(), 0.0);
  const double avg = sum / static_cast<double>(sample_temps_c.size());
  return avg >= (target - 1.0);
}

MissionPhase StateManager::Update(double pressure_mbar,
                                  const std::vector<double>& sample_temps_c,
                                  StateOverrides overrides,
                                  std::chrono::steady_clock::time_point now) {
  if (overrides.reset_control) {
    Reset();
  }

  if (overrides.shutdown_safe) {
    phase_ = MissionPhase::kStopped;
    return phase_;
  }

  if (overrides.force_stop) {
    phase_ = MissionPhase::kDescentFloor;
    return phase_;
  }

  if (overrides.force_start && phase_ == MissionPhase::kAscentHold) {
    phase_ = MissionPhase::kActivationRamp;
  }

  switch (phase_) {
    case MissionPhase::kAscentHold:
      if (pressure_mbar <= config_.transition.ascent_to_activation_mbar) {
        phase_ = MissionPhase::kActivationRamp;
      }
      break;

    case MissionPhase::kActivationRamp:
      if (IsAtTarget(sample_temps_c, config_.phase.activation_target_c)) {
        phase_ = MissionPhase::kFloatHold;
        float_hold_started_ = now;
        float_hold_started_valid_ = true;
      }
      break;

    case MissionPhase::kFloatHold:
      if (!float_hold_started_valid_) {
        float_hold_started_ = now;
        float_hold_started_valid_ = true;
      }
      if (pressure_mbar >= config_.transition.float_to_descent_mbar) {
        phase_ = MissionPhase::kDescentFloor;
        break;
      }
      if (float_hold_started_valid_) {
        const auto hold_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            now - float_hold_started_);
        if (hold_seconds.count() >= static_cast<long long>(config_.phase.float_hold_minutes * 60.0)) {
          phase_ = MissionPhase::kDescentFloor;
        }
      }
      break;

    case MissionPhase::kDescentFloor:
      break;

    case MissionPhase::kStopped:
      break;
  }

  return phase_;
}

}  // namespace coatheal