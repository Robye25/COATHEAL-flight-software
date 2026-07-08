#pragma once

#include <cstddef>
#include <cstdint>

namespace coatheal {

// Historical compatibility stub for the old sample-resistance path. The final
// Rev C BOM disables resistance acquisition with `sensor.resistance_source`.
class Ina3221Adapter {
 public:
  static constexpr std::uint8_t kDefaultAddrA = 0x40;
  static constexpr std::uint8_t kDefaultAddrB = 0x41;

  bool ReadChannel(std::uint8_t addr, int channel,
                   double* bus_v, double* shunt_v);
  bool healthy() const { return healthy_; }
  void set_healthy(bool h) { healthy_ = h; }

 private:
  bool healthy_ = true;
};

}  // namespace coatheal
