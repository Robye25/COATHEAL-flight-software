#pragma once

#include <chrono>
#include <vector>

#include "coatheal/config.hpp"
#include "coatheal/phase.hpp"

namespace coatheal {

struct StateOverrides {
  bool force_start = false;
  bool force_stop = false;
  bool reset_control = false;
  bool shutdown_safe = false;
  bool secondary_cycle = false;  // retained for compat; no-op in Rev C FSM
  bool fatigue_complete = false;  // Rev C: set by FatigueSequencer when done
};

class StateManager {
 public:
  explicit StateManager(const OnboardConfig& config);

  void Reset();

  MissionPhase Update(double pressure_mbar,
                      const std::vector<double>& sample_temps_c,
                      StateOverrides overrides,
                      std::chrono::steady_clock::time_point now);

  MissionPhase phase() const { return phase_; }

 private:
  OnboardConfig config_;
  MissionPhase phase_ = MissionPhase::kBoot;

  // Debounce counters: how many consecutive ticks the threshold has been met.
  // Reset to 0 when the condition is not met.
  int debounce_pre_float_ = 0;
  int debounce_descent_ = 0;
  int debounce_landed_ = 0;
};

}  // namespace coatheal

