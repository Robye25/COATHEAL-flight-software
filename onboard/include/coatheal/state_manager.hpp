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
  bool secondary_cycle = false;  // retained for compat; no-op in Rev B FSM
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
};

}  // namespace coatheal
