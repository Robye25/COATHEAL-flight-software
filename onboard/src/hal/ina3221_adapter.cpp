#include "coatheal/hal/ina3221_adapter.hpp"

namespace coatheal {

bool Ina3221Adapter::ReadChannel(std::uint8_t /*addr*/, int /*channel*/,
                                 double* bus_v, double* shunt_v) {
  // Stub: no real I2C transaction yet. Real Modbus/I2C bring-up happens on
  // flight hardware; for now we return zeros and stay healthy.
  if (bus_v != nullptr) *bus_v = 0.0;
  if (shunt_v != nullptr) *shunt_v = 0.0;
  return true;
}

}  // namespace coatheal
