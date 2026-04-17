#pragma once

#include <cstddef>
#include <cstdint>

namespace coatheal {

// I2C stub for the INA3221 3-channel V/I monitor used in Rev B.1 as a
// sample-resistance instrument (not a power monitor). Real I2C reads are
// TBD; today this returns zeros and tracks a `healthy_` flag. Two chips at
// addresses 0x40 and 0x41 cover channels 1..3 each (6 samples total).
class Ina3221Adapter {
 public:
  static constexpr std::uint8_t kDefaultAddrA = 0x40;
  static constexpr std::uint8_t kDefaultAddrB = 0x41;

  // Returns bus voltage (V) and shunt voltage (V) on [addr, channel]. Writes
  // zero and returns true in the stub; sets healthy_=false on probe failure
  // (which the stub never does). Channel is 1..3 per the INA3221 register map.
  bool ReadChannel(std::uint8_t addr, int channel,
                   double* bus_v, double* shunt_v);
  bool healthy() const { return healthy_; }
  void set_healthy(bool h) { healthy_ = h; }

 private:
  bool healthy_ = true;
};

}  // namespace coatheal
