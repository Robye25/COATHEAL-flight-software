#include "coatheal/heater_scheduler.hpp"

#include <algorithm>
#include <iostream>
#include <numeric>
#include <utility>
#include <vector>

#include "coatheal/motion_lock.hpp"

namespace coatheal {

HeaterScheduler::HeaterScheduler(PowerConfig power, std::size_t electronics_heater_index)
    : power_(power), electronics_heater_index_(electronics_heater_index) {}

HeaterScheduler::HeaterScheduler(PowerConfig power,
                                 std::size_t electronics_heater_index,
                                 MotionLock* lock)
    : power_(power),
      electronics_heater_index_(electronics_heater_index),
      lock_(lock) {}

void HeaterScheduler::Reset() {
  energy_consumed_wh_ = 0.0;
  budget_exhausted_ = false;
  last_inhibited_ = false;
}

std::vector<double> HeaterScheduler::Schedule(const std::vector<double>& requested,
                                              bool deprioritize_electronics) {
  return Schedule(requested, deprioritize_electronics, 0.0);
}

std::vector<double> HeaterScheduler::Schedule(const std::vector<double>& requested,
                                              bool deprioritize_electronics,
                                              double dt_seconds) {
  // === SAFETY INTERLOCK (REV B) ===
  // Heater↔motor mutex. If any stepper motor holds the lock (pull cycle in
  // progress) every heater duty MUST be zero. There is no "but only clamp
  // to 20 %" branch — anything non-zero here is a safety violation.
  const bool motion_active = (lock_ != nullptr) && lock_->is_active();
  if (motion_active) {
    if (!last_inhibited_) {
      std::cerr << "[safety] heater scheduler INHIBITED — motion lock held by "
                << "motor " << lock_->holder() << "; forcing all duties to 0\n";
    }
    last_inhibited_ = true;
    return std::vector<double>(requested.size(), 0.0);
  }
  if (last_inhibited_) {
    std::cerr << "[safety] heater scheduler RESUMED — motion lock released\n";
  }
  last_inhibited_ = false;

  // Once the energy budget is exhausted, latch all heaters off for the rest
  // of the mission. The team's BEXUS power allocation (User Manual §5.2) is
  // a hard cap; we never re-enable heaters until Reset() is called.
  if (budget_exhausted_) {
    return std::vector<double>(requested.size(), 0.0);
  }

  std::vector<double> clamped = requested;
  for (double& value : clamped) {
    value = std::clamp(value, 0.0, 1.0);
  }

  std::vector<std::pair<std::size_t, double>> ranked;
  ranked.reserve(clamped.size());
  for (std::size_t i = 0; i < clamped.size(); ++i) {
    double score = clamped[i];
    if (deprioritize_electronics && i == electronics_heater_index_) {
      score *= 0.1;
    }
    ranked.push_back({i, score});
  }

  std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
    return a.second > b.second;
  });

  std::vector<double> scheduled(clamped.size(), 0.0);
  std::size_t enabled = 0;
  for (const auto& entry : ranked) {
    if (enabled >= power_.max_active_heaters) {
      break;
    }
    const std::size_t idx = entry.first;
    if (clamped[idx] <= 0.0) {
      continue;
    }
    scheduled[idx] = clamped[idx];
    ++enabled;
  }

  double thermal_power = 0.0;
  for (double duty : scheduled) {
    thermal_power += duty * power_.heater_nominal_w;
  }

  if (thermal_power > power_.max_thermal_w && thermal_power > 1e-9) {
    const double scale = power_.max_thermal_w / thermal_power;
    for (double& duty : scheduled) {
      duty *= scale;
    }
    thermal_power = power_.max_thermal_w;
  }

  // Cumulative energy enforcement (BEXUS User Manual §5.2: 150 Wh allocation).
  if (power_.energy_budget_wh > 0.0 && dt_seconds > 0.0) {
    const double tick_energy_wh = thermal_power * (dt_seconds / 3600.0);
    energy_consumed_wh_ += tick_energy_wh;
    if (energy_consumed_wh_ >= power_.energy_budget_wh) {
      budget_exhausted_ = true;
      // Drop this tick's duties as well so we never overshoot the budget.
      std::fill(scheduled.begin(), scheduled.end(), 0.0);
    }
  }

  return scheduled;
}

}  // namespace coatheal
