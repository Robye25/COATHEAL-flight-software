#pragma once

#include <cstddef>
#include <vector>

#include "coatheal/config.hpp"

namespace coatheal {

// HeaterScheduler enforces the BEXUS power constraints from User Manual §5.2:
//   * max_active_heaters    — at most N heaters energised simultaneously
//   * max_thermal_w         — peak combined thermal draw (proportional clamp)
//   * energy_budget_wh      — cumulative heater energy over the mission
//
// The energy budget is optional: when energy_budget_wh <= 0 it is disabled and
// the scheduler behaves like the legacy peak-power-only version. When set, the
// scheduler integrates duty * heater_nominal_w * dt every Schedule() call and
// returns all-zero duty cycles once the budget is exhausted, latching heaters
// off for the remainder of the mission.
class HeaterScheduler {
 public:
  HeaterScheduler(PowerConfig power, std::size_t electronics_heater_index);

  // Schedule with no energy accounting (legacy entry point).
  std::vector<double> Schedule(const std::vector<double>& requested,
                               bool deprioritize_electronics);

  // Schedule with energy accounting. dt_seconds is the integration window for
  // this tick. When dt_seconds <= 0 this is identical to Schedule() above.
  std::vector<double> Schedule(const std::vector<double>& requested,
                               bool deprioritize_electronics,
                               double dt_seconds);

  // Cumulative heater energy delivered so far (Wh). Resets only with Reset().
  double energy_consumed_wh() const { return energy_consumed_wh_; }

  // True if energy_budget_wh > 0 and the budget has been reached. Once true,
  // Schedule() returns all-zero duty cycles for the rest of the mission.
  bool is_budget_exhausted() const { return budget_exhausted_; }

  // Reset cumulative energy and unlatch the budget. Intended for tests.
  void Reset();

 private:
  PowerConfig power_;
  std::size_t electronics_heater_index_ = 0;
  double energy_consumed_wh_ = 0.0;
  bool budget_exhausted_ = false;
};

}  // namespace coatheal
