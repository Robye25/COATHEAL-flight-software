#include "coatheal/state_manager.hpp"

namespace coatheal {

StateManager::StateManager(const OnboardConfig& config) : config_(config) {}

void StateManager::ResetDebounce() {
  debounce_pre_float_ = 0;
  debounce_float_ = 0;
  debounce_descent_ = 0;
  debounce_landed_ = 0;
}

void StateManager::Reset() {
  phase_ = MissionPhase::kBoot;
  ResetDebounce();
}

void StateManager::SetPhase(MissionPhase phase) {
  phase_ = phase;
  ResetDebounce();
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
    debounce_pre_float_ = 0;
    debounce_descent_ = 0;
    return phase_;
  }

  if (overrides.force_start && phase_ == MissionPhase::kBoot) {
    phase_ = MissionPhase::kAscent;
  }

  const int required = std::max(1, config_.transition.debounce_samples);

  switch (phase_) {
    case MissionPhase::kBoot:
      // First tick out of BOOT is immediate; the orchestrator gates this
      // behind SystemMode::kRun so BOOT only persists while in STANDBY.
      phase_ = MissionPhase::kAscent;
      break;

    case MissionPhase::kAscent:
      // Rev C: transition to PRE_FLOAT when pressure drops below threshold
      // for `debounce_samples` consecutive ticks.
      if (pressure_mbar <= config_.transition.pre_float_mbar) {
        ++debounce_pre_float_;
        if (debounce_pre_float_ >= required) {
          phase_ = MissionPhase::kPreFloat;
          debounce_pre_float_ = 0;
        }
      } else {
        debounce_pre_float_ = 0;
      }
      break;

    case MissionPhase::kPreFloat:
      // Fallback tracking follows pressure only. It never starts motion.
      if (pressure_mbar >= config_.transition.float_to_descent_mbar) {
        ++debounce_descent_;
        if (debounce_descent_ >= required) {
          phase_ = MissionPhase::kDescent;
          debounce_descent_ = 0;
          debounce_float_ = 0;
        }
      } else {
        debounce_descent_ = 0;
        if (pressure_mbar <= config_.transition.ascent_to_float_mbar) {
          ++debounce_float_;
          if (debounce_float_ >= required) {
            phase_ = MissionPhase::kFloat;
            debounce_float_ = 0;
          }
        } else {
          debounce_float_ = 0;
        }
      }
      break;

    case MissionPhase::kFloat:
      if (pressure_mbar >= config_.transition.float_to_descent_mbar) {
        ++debounce_descent_;
        if (debounce_descent_ >= required) {
          phase_ = MissionPhase::kDescent;
          debounce_descent_ = 0;
        }
      } else {
        debounce_descent_ = 0;
      }
      break;

    case MissionPhase::kDescent:
      if (pressure_mbar >= config_.transition.descent_to_landed_mbar) {
        ++debounce_landed_;
        if (debounce_landed_ >= required) {
          phase_ = MissionPhase::kLanded;
          debounce_landed_ = 0;
        }
      } else {
        debounce_landed_ = 0;
      }
      break;

    case MissionPhase::kLanded:
    case MissionPhase::kStopped:
      break;
  }

  return phase_;
}

}  // namespace coatheal

