#include "coatheal/hal/spi_bus_lock.hpp"

#include <map>

namespace coatheal {

std::mutex& SpiBusMutex(const std::string& spi_device) {
  static std::mutex registry_mu;
  static std::map<std::string, std::mutex> locks;
  std::lock_guard<std::mutex> lock(registry_mu);
  return locks[spi_device];
}

}  // namespace coatheal
