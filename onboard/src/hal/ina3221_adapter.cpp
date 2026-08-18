#include "coatheal/hal/ina3221_adapter.hpp"

namespace coatheal {

bool Ina3221Adapter::ReadChannel(std::uint8_t /*addr*/, int /*channel*/,
                                 double* bus_v, double* shunt_v) {
  // Stub: this adapter never touches the bus. Sample resistance now comes
  // from the Sequent RTD card (see SequentRtdAdapter), and no config value
  // routes here any more; it stays compiled only so a legacy
  // sensor.resistance_source label still links. Returns zeros, stays healthy.
  if (bus_v != nullptr) *bus_v = 0.0;
  if (shunt_v != nullptr) *shunt_v = 0.0;
  return true;
}

}  // namespace coatheal
