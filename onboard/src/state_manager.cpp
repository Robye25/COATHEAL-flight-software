#include "coatheal/state_manager.hpp"

namespace coatheal {

StateManager::StateManager(const OnboardConfig& config) : config_(config) {}

void StateManager::Reset() {
  phase_ = MissionPhase::kBoot;
}

MissionPhase StateManager::Update(double pressure_mbar,
                                  const std::vector<double>& /*sample_temps_c*/,
                                  StateOverrides overrides,
                                  std::chrono::steady_clock::time_point /*now*/) {
  if (overrides.reset_control) {
    Reset();
  }

  if (overrides.shutdown_safe) {
    phase_ = MissionPhase::kStopped;
    return phase_;
  }

  if (overrides.force_stop) {
    // FORCE_STOP short-circuits straight to DESCENT so heaters/motors follow
    // the descent policy; a subsequent SHUTDOWN_SAFE takes us to STOPPED.
    phase_ = MissionPhase::kDescent;
    return phase_;
  }

  if (overrides.force_start && phase_ == MissionPhase::kBoot) {
    phase_ = MissionPhase::kAscent;
  }

  switch (phase_) {
    case MissionPhase::kBoot:
      // First tick out of BOOT is immediate; the orchestrator gates this
      // behind SystemMode::kRun so BOOT only persists while in STANDBY.
      phase_ = MissionPhase::kAscent;
      break;

    case MissionPhase::kAscent:
      if (pressure_mbar <= config_.transition.ascent_to_float_mbar) {
        phase_ = MissionPhase::kFloat;
      }
      break;

    case MissionPhase::kFloat:
      if (pressure_mbar >= config_.transition.float_to_descent_mbar) {
        phase_ = MissionPhase::kDescent;
      }
      break;

    case MissionPhase::kDescent:
      if (pressure_mbar >= config_.transition.descent_to_landed_mbar) {
        phase_ = MissionPhase::kLanded;
      }
      break;

    case MissionPhase::kLanded:
    case MissionPhase::kStopped:
      break;
  }

  return phase_;
}

}  // namespace coatheal
