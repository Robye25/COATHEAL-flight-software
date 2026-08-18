#pragma once

#include <cstddef>
#include <cstdint>

namespace coatheal {

// Historical compatibility stub for the old sample-resistance path. The final
// Rev C BOM disables resistance acquisition with `sensor.resistance_source`.
class Ina3221Adapter {
 public:
  // ADDRESS COLLISION: 0x40 and 0x41 are no longer free. They are now the
  // Sequent RTD HAT's stack-0 and stack-1 addresses (kAddressBase = 0x40 in
  // sequent_rtd_adapter.hpp, address = 0x40 + stack). Nothing here talks to
  // the bus today - ReadChannel is a stub - so the clash is dormant. Do not
  // re-enable a real INA3221 on these addresses: either move the INA3221
  // (A0 strapping gives 0x42/0x43) or move the RTD card's DIP-switch stack,
  // and update both this note and the reciprocal one in
  // sequent_rtd_adapter.hpp.
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
