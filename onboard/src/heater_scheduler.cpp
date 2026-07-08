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
    scratch_scheduled_.assign(requested.size(), 0.0);
    return scratch_scheduled_;
  }
  if (last_inhibited_) {
    std::cerr << "[safety] heater scheduler RESUMED — motion lock released\n";
  }
  last_inhibited_ = false;

  // Once the energy budget is exhausted, latch all heaters off for the rest
  // of the mission. The team's BEXUS power allocation (User Manual §5.2) is
  // a hard cap; we never re-enable heaters until Reset() is called.
  if (budget_exhausted_) {
    scratch_scheduled_.assign(requested.size(), 0.0);
    return scratch_scheduled_;
  }

  // Perf: reuse scratch buffers across ticks. First call sizes them; after
  // that we only memcpy/fill into existing storage, no heap allocation.
  scratch_clamped_.assign(requested.begin(), requested.end());
  for (double& value : scratch_clamped_) {
    value = std::clamp(value, 0.0, 1.0);
  }

  // When `electronics_heater_index_ == SIZE_MAX` there is no
  // electronics-box heater in the system, so the `deprioritize_electronics`
  // flag has no effect. The scheduler still accepts the argument for ABI
  // stability; it simply falls through as a no-op in that branch.
  const bool have_electronics_heater =
      (electronics_heater_index_ != static_cast<std::size_t>(-1));
  scratch_ranked_.clear();
  if (scratch_ranked_.capacity() < scratch_clamped_.size()) {
    scratch_ranked_.reserve(scratch_clamped_.size());
  }
  for (std::size_t i = 0; i < scratch_clamped_.size(); ++i) {
    double score = scratch_clamped_[i];
    if (deprioritize_electronics && have_electronics_heater &&
        i == electronics_heater_index_) {
      score *= 0.1;
    }
    scratch_ranked_.push_back({i, score});
  }

  std::sort(scratch_ranked_.begin(), scratch_ranked_.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  scratch_scheduled_.assign(scratch_clamped_.size(), 0.0);
  std::size_t enabled = 0;
  for (const auto& entry : scratch_ranked_) {
    if (enabled >= power_.max_active_heaters) {
      break;
    }
    const std::size_t idx = entry.first;
    if (scratch_clamped_[idx] <= 0.0) {
      continue;
    }
    scratch_scheduled_[idx] = scratch_clamped_[idx];
    ++enabled;
  }

  double thermal_power = 0.0;
  for (double duty : scratch_scheduled_) {
    thermal_power += duty * power_.heater_nominal_w;
  }

  if (thermal_power > power_.max_thermal_w && thermal_power > 1e-9) {
    const double scale = power_.max_thermal_w / thermal_power;
    for (double& duty : scratch_scheduled_) {
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
      std::fill(scratch_scheduled_.begin(), scratch_scheduled_.end(), 0.0);
    }
  }

  // Return by value — callers treat this as owned data. With NRVO the
  // compiler typically elides the copy; the caller (system_controller)
  // assigns it to `last_heater_duty_` which reuses its own capacity.
  return scratch_scheduled_;
}

}  // namespace coatheal
