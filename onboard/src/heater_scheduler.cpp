#include "coatheal/heater_scheduler.hpp"

#include <algorithm>
#include <numeric>
#include <utility>
#include <vector>

namespace coatheal {

HeaterScheduler::HeaterScheduler(PowerConfig power, std::size_t electronics_heater_index)
    : power_(power), electronics_heater_index_(electronics_heater_index) {}

std::vector<double> HeaterScheduler::Schedule(const std::vector<double>& requested,
                                              bool deprioritize_electronics) const {
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
  }

  return scheduled;
}

}  // namespace coatheal