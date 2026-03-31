#pragma once

#include <cstddef>
#include <vector>

#include "coatheal/config.hpp"

namespace coatheal {

class HeaterScheduler {
 public:
  HeaterScheduler(PowerConfig power, std::size_t electronics_heater_index);

  std::vector<double> Schedule(const std::vector<double>& requested,
                               bool deprioritize_electronics) const;

 private:
  PowerConfig power_;
  std::size_t electronics_heater_index_ = 0;
};

}  // namespace coatheal