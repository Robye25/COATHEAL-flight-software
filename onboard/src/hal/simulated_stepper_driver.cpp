#include "coatheal/hal/stepper_driver.hpp"

namespace coatheal {

SimulatedStepperDriver::SimulatedStepperDriver() = default;

bool SimulatedStepperDriver::Enable(bool enable) {
  enabled_ = enable;
  return true;
}

bool SimulatedStepperDriver::Step(bool direction_forward) {
  if (!enabled_) {
    return false;
  }
  last_dir_ = direction_forward;
  ++pulses_;
  return true;
}

void SimulatedStepperDriver::SetMicrostep(int divisor) {
  if (divisor > 0) {
    microstep_ = divisor;
  }
}

}  // namespace coatheal
